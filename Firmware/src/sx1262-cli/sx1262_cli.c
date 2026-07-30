#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <stdint.h>
#include <errno.h>

/* Copy ioctl defines from the kernel driver header */
#define SX1262_IOCTL_MAGIC 'L'
#define SX1262_IOCTL_RESET         _IO(SX1262_IOCTL_MAGIC, 0)
#define SX1262_IOCTL_SET_STANDBY   _IO(SX1262_IOCTL_MAGIC, 1)
#define SX1262_IOCTL_SET_SLEEP     _IO(SX1262_IOCTL_MAGIC, 2)
#define SX1262_IOCTL_SET_FREQ      _IOW(SX1262_IOCTL_MAGIC, 3, uint32_t)
#define SX1262_IOCTL_SET_POWER     _IOW(SX1262_IOCTL_MAGIC, 4, int8_t)
#define SX1262_IOCTL_SET_MODEM     _IOW(SX1262_IOCTL_MAGIC, 5, struct sx1262_modem_config)
#define SX1262_IOCTL_SET_TX        _IOW(SX1262_IOCTL_MAGIC, 6, uint32_t)
#define SX1262_IOCTL_SET_RX        _IOW(SX1262_IOCTL_MAGIC, 7, uint32_t)
#define SX1262_IOCTL_GET_STATUS    _IOR(SX1262_IOCTL_MAGIC, 8, uint8_t)
#define SX1262_IOCTL_GET_RSSI      _IOR(SX1262_IOCTL_MAGIC, 9, int16_t)
#define SX1262_IOCTL_SET_ANTSW     _IOW(SX1262_IOCTL_MAGIC, 10, uint8_t)
#define SX1262_IOCTL_READ_REG      _IOWR(SX1262_IOCTL_MAGIC, 12, struct sx1262_reg_access)
#define SX1262_IOCTL_WRITE_REG     _IOW(SX1262_IOCTL_MAGIC, 13, struct sx1262_reg_access)
#define SX1262_IOCTL_GET_IRQ_STATUS _IOR(SX1262_IOCTL_MAGIC, 14, uint16_t)
#define SX1262_IOCTL_SET_CW         _IO(SX1262_IOCTL_MAGIC, 15)

struct sx1262_reg_access {
    uint16_t addr;
    uint8_t val;
};

struct sx1262_modem_config {
    uint8_t sf;
    uint32_t bw;
    uint8_t cr;
    uint16_t preamble;
};

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s --dev <0|1> <command> [args]\n", prog);
    fprintf(stderr, "\nCommands:\n");
    fprintf(stderr, "  detect              Check if SX1262 is present\n");
    fprintf(stderr, "  reset               HW reset\n");
    fprintf(stderr, "  standby             Enter standby mode\n");
    fprintf(stderr, "  sleep               Enter sleep mode\n");
    fprintf(stderr, "  freq <hz>           Set frequency in Hz\n");
    fprintf(stderr, "  power <dbm>         Set TX power (-9 to 22 dBm)\n");
    fprintf(stderr, "  setTX <hz> <ms>     Set frequency and transmit\n");
    fprintf(stderr, "  setRX <hz> <ms>     Set frequency and receive\n");
    fprintf(stderr, "  rssi                Get RSSI\n");
    fprintf(stderr, "  status              Get chip status\n");
    fprintf(stderr, "  ant <0|1|2>         Set ANT_SW mode (0=auto, 1=TX, 2=RX)\n");
    fprintf(stderr, "  cw <hz>             Continuous carrier at freq (stop with 'standby')\n");
    fprintf(stderr, "  modem <sf> <bwhz> <cr>  Set SF/BW/CR (e.g. 7 125000 1)\n");
    fprintf(stderr, "  send <hex> [ms]     Send hex packet, optional timeout\n");
    fprintf(stderr, "  recv [ms]           Receive packet, optional timeout\n");
    fprintf(stderr, "  regread <hexaddr>   Read register (addr in hex)\n");
    fprintf(stderr, "  regwrite <hexaddr> <hexval>  Write register\n");
    fprintf(stderr, "  irq                 Get IRQ status\n");
}

static int open_dev(int dev_id)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/sx1262-%d", dev_id);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
    }
    return fd;
}

static int cmd_detect(int fd)
{
    uint8_t status;
    int ret = ioctl(fd, SX1262_IOCTL_GET_STATUS, &status);
    if (ret < 0) {
        printf("FAIL: %s\n", strerror(errno));
        return ret;
    }
    printf("OK: status=0x%02X\n", status);
    return 0;
}

static int cmd_reset(int fd)
{
    return ioctl(fd, SX1262_IOCTL_RESET);
}

static int cmd_standby(int fd)
{
    return ioctl(fd, SX1262_IOCTL_SET_STANDBY);
}

static int cmd_sleep(int fd)
{
    return ioctl(fd, SX1262_IOCTL_SET_SLEEP);
}

static int cmd_freq(int fd, uint32_t hz)
{
    return ioctl(fd, SX1262_IOCTL_SET_FREQ, &hz);
}

static int cmd_power(int fd, int8_t dbm)
{
    return ioctl(fd, SX1262_IOCTL_SET_POWER, &dbm);
}

static int cmd_rssi(int fd)
{
    int16_t rssi;
    int ret = ioctl(fd, SX1262_IOCTL_GET_RSSI, &rssi);
    if (ret < 0) return ret;
    printf("RSSI: %d dBm\n", rssi);
    return 0;
}

static int cmd_status(int fd)
{
    uint8_t status;
    int ret = ioctl(fd, SX1262_IOCTL_GET_STATUS, &status);
    if (ret < 0) return ret;
    printf("Status: 0x%02X\n", status);
    printf("  Chip mode: ");
    switch ((status >> 4) & 0x07) {
        case 2: printf("STDBY_RC\n"); break;
        case 3: printf("STDBY_XOSC\n"); break;
        case 4: printf("FS\n"); break;
        case 5: printf("RX\n"); break;
        case 6: printf("TX\n"); break;
        default: printf("SLEEP/UNKNOWN\n"); break;
    }
    return 0;
}

static int cmd_ant(int fd, uint8_t mode)
{
    return ioctl(fd, SX1262_IOCTL_SET_ANTSW, &mode);
}

static int cmd_modem(int fd, uint8_t sf, uint32_t bw, uint8_t cr)
{
    struct sx1262_modem_config mc = { .sf = sf, .bw = bw, .cr = cr, .preamble = 8 };
    int ret = ioctl(fd, SX1262_IOCTL_SET_MODEM, &mc);
    if (ret < 0) return ret;
    printf("Modem set: SF%u BW%u CR4/%u\n", sf, bw, cr + 4);
    return 0;
}

static int cmd_cw(int fd, uint32_t hz)
{
    int ret = ioctl(fd, SX1262_IOCTL_SET_FREQ, &hz);
    if (ret < 0) return ret;
    ret = ioctl(fd, SX1262_IOCTL_SET_CW);
    if (ret < 0) return ret;
    printf("Continuous wave ON at %u Hz (use 'standby' to stop)\n", hz);
    return 0;
}

static int cmd_settx(int fd, uint32_t hz, uint32_t timeout_ms)
{
    int ret = ioctl(fd, SX1262_IOCTL_SET_FREQ, &hz);
    if (ret < 0) return ret;
    ret = ioctl(fd, SX1262_IOCTL_SET_STANDBY);
    if (ret < 0) return ret;
    usleep(10000);
    return ioctl(fd, SX1262_IOCTL_SET_TX, &timeout_ms);
}

static int cmd_setrx(int fd, uint32_t hz, uint32_t timeout_ms)
{
    int ret = ioctl(fd, SX1262_IOCTL_SET_FREQ, &hz);
    if (ret < 0) return ret;
    ret = ioctl(fd, SX1262_IOCTL_SET_STANDBY);
    if (ret < 0) return ret;
    usleep(10000);
    return ioctl(fd, SX1262_IOCTL_SET_RX, &timeout_ms);
}

static int hex_to_bytes(const char *hex, uint8_t *buf, size_t max)
{
    size_t len = strlen(hex);
    if (len % 2) return -1;
    size_t count = len / 2;
    if (count > max) return -1;
    for (size_t i = 0; i < count; i++) {
        char byte[3] = { hex[i*2], hex[i*2+1], 0 };
        buf[i] = (uint8_t)strtoul(byte, NULL, 16);
    }
    return (int)count;
}

static int cmd_send(int fd, const char *hex_str, uint32_t timeout_ms)
{
    uint8_t buf[255];
    int len = hex_to_bytes(hex_str, buf, sizeof(buf));
    if (len < 0) {
        fprintf(stderr, "Invalid hex string\n");
        return -1;
    }
    /* Write to driver (triggers FIFO load + TX) */
    ssize_t w = write(fd, buf, (size_t)len);
    if (w < 0) {
        perror("write");
        return -1;
    }
    printf("Sent %d bytes\n", (int)w);
    return 0;
}

static int cmd_regread(int fd, uint16_t addr)
{
    struct sx1262_reg_access ra = { .addr = addr, .val = 0 };
    int ret = ioctl(fd, SX1262_IOCTL_READ_REG, &ra);
    if (ret < 0) return ret;
    printf("Reg 0x%04X = 0x%02X\n", ra.addr, ra.val);
    return 0;
}

static int cmd_regwrite(int fd, uint16_t addr, uint8_t val)
{
    struct sx1262_reg_access ra = { .addr = addr, .val = val };
    return ioctl(fd, SX1262_IOCTL_WRITE_REG, &ra);
}

static int cmd_irq(int fd)
{
    uint16_t irq_val;
    int ret = ioctl(fd, SX1262_IOCTL_GET_IRQ_STATUS, &irq_val);
    if (ret < 0) return ret;
    printf("IRQ status: 0x%04X\n", irq_val);
    if (irq_val & (1 << 0))  printf("  TX_DONE\n");
    if (irq_val & (1 << 1))  printf("  RX_DONE\n");
    if (irq_val & (1 << 2))  printf("  PREAMBLE_DETECTED\n");
    if (irq_val & (1 << 3))  printf("  SYNC_WORD_VALID\n");
    if (irq_val & (1 << 4))  printf("  HEADER_VALID\n");
    if (irq_val & (1 << 5))  printf("  HEADER_ERROR\n");
    if (irq_val & (1 << 6))  printf("  CRC_ERROR\n");
    if (irq_val & (1 << 7))  printf("  CAD_DONE\n");
    if (irq_val & (1 << 8))  printf("  CAD_DETECTED\n");
    if (irq_val & (1 << 9))  printf("  TIMEOUT\n");
    return 0;
}

static int cmd_recv(int fd, uint32_t timeout_ms)
{
    uint8_t buf[255];
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms > 0 ? (int)timeout_ms : -1);
    if (ret < 0) { perror("poll"); return -1; }
    if (ret == 0) { printf("Timeout\n"); return 0; }

    ssize_t r = read(fd, buf, sizeof(buf));
    if (r < 0) { perror("read"); return -1; }
    if (r == 0) { printf("No data\n"); return 0; }

    printf("Received %d bytes: ", (int)r);
    for (ssize_t i = 0; i < r; i++)
        printf("%02X ", buf[i]);
    printf("\n[RSSI: get via ioctl]\n");
    return 0;
}

int main(int argc, char **argv)
{
    int dev_id = 0;

    if (argc < 2) { usage(argv[0]); return 1; }

    /* Parse --dev option */
    int argpos = 1;
    if (argc > 2 && strcmp(argv[1], "--dev") == 0) {
        dev_id = atoi(argv[2]);
        argpos = 3;
    }

    if (argpos >= argc) { usage(argv[0]); return 1; }
    const char *cmd = argv[argpos];

    int fd = open_dev(dev_id);
    if (fd < 0) return 1;

    int ret = 0;
    if (strcmp(cmd, "detect") == 0) {
        ret = cmd_detect(fd);
    } else if (strcmp(cmd, "reset") == 0) {
        ret = cmd_reset(fd);
    } else if (strcmp(cmd, "standby") == 0) {
        ret = cmd_standby(fd);
    } else if (strcmp(cmd, "sleep") == 0) {
        ret = cmd_sleep(fd);
    } else if (strcmp(cmd, "freq") == 0 && argc > argpos+1) {
        ret = cmd_freq(fd, (uint32_t)atol(argv[argpos+1]));
    } else if (strcmp(cmd, "power") == 0 && argc > argpos+1) {
        ret = cmd_power(fd, (int8_t)atoi(argv[argpos+1]));
    } else if (strcmp(cmd, "rssi") == 0) {
        ret = cmd_rssi(fd);
    } else if (strcmp(cmd, "status") == 0) {
        ret = cmd_status(fd);
    } else if (strcmp(cmd, "ant") == 0 && argc > argpos+1) {
        ret = cmd_ant(fd, (uint8_t)atoi(argv[argpos+1]));
    } else if (strcmp(cmd, "cw") == 0 && argc > argpos+1) {
        ret = cmd_cw(fd, (uint32_t)atol(argv[argpos+1]));
    } else if (strcmp(cmd, "modem") == 0 && argc > argpos+3) {
        ret = cmd_modem(fd, (uint8_t)atoi(argv[argpos+1]),
                        (uint32_t)atol(argv[argpos+2]), (uint8_t)atoi(argv[argpos+3]));
    } else if (strcmp(cmd, "setTX") == 0 && argc > argpos+2) {
        ret = cmd_settx(fd, (uint32_t)atol(argv[argpos+1]), (uint32_t)atol(argv[argpos+2]));
    } else if (strcmp(cmd, "setRX") == 0 && argc > argpos+2) {
        ret = cmd_setrx(fd, (uint32_t)atol(argv[argpos+1]), (uint32_t)atol(argv[argpos+2]));
    } else if (strcmp(cmd, "send") == 0 && argc > argpos+1) {
        uint32_t tmo = (argc > argpos+2) ? (uint32_t)atol(argv[argpos+2]) : 5000;
        ret = cmd_send(fd, argv[argpos+1], tmo);
    } else if (strcmp(cmd, "recv") == 0) {
        uint32_t tmo = (argc > argpos+1) ? (uint32_t)atol(argv[argpos+1]) : 0;
        ret = cmd_recv(fd, tmo);
    } else if (strcmp(cmd, "regread") == 0 && argc > argpos+1) {
        ret = cmd_regread(fd, (uint16_t)strtoul(argv[argpos+1], NULL, 16));
    } else if (strcmp(cmd, "regwrite") == 0 && argc > argpos+2) {
        ret = cmd_regwrite(fd, (uint16_t)strtoul(argv[argpos+1], NULL, 16),
                           (uint8_t)strtoul(argv[argpos+2], NULL, 16));
    } else if (strcmp(cmd, "irq") == 0) {
        ret = cmd_irq(fd);
    } else {
        usage(argv[0]);
        ret = 1;
    }

    if (ret < 0) {
        fprintf(stderr, "Command failed: %s\n", strerror(errno));
    }

    close(fd);
    return ret ? 1 : 0;
}
