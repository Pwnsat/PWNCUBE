/*
 * CubeSat — SensorService client (BME280 + ICM-42670 on the RISC-V MCU).
 *
 * The I2C0 sensors are owned by the MCU's RT-Thread firmware; this tool
 * queries them over IPC (rpmsg). The "rpmsg-sensor" channel is bound at boot
 * (rcS). See docs/migration/{design/40-migration-design,implementation/90-mcu-config-replication}.md.
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

#define SENSOR_DST   0x4006
#define SENSOR_ERR_IO 0x11

static uint32_t g_src;   /* unique per-process src, so we can find our endpoint */

static void usage(void)
{
    puts(
"sensor_test — reads the I2C0 sensors (BME280 + ICM-42670) owned by the MCU\n"
"\n"
"USAGE:  sensor_test <command>\n"
"\n"
"COMMANDS:\n"
"  ping             Check that SensorService responds (id=SENS).\n"
"  whoami <chip>    Read the chip ID. chip: bme (=0x60) or icm (=0x67).\n"
"  bme              BME280: compensated temperature, pressure, and humidity.\n"
"  imu              ICM-42670: acceleration, gyro, and temperature.\n"
"  all              bme + imu.\n"
"  help             This help text.");
}

/* Read /sys/class/rpmsg/rpmsgN/src (the endpoint's local address), or -1. */
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

/* Find the rpmsg_ctrl index whose backing channel is "rpmsg-sensor". The dst
 * override is ignored when the ctrl belongs to a different channel (the channel
 * forces its own remote address), so we MUST use the sensor channel's own ctrl
 * device — not just the first one that opens. */
static int find_sensor_ctrl(void)
{
    char lp[96], target[256];
    ssize_t r;
    int i;

    for (i = 0; i < 8; i++) {
        snprintf(lp, sizeof(lp), "/sys/class/rpmsg/rpmsg_ctrl%d/device", i);
        r = readlink(lp, target, sizeof(target) - 1);
        if (r > 0) {
            target[r] = '\0';
            if (strstr(target, "rpmsg-sensor"))
                return i;
        }
    }
    return -1;
}

/* Create an endpoint (dst 0x4006) on the rpmsg-sensor ctrl device with a
 * unique src, then locate our /dev/rpmsgN by matching that src in sysfs. Using
 * a per-process src makes discovery race-free even when several clients run
 * concurrently and stale endpoint nodes linger. */
static int open_ept(void)
{
    struct rpmsg_endpoint_info ept = { .name = "sensor" };
    int cfd = -1, ctrl, t, n;
    char cp[64];

    ctrl = find_sensor_ctrl();
    if (ctrl < 0) {
        fprintf(stderr, "error: can't find channel rpmsg-sensor (did the MCU boot? did rcS bind it?).\n"
                        "Check: ls /sys/bus/rpmsg/devices/  (should show virtio0.rpmsg-sensor.*)\n");
        return -1;
    }
    g_src = 0x5000 + (uint32_t)(getpid() & 0x0FFF);
    ept.src = g_src;
    ept.dst = SENSOR_DST;

    snprintf(cp, sizeof(cp), "/dev/rpmsg_ctrl%d", ctrl);
    cfd = open(cp, O_RDWR);
    if (cfd < 0) { perror(cp); return -1; }
    if (ioctl(cfd, RPMSG_CREATE_EPT_IOCTL, &ept) < 0) { perror("create ept"); close(cfd); return -1; }
    close(cfd);

    for (t = 0; t < 50; t++) {
        for (n = 0; n < 32; n++) {
            if (ept_src(n) == (long)g_src) {
                char np[64];
                int fd;
                snprintf(np, sizeof(np), "/dev/rpmsg%d", n);
                fd = open(np, O_RDWR);
                if (fd >= 0)
                    return fd;
            }
        }
        usleep(20000);
    }
    fprintf(stderr, "error: /dev/rpmsgN endpoint never appeared after creating it.\n");
    return -1;
}

/* Tear down our endpoint so /dev/rpmsgN nodes don't leak across invocations. */
static void close_ept(int fd)
{
    if (fd >= 0) {
        (void)ioctl(fd, RPMSG_DESTROY_EPT_IOCTL, 0);
        close(fd);
    }
}

static int xfer(int fd, const uint8_t *req, int req_len, uint8_t *rsp, int rsp_max, int tmo_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int n;

    if (write(fd, req, req_len) != req_len) { perror("write"); return -1; }
    n = poll(&pfd, 1, tmo_ms);
    if (n <= 0) return n;
    return read(fd, rsp, rsp_max);
}

static int32_t g_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static int16_t g_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int do_bme(int fd)
{
    uint8_t req[2] = { 0x10, 0 }, rsp[64];
    int n = xfer(fd, req, 1, rsp, sizeof(rsp), 3000);

    if (n < 14 || rsp[1] != 0) {
        fprintf(stderr, "bme: failed (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF);
        return 1;
    }
    {
        int32_t t = g_i32(&rsp[2]);
        uint32_t p = (uint32_t)g_i32(&rsp[6]);
        uint32_t h = (uint32_t)g_i32(&rsp[10]);
        printf("bme280: temp=%d.%03d C  press=%u.%03u hPa  hum=%u.%03u %%RH\n",
               t / 1000, (t < 0 ? -t : t) % 1000,
               p / 100, p % 100 * 10, h / 1000, h % 1000);
    }
    return 0;
}

static int do_imu(int fd)
{
    uint8_t req[2] = { 0x20, 0 }, rsp[64];
    int n = xfer(fd, req, 1, rsp, sizeof(rsp), 3000);

    if (n < 16 || rsp[1] != 0) {
        fprintf(stderr, "imu: failed (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF);
        return 1;
    }
    {
        int16_t ax = g_i16(&rsp[2]),  ay = g_i16(&rsp[4]),  az = g_i16(&rsp[6]);
        int16_t gx = g_i16(&rsp[8]),  gy = g_i16(&rsp[10]), gz = g_i16(&rsp[12]);
        int16_t tp = g_i16(&rsp[14]);
        /* accel ±16 g -> g = raw*16/32768 ; gyro ±2000 dps ; temp C = raw/128+25 */
        printf("icm42670: accel[g]=%.3f,%.3f,%.3f  gyro[dps]=%.2f,%.2f,%.2f  temp=%.1f C\n",
               ax * 16.0 / 32768, ay * 16.0 / 32768, az * 16.0 / 32768,
               gx * 2000.0 / 32768, gy * 2000.0 / 32768, gz * 2000.0 / 32768,
               tp / 128.0 + 25.0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd;
    int fd, n, rc = 0;
    uint8_t rsp[64];

    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    cmd = argv[1];

    fd = open_ept();
    if (fd < 0) return 1;

    if (!strcmp(cmd, "ping")) {
        uint8_t req[1] = { 0x01 };
        n = xfer(fd, req, 1, rsp, sizeof(rsp), 3000);
        if (n >= 7 && rsp[1] == 0)
            printf("ping: err=0x%02x id=%c%c%c%c v%d\n", rsp[1], rsp[2], rsp[3], rsp[4], rsp[5], rsp[6]);
        else { fprintf(stderr, "ping: no valid response (n=%d)\n", n); rc = 1; }
    }
    else if (!strcmp(cmd, "whoami")) {
        uint8_t req[2] = { 0x02, 0 };
        int chip;
        if (argc < 3) { fprintf(stderr, "error: missing <chip> (bme|icm)\n"); close_ept(fd); return 2; }
        chip = (!strcmp(argv[2], "icm") || !strcmp(argv[2], "1")) ? 1 : 0;
        req[1] = (uint8_t)chip;
        n = xfer(fd, req, 2, rsp, sizeof(rsp), 3000);
        if (n >= 3 && rsp[1] == 0)
            printf("whoami %s: id=0x%02x (esperado %s)\n", chip ? "icm" : "bme",
                   rsp[2], chip ? "0x67" : "0x60");
        else { fprintf(stderr, "whoami: failed (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF); rc = 1; }
    }
    else if (!strcmp(cmd, "bme"))  rc = do_bme(fd);
    else if (!strcmp(cmd, "imu"))  rc = do_imu(fd);
    else if (!strcmp(cmd, "all"))  { rc  = do_bme(fd); rc |= do_imu(fd); }
    else { fprintf(stderr, "error: unknown command '%s'. Try 'sensor_test help'.\n", cmd); rc = 2; }

    close_ept(fd);
    return rc;
}
