/*
 * CubeSat — HAL_UART glue + minimal NMEA parser for a real u-blox NEO-6M
 * GPS (Etapa 5), mirroring sensor_port_rtt.c's pattern: the RV1106 MCU
 * BSP never wired RT_USING_UART0/drv_uart, so this drives HAL_UART
 * directly in poll mode with the ready-made g_uart0Dev clock/pin facts,
 * no RT-Thread serial device framework.
 *
 * Wiring (the board's 6-pin GND/SCL/SDA/TX/RX/3V3 header): the SCL/RX and
 * SDA/TX pairs share the same two SoC pads (GPIO2_B0/GPIO2_B1), alt-muxed
 * between I2C1 and UART0 — confirmed free (both controllers status=
 * "disabled" in the Linux device tree, no overlap with the two SX1262
 * radios, the BME280/ICM42670 I2C0 bus, or the IPC camera's I2C4). GND->
 * GND, 3V3->3V3, NEO-6M TX->header RX(=GPIO2_B0/UART0_RX), NEO-6M RX->
 * header TX(=GPIO2_B1/UART0_TX, only needed to ever configure the module,
 * not for reading NMEA). 9600 8N1, the NEO-6M's factory default.
 *
 * Only $GPGGA/$GNGGA is parsed — deliberately, it is the one NMEA
 * sentence that already carries everything this driver needs (lat, lon,
 * altitude, satellite count, fix quality) without cross-referencing a
 * second sentence like $GPRMC. No checksum verification, matching this
 * firmware's existing "errors are never checked" stance elsewhere.
 *
 * Precedence vs. GPS_OVERRIDE lives in command_service.c, not here: this
 * file only ever reports what the module is actually seeing.
 */
#include "gps_nmea.h"
#include <stdlib.h>

/* g_uart0Dev is defined unconditionally in hal_bsp.c (guarded only by
 * HAL_UART_MODULE_ENABLED, which hal_conf.h always defines) — declared
 * locally here rather than pulling in hal_bsp.h, whose include path is
 * not wired into this applications/ build group. */
extern const struct HAL_UART_DEV g_uart0Dev;

#define GPS_UART_RX_BUDGET   256U   /* bytes drained per poll() call, max */
#define GPS_LINE_MAX         96U    /* longest sentence we care about + margin */

static bool           s_uart0_ready;
static char            s_line[GPS_LINE_MAX];
static uint32_t         s_line_len;
static gps_nmea_fix_t   s_fix;

/* ddmm.mmmm (lat) or dddmm.mmmm (lon) + hemisphere -> degrees * 1e7, signed. */
static int32_t nmea_parse_coord(const char *field, char hemi, bool is_lon)
{
    int deg_digits = is_lon ? 3 : 2;
    char deg_buf[4] = {0};
    int i;
    double degrees, minutes, val;

    if (field == NULL || field[0] == '\0')
        return 0;

    for (i = 0; i < deg_digits && field[i] >= '0' && field[i] <= '9'; i++)
        deg_buf[i] = field[i];
    deg_buf[i] = '\0';
    if (i == 0)
        return 0;

    degrees = atof(deg_buf);
    minutes = atof(field + i);
    val = degrees + (minutes / 60.0);

    if (hemi == 'S' || hemi == 'W')
        val = -val;

    return (int32_t)(val * 10000000.0);
}

/* Parses one already NUL-terminated, newline-stripped NMEA line in place
 * (strtok_r mutates it). Updates s_fix only for sentences it recognizes. */
static void nmea_process_sentence(char *line)
{
    char *save = NULL;
    char *tok;
    int field;
    char lat_field[16] = {0};
    char lat_hemi = 0;
    char lon_field[16] = {0};
    char lon_hemi = 0;
    int fix_quality = -1;
    int sats = 0;
    double alt_m = 0.0;
    bool have_alt = false;

    if (strncmp(line, "$GPGGA", 6) != 0 && strncmp(line, "$GNGGA", 6) != 0)
        return; /* only GGA carries lat/lon/alt/sats/fix-quality in one line */

    field = 0;
    tok = strtok_r(line, ",*", &save);
    while (tok != NULL) {
        switch (field) {
        case 2: strncpy(lat_field, tok, sizeof(lat_field) - 1); break;
        case 3: lat_hemi = tok[0]; break;
        case 4: strncpy(lon_field, tok, sizeof(lon_field) - 1); break;
        case 5: lon_hemi = tok[0]; break;
        case 6: fix_quality = atoi(tok); break;
        case 7: sats = atoi(tok); break;
        case 9: alt_m = atof(tok); have_alt = true; break;
        default: break;
        }
        tok = strtok_r(NULL, ",*", &save);
        field++;
    }

    if (fix_quality < 0)
        return; /* malformed/short line -- leave last-known fix untouched */

    s_fix.fix_valid = (fix_quality > 0);
    s_fix.sats = (uint8_t)sats;
    if (s_fix.fix_valid) {
        s_fix.lat_e7 = nmea_parse_coord(lat_field, lat_hemi, false);
        s_fix.lon_e7 = nmea_parse_coord(lon_field, lon_hemi, true);
        if (have_alt)
            s_fix.alt_cm = (int32_t)(alt_m * 100.0);
    }
}

int gps_nmea_init(void)
{
    struct HAL_UART_CONFIG cfg;

    if (s_uart0_ready)
        return 0;

    /* UART0 lives in PERICRU like I2C0 -- ungate pclk + functional clk
     * BEFORE HAL_UART_Init touches any register (same lesson as I2C0's
     * own init: touching registers pre-ungate bus-stalls the SCR1 with
     * no trace). */
    HAL_CRU_ClkEnable(PCLK_UART0_GATE);
    HAL_CRU_ClkEnable(SCLK_UART0_GATE);

    /* uart0m1 pinmux (the board's GND/SCL/SDA/TX/RX/3V3 header):
     * RX=GPIO2_B0, TX=GPIO2_B1, func1 (dts/rv1106-pinctrl.dtsi's
     * uart0m1_xfer group — confirmed unused by anything else). */
    HAL_PINCTRL_SetIOMUX(GPIO_BANK2, GPIO_PIN_B0 | GPIO_PIN_B1,
                         PIN_CONFIG_MUX_FUNC1);

    cfg.baudRate = UART_BR_9600;
    cfg.dataBit  = UART_DATA_8B;
    cfg.stopBit  = UART_ONE_STOPBIT;
    cfg.parity   = UART_PARITY_DISABLE;

    if (HAL_UART_Init(&g_uart0Dev, &cfg) != HAL_OK)
        return -1;

    memset(&s_fix, 0, sizeof(s_fix));
    s_line_len = 0;
    s_uart0_ready = true;
    return 0;
}

void gps_nmea_poll(gps_nmea_fix_t *out)
{
    uint8_t byte;
    uint32_t budget = GPS_UART_RX_BUDGET;

    /* Self-initializing, like sensor_lazy_i2c() does for the BME280/
     * ICM42670 pair — command_service.c's telemetry worker only ever
     * needs to call gps_nmea_poll(), no separate init wiring required.
     * gps_nmea_init() is idempotent and cheap once ready (single bool
     * check), so retrying it every tick before it succeeds is harmless. */
    if (!s_uart0_ready)
        (void)gps_nmea_init();

    if (!s_uart0_ready) {
        if (out) *out = s_fix;
        return;
    }

    /* Loopback self-test (added 2026-07-30, field debugging a board that
     * never receives anything from an external NEO-6M in any wiring
     * combination tried). Transmits a fixed line out of UART0_TX on a
     * ~1s cadence (gated by poll-call count, since this function runs
     * every ~2ms from the main loop) -- with a physical jumper wire
     * bridging the header's TX pin straight to its RX pin (module fully
     * disconnected), this string should come back and show up as
     * last_line via gps_raw. Proves pin/pinmux/clock/HAL/RX-parsing all
     * work end-to-end, independent of the GPS module or its wiring. */
    {
        static uint32_t s_tx_test_tick;
        if (++s_tx_test_tick >= 500U) {
            s_tx_test_tick = 0;
            static const uint8_t test_line[] = "LOOPTEST\n";
            int sent = HAL_UART_SerialOut(g_uart0Dev.pReg, test_line, sizeof(test_line) - 1);
            if (sent > 0)
                s_fix.total_bytes_tx += (uint32_t)sent;
        }
    }

    while (budget-- && HAL_UART_SerialIn(g_uart0Dev.pReg, &byte, 1) == 1) {
        s_fix.uart_ok = true; /* at least one byte ever seen on the wire */
        s_fix.total_bytes_rx++;

        if (byte == '\n') {
            if (s_line_len > 0) {
                s_line[s_line_len < GPS_LINE_MAX ? s_line_len : GPS_LINE_MAX - 1] = '\0';
                /* Snapshot BEFORE nmea_process_sentence() mutates the buffer
                 * (strtok_r) -- captures the line exactly as received,
                 * whether or not it matched $GPGGA/$GNGGA. Diagnostic only
                 * (added 2026-07-30): tells apart "real NMEA text, just no
                 * fix yet" from "garbage / wrong baud" from "nothing at all". */
                strncpy(s_fix.last_line, s_line, sizeof(s_fix.last_line) - 1);
                s_fix.last_line[sizeof(s_fix.last_line) - 1] = '\0';
                nmea_process_sentence(s_line);
            }
            s_line_len = 0;
        } else if (byte != '\r') {
            if (s_line_len < GPS_LINE_MAX - 1)
                s_line[s_line_len++] = (char)byte;
            /* else: line too long -- keep truncating silently until the
             * next '\n', matching this firmware's "errors never checked"
             * stance rather than adding new failure handling. */
        }
    }

    /* Fallback for the pathological case where bytes are flowing but no
     * '\n' has ever completed a line (e.g. wrong baud rate garbling the
     * framing so badly 0x0A never lands where expected) -- surface
     * whatever is sitting in the in-progress buffer instead of leaving
     * last_line empty forever, so this diagnostic still shows something. */
    if (s_fix.last_line[0] == '\0' && s_line_len > 0) {
        uint32_t n = s_line_len < sizeof(s_fix.last_line) - 1
                     ? s_line_len : sizeof(s_fix.last_line) - 1;
        memcpy(s_fix.last_line, s_line, n);
        s_fix.last_line[n] = '\0';
    }

    if (out) *out = s_fix;
}
