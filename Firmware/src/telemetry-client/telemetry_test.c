/*
 * CubeSat — TelemetryService client + monitor (TX/RX packet sniffing).
 *
 * El servicio de telemetria corre en el MCU RISC-V (ept 0x4007
 * "rpmsg-telemetry"). La radio fisica es controlada por RadioService
 * (ept 0x4005 "rpmsg-radio").
 *
 * Esta herramienta puede abrir ambos endpoints simultaneamente para
 * monitorizar TX y/o RX.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

struct rpmsg_endpoint_info {
    char name[32];
    uint32_t src;
    uint32_t dst;
};
#define RPMSG_CREATE_EPT_IOCTL  _IOW(0xb5, 0x1, struct rpmsg_endpoint_info)
#define RPMSG_DESTROY_EPT_IOCTL _IO(0xb5, 0x2)

#define TELEM_DST      0x4007
#define RADIO_DST      0x4005
#define EVT_TX         0xE2
#define EVT_RX         0xE0
#define EVT_RX_TIMEOUT 0xE1

static uint32_t g_src;

/* ------------------------------------------------------------------ */
/*  RPMSG helpers                                                     */
/* ------------------------------------------------------------------ */

static long ept_src(int n)
{
    char lp[80], val[32];
    int f, r;
    snprintf(lp, sizeof(lp), "/sys/class/rpmsg/rpmsg%d/src", n);
    f = open(lp, O_RDONLY);
    if (f < 0) return -1;
    r = read(f, val, sizeof(val) - 1);
    close(f);
    if (r <= 0) return -1;
    val[r] = '\0';
    return strtol(val, NULL, 10);
}

static int open_channel(const char *name, uint32_t dst)
{
    struct rpmsg_endpoint_info ept;
    char lp[96], target[256], np[64], cp[64];
    ssize_t rr;
    int ctrl, cfd, t, n, fd;

    memset(&ept, 0, sizeof(ept));
    snprintf(ept.name, sizeof(ept.name), "%s", name);
    ept.dst = dst;

    for (ctrl = 0; ctrl < 8; ctrl++) {
        snprintf(lp, sizeof(lp), "/sys/class/rpmsg/rpmsg_ctrl%d/device", ctrl);
        rr = readlink(lp, target, sizeof(target) - 1);
        if (rr > 0) {
            target[rr] = '\0';
            if (strstr(target, name))
                goto found;
        }
    }
    fprintf(stderr, "error: no encuentro el canal %s (arranco el MCU? rcS lo ligo?).\n"
                    "Revisa: ls /sys/bus/rpmsg/devices/\n", name);
    return -1;

found:
    g_src = 0x5000 + (uint32_t)(getpid() & 0x0FFF);
    ept.src = g_src;

    snprintf(cp, sizeof(cp), "/dev/rpmsg_ctrl%d", ctrl);
    cfd = open(cp, O_RDWR);
    if (cfd < 0) { perror(cp); return -1; }
    if (ioctl(cfd, RPMSG_CREATE_EPT_IOCTL, &ept) < 0) { perror("create ept"); close(cfd); return -1; }
    close(cfd);

    for (t = 0; t < 50; t++) {
        for (n = 0; n < 32; n++) {
            if (ept_src(n) == (long)g_src) {
                snprintf(np, sizeof(np), "/dev/rpmsg%d", n);
                fd = open(np, O_RDWR);
                if (fd >= 0)
                    return fd;
            }
        }
        usleep(20000);
    }
    fprintf(stderr, "error: no aparecio el endpoint /dev/rpmsgN para %s.\n", name);
    return -1;
}

static void close_ept(int fd)
{
    if (fd >= 0) {
        (void)ioctl(fd, RPMSG_DESTROY_EPT_IOCTL, 0);
        close(fd);
    }
}

static int xfer(int fd, const uint8_t *req, int req_len,
                uint8_t *rsp, int rsp_max, int tmo_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int n;
    if (write(fd, req, req_len) != req_len) { perror("write"); return -1; }
    n = poll(&pfd, 1, tmo_ms);
    if (n <= 0) return n;
    return read(fd, rsp, rsp_max);
}

static int need_arg(int have, const char *what)
{
    if (have) return 0;
    fprintf(stderr, "error: falta <%s>.\n", what);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Packet decoder  (FlatSat SPP — no sec hdr, no CRC, LE payload)     */
/* ------------------------------------------------------------------ */

static const char *apid_name(uint16_t apid)
{
    switch (apid) {
    case 0x008: return "SEND_TM";
    case 0x001: return "PING/SYNC";
    case 0x7FF: return "IDLE";
    default:    return "UNKNOWN";
    }
}

static void print_packet(const char *prefix, const uint8_t *buf, int len)
{
    int i;
    printf("%s | %d bytes\n", prefix, len);
    printf("%s | hex: ", prefix);
    for (i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");

    if (len < 6) { printf("%s | (too short)\n", prefix); return; }

    uint16_t pkt_id = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t apid   = pkt_id & 0x07FF;
    uint16_t seq    = ((uint16_t)buf[2] << 8) | buf[3];
    uint16_t dlen   = ((uint16_t)buf[4] << 8) | buf[5];
    int      flen   = dlen + 1 + 6;

    printf("%s | APID=0x%03x (%s) seq=0x%04x data_len=%d frame_len=%d\n",
           prefix, apid, apid_name(apid), seq, dlen, flen);

    /* No secondary header; payload is immediately after 6-byte primary header. */
    const uint8_t *pl = buf + 6;
    int plen = len - 6;
    if (plen < 1) return;

    if (apid == 0x008 && plen >= 20) {
        /* LE int16 x100 fields */
        int16_t ax     = (int16_t)((uint16_t)pl[1]  | ((uint16_t)pl[2]  << 8));
        int16_t ay     = (int16_t)((uint16_t)pl[3]  | ((uint16_t)pl[4]  << 8));
        int16_t az     = (int16_t)((uint16_t)pl[5]  | ((uint16_t)pl[6]  << 8));
        int16_t acc_t  = (int16_t)((uint16_t)pl[7]  | ((uint16_t)pl[8]  << 8));
        int16_t bme_t  = (int16_t)((uint16_t)pl[9]  | ((uint16_t)pl[10] << 8));
        int16_t press  = (int16_t)((uint16_t)pl[11] | ((uint16_t)pl[12] << 8));
        int16_t alt    = (int16_t)((uint16_t)pl[13] | ((uint16_t)pl[14] << 8));
        int16_t hum    = (int16_t)((uint16_t)pl[15] | ((uint16_t)pl[16] << 8));
        uint8_t  thr0  = pl[17];
        uint8_t  thr1  = pl[18];
        printf("%s | sens: sc_id=0x%02x accel=%d,%d,%d "
               "accT=%d.%d C bmeT=%d.%d C press=%d.%d hPa "
               "alt=%d.%d m hum=%d.%d %% thr=%d/%d\n",
               prefix, pl[0],
               ax, ay, az,
               acc_t / 100, (acc_t < 0 ? -acc_t : acc_t) % 100,
               bme_t / 100, (bme_t < 0 ? -bme_t : bme_t) % 100,
               press / 100, (press < 0 ? -press : press) % 100,
               alt   / 100, (alt   < 0 ? -alt   : alt)   % 100,
               hum   / 100, (hum   < 0 ? -hum   : hum)   % 100,
               thr0, thr1);
    }
    else if (apid == 0x001 && plen >= 8) {
        char msg[9];
        memcpy(msg, pl + 1, plen > 8 ? 8 : plen - 1);
        msg[plen > 8 ? 8 : plen - 1] = '\0';
        printf("%s | sync: sc_id=0x%02x msg=\"%s\"\n", prefix, pl[0], msg);
    }
    else if (apid == 0x7FF && plen >= 14) {
        char msg[15];
        memcpy(msg, pl, 14);
        msg[14] = '\0';
        printf("%s | idle: \"%s\"\n", prefix, msg);
    }
    else {
        printf("%s | payload (%d bytes follows header)\n", prefix, plen);
    }
}

/* ------------------------------------------------------------------ */
/*  Usage                                                             */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    puts(
"telemetry_test — control y monitor de telemetria CCSDS via SX1262\n"
"\n"
"USO:  telemetry_test <comando> [argumentos]\n"
"\n"
"COMANDOS DE CONTROL (solo telemetry_service):\n"
"  ping                     Verifica que el TelemetryService responde.\n"
"  start <inst> <ms>        Inicia TX periodica cada <ms> sobre radio <inst>.\n"
"  stop                     Detiene la TX.\n"
"  status                   Estado actual.\n"
"  config <inst> <freq_hz>  Configura parametros LoRa.\n"
"         <sf> <bw> <cr>\n"
"         <pwr_dbm>\n"
"\n"
"COMANDO MONITOR (abre telemetry + radio):\n"
"  monitor <inst> <flags>   Escucha paquetes. <flags> elige que ver:\n"
"      1 = solo [Monitor TX] (telemetria propia)\n"
"      2 = solo [Monitor RX] (paquetes de otro flatsat)\n"
"      3 = ambos (TX + RX), por defecto\n"
"    El radio escucha en Rx continua y tambien transmite tu telemetria.\n"
"    Ctrl-C para salir.\n"
"\n"
"Valores por defecto (flatsat): 916 MHz, SF7, BW250, CR 4/5, 20 dBm, 10500 ms.\n"
"\n"
"EJEMPLOS:\n"
"  telemetry_test config 0 916000000 7 250000 1 20\n"
"  telemetry_test start 0 10500\n"
"  telemetry_test monitor 0 1     # solo ver TX propia\n"
"  telemetry_test monitor 0 2     # solo esnifar RX de otro flatsat\n"
"  telemetry_test monitor 0 3     # ambos a la vez\n"
"  telemetry_test status\n"
"  telemetry_test stop");
}

/* ------------------------------------------------------------------ */
/*  Monitor loop                                                      */
/* ------------------------------------------------------------------ */

static int do_monitor(int tf, int rf)
{
    uint8_t buf[512];
    int n;

    for (;;) {
        struct pollfd pfds[2];

        pfds[0].fd = tf; pfds[0].events = POLLIN;
        pfds[1].fd = rf; pfds[1].events = POLLIN;

        n = poll(pfds, 2, 60000);
        if (n < 0) { perror("poll"); break; }
        if (n == 0) { fprintf(stderr, "monitor: timeout\n"); break; }

        for (int i = 0; i < 2; i++) {
            if (!(pfds[i].revents & POLLIN))
                continue;

            n = read(pfds[i].fd, buf, sizeof(buf));
            if (n <= 0) { perror("read"); break; }

            if (pfds[i].fd == tf && buf[0] == EVT_TX) {
                uint32_t off = 1;
                while (off < (uint32_t)n) {
                    uint8_t plen = buf[off++];
                    if (off + plen > (uint32_t)n) break;
                    if (plen >= 6) {
                        uint16_t apid = (((uint16_t)buf[off] << 8) | buf[off + 1]) & 0x07FF;
                        char label[64];
                        snprintf(label, sizeof(label),
                                 "[Monitor TX] APID=0x%03x (%s)",
                                 apid, apid_name(apid));
                        print_packet(label, buf + off, plen);
                    }
                    off += plen;
                }
            }
            else if (pfds[i].fd == rf) {
                if (buf[0] == EVT_RX && n >= 7) {
                    uint8_t inst = buf[1], flags = buf[2], len = buf[3];
                    int16_t rssi = (int16_t)(((uint16_t)buf[4] << 8) | buf[5]);
                    int8_t  snr  = (int8_t)buf[6];
                    printf("[Monitor RX] radio=%d len=%d rssi=%d dBm snr=%d crc=%s\n",
                           inst, len, rssi, snr, (flags & 1) ? "ok" : "BAD");
                    if (n >= 7 + (int)len) {
                        uint16_t apid = (((uint16_t)buf[7] << 8) | buf[8]) & 0x07FF;
                        char label[64];
                        snprintf(label, sizeof(label),
                                 "[Monitor RX] APID=0x%03x (%s)",
                                 apid, apid_name(apid));
                        print_packet(label, buf + 7, len);
                    }
                }
                else if (buf[0] == EVT_RX_TIMEOUT && n >= 2) {
                    printf("[Monitor RX] radio=%d timeout\n", buf[1]);
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *cmd;
    int fd = -1, rf = -1, n, rc = 0;
    uint8_t req[16], rsp[64];
    int flags = 3;

    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    cmd = argv[1];

    /* ---- monitor ---- */
    if (!strcmp(cmd, "monitor")) {
        int inst = (argc >= 3) ? atoi(argv[2]) : 0;
        if (argc >= 4) flags = atoi(argv[3]);
        if (flags < 1 || flags > 3) { fprintf(stderr, "error: flags debe ser 1, 2 o 3.\n"); return 2; }
        if (inst < 0 || inst > 1) { fprintf(stderr, "error: inst debe ser 0 o 1.\n"); return 2; }

        if (flags & 1) {
            fd = open_channel("rpmsg-telemetry", TELEM_DST);
            if (fd < 0) return 1;
        }
        if (flags & 2) {
            rf = open_channel("rpmsg-radio", RADIO_DST);
            if (rf < 0) { if (fd >= 0) close_ept(fd); return 1; }
        }

        if (fd >= 0) {
            uint8_t mon_req[2] = { 0x06, 0x01 };
            n = xfer(fd, mon_req, 2, rsp, sizeof(rsp), 3000);
            if (n < 2 || rsp[1] != 0) {
                fprintf(stderr, "monitor: fallo al activar monitor en MCU\n");
                close_ept(fd); close_ept(rf); return 1;
            }
        }

        if (rf >= 0) {
            uint8_t rx_req[4] = { 0x0E, (uint8_t)inst, 0x00, 0x00 };
            n = xfer(rf, rx_req, 4, rsp, sizeof(rsp), 3000);
            if (n < 2 || rsp[1] != 0) {
                fprintf(stderr, "monitor: fallo al iniciar RX en radio %d (n=%d err=0x%02x)\n"
                                "Asegurate de haber hecho 'radio_test init %d' primero.\n",
                        inst, n, n >= 2 ? rsp[1] : 0xFF,
                        inst == 0 ? 915000000 : 915000000);
                close_ept(fd); close_ept(rf); return 1;
            }
        }

        printf("[Monitor] activo:");
        if (fd >= 0) printf(" [Monitor TX]");
        if (rf >= 0) printf(" [Monitor RX] radio=%d", inst);
        printf(". Ctrl-C para salir.\n");

        rc = do_monitor(fd >= 0 ? fd : -2, rf >= 0 ? rf : -2);
        close_ept(fd);
        close_ept(rf);
        return rc;
    }

    /* ---- commands that only need telemetry channel ---- */
    fd = open_channel("rpmsg-telemetry", TELEM_DST);
    if (fd < 0) return 1;

    if (!strcmp(cmd, "ping")) {
        uint8_t p[1] = { 0x01 };
        n = xfer(fd, p, 1, rsp, sizeof(rsp), 3000);
        if (n >= 7 && rsp[1] == 0)
            printf("ping: err=0x%02x id=%c%c%c%c v%d\n",
                   rsp[1], rsp[2], rsp[3], rsp[4], rsp[5], rsp[6]);
        else { fprintf(stderr, "ping: sin respuesta valida (n=%d)\n", n); rc = 1; }
    }
    else if (!strcmp(cmd, "start")) {
        int inst, interval;
        if (need_arg(argc >= 4, "inst> <ms")) { rc = 2; goto out; }
        inst = atoi(argv[2]);
        interval = atoi(argv[3]);
        if (inst < 0 || inst > 1) { fprintf(stderr, "error: inst debe ser 0 o 1.\n"); rc = 2; goto out; }
        if (interval < 1000 || interval > 600000) { fprintf(stderr, "error: ms debe ser 1000-600000.\n"); rc = 2; goto out; }
        req[0] = 0x02; req[1] = (uint8_t)inst;
        req[2] = (uint8_t)(interval >> 8); req[3] = (uint8_t)interval;
        n = xfer(fd, req, 4, rsp, sizeof(rsp), 3000);
        if (n >= 2 && rsp[1] == 0)
            printf("start: ok (radio %d, interval=%d ms)\n", inst, interval);
        else { fprintf(stderr, "start: fallo (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF); rc = 1; }
    }
    else if (!strcmp(cmd, "stop")) {
        uint8_t p[1] = { 0x03 };
        n = xfer(fd, p, 1, rsp, sizeof(rsp), 3000);
        if (n >= 2 && rsp[1] == 0)
            printf("stop: ok\n");
        else { fprintf(stderr, "stop: fallo (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF); rc = 1; }
    }
    else if (!strcmp(cmd, "status")) {
        uint8_t p[1] = { 0x04 };
        n = xfer(fd, p, 1, rsp, sizeof(rsp), 3000);
        if (n >= 16 && rsp[1] == 0) {
            int running = rsp[2], inst = rsp[3];
            int interval = ((int)rsp[4] << 8) | rsp[5];
            int seq = ((int)rsp[6] << 8) | rsp[7];
            uint32_t tx_ok = ((uint32_t)rsp[8] << 24) | ((uint32_t)rsp[9] << 16) |
                             ((uint32_t)rsp[10] << 8) | rsp[11];
            uint32_t fails = ((uint32_t)rsp[12] << 24) | ((uint32_t)rsp[13] << 16) |
                             ((uint32_t)rsp[14] << 8) | rsp[15];
            printf("status: %s  inst=%d  interval=%d ms  seq=%u  tx_ok=%u  fails=%u\n",
                   running ? "RUNNING" : "STOPPED", inst, interval, seq, tx_ok, fails);
        } else { fprintf(stderr, "status: fallo (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF); rc = 1; }
    }
    else if (!strcmp(cmd, "config")) {
        int inst, sf, cr, power;
        uint32_t freq, bw;
        if (need_arg(argc >= 8, "inst> <freq_hz> <sf> <bw> <cr> <pwr_dbm")) { rc = 2; goto out; }
        inst  = atoi(argv[2]);
        freq  = (uint32_t)strtoul(argv[3], NULL, 10);
        sf    = atoi(argv[4]);
        bw    = (uint32_t)strtoul(argv[5], NULL, 10);
        cr    = atoi(argv[6]);
        power = atoi(argv[7]);
        if (inst < 0 || inst > 1) { fprintf(stderr, "error: inst debe ser 0 o 1.\n"); rc = 2; goto out; }
        if (freq < 150000000U || freq > 960000000U) { fprintf(stderr, "error: freq fuera de rango.\n"); rc = 2; goto out; }
        if (sf < 5 || sf > 12) { fprintf(stderr, "error: SF 5-12.\n"); rc = 2; goto out; }
        if (power < -9 || power > 22) { fprintf(stderr, "error: potencia -9..22 dBm.\n"); rc = 2; goto out; }
        req[0] = 0x05; req[1] = (uint8_t)inst;
        req[2] = (uint8_t)(freq >> 24); req[3] = (uint8_t)(freq >> 16);
        req[4] = (uint8_t)(freq >> 8);  req[5] = (uint8_t)freq;
        req[6] = (uint8_t)sf;
        req[7] = (uint8_t)(bw >> 8); req[8] = (uint8_t)bw;
        req[9] = (uint8_t)cr; req[10] = (uint8_t)power;
        n = xfer(fd, req, 11, rsp, sizeof(rsp), 3000);
        if (n >= 2 && rsp[1] == 0)
            printf("config: ok (radio=%d freq=%u sf=%d bw=%u cr=%d power=%d dBm)\n",
                   inst, freq, sf, bw, cr, power);
        else { fprintf(stderr, "config: fallo (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF); rc = 1; }
    }
    else {
        fprintf(stderr, "error: comando desconocido '%s'.\n", cmd);
        rc = 2;
    }

out:
    close_ept(fd);
    return rc;
}
