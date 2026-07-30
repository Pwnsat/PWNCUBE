/*
 * CubeSat — real GPS (u-blox NEO-6M) UART/NMEA driver interface (Etapa 5).
 *
 * This is the REAL receiver, wired via the board's 6-pin GND/SCL/SDA/TX/
 * RX/3V3 header (UART0, GPIO2_B0/B1) — separate from and orthogonal to
 * the GPS_OVERRIDE debug hook (Etapa 3, command_service.c), which stays
 * the only thing that can unlock the GS_ACCESS gate (gs_gps_usable()).
 * A real fix only ever updates the shared position telemetry, and only
 * while GPS_OVERRIDE is not active — see command_service.c's telemetry
 * worker for the precedence rule itself.
 */
#ifndef GPS_NMEA_H
#define GPS_NMEA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <rtthread.h>

/* HAL_UART_MODULE_ENABLED is force-defined unconditionally in this
 * board's hal_conf.h (same treatment already given to HAL_SPI_MODULE_
 * ENABLED for the SX1262 radios) -- RT_USING_UART is deliberately never
 * set, which keeps common/drivers/drv_uart.c (RT-Thread's serial device
 * framework) out of the build entirely. This file drives UART0 with
 * bare HAL_UART_* calls, exactly like sensor_port_rtt.c does for I2C0. */
#include "hal_base.h"

typedef struct {
    bool    uart_ok;    /* UART0 initialized and has received at least one byte */
    bool    fix_valid;  /* last complete $GPGGA/$GNGGA reported fix_quality > 0 */
    int32_t lat_e7;     /* degrees * 1e7, same fixed-point as GPS_OVERRIDE/FlatSat */
    int32_t lon_e7;
    int32_t alt_cm;
    uint8_t sats;
    /* Diagnostics (added 2026-07-30, field debugging real hardware with no
     * fix): total_bytes_rx proves sustained data flow (vs. a single stray
     * byte), and last_line is the most recently completed line EXACTLY as
     * received -- captured before nmea_process_sentence() mutates it via
     * strtok_r, and regardless of whether it matched $GPGGA/$GNGGA -- so a
     * human can tell "genuine NMEA text, just no fix yet" apart from
     * "garbage/wrong baud" apart from "nothing coming through at all". */
    uint32_t total_bytes_rx;
    uint32_t total_bytes_tx;  /* sum of HAL_UART_SerialOut()'s own return values --
                                 proves the TX path is actually accepted by the
                                 HAL/hardware, separate from whether RX ever sees it */
    char     last_line[24];
} gps_nmea_fix_t;

/* Bring up UART0 (clock ungate + iomux + HAL_UART_Init @ 9600 8N1, the
 * NEO-6M's factory-default NMEA rate). Idempotent. 0 on success, <0 on error. */
int gps_nmea_init(void);

/* Non-blocking: drains whatever bytes are currently sitting in UART0's RX
 * FIFO, feeds them into the internal NMEA line parser, and writes the
 * latest known fix state into *out (if non-NULL). Cheap/safe to call
 * every tick — never blocks, bounded internal spin budget. */
void gps_nmea_poll(gps_nmea_fix_t *out);

#endif /* GPS_NMEA_H */
