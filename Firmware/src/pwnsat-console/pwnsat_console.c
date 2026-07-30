/* PWNSAT console -- "Black Hat Arsenal Edition" banner/menu, ported from the
 * FlatSat serial console (spacecan.cpp: scPrintBanner/scPrintMenu). FlatSat
 * runs this bare-metal over USB CDC; on PWNCUBE the USB gadget console is
 * owned by Linux (ttyGS0), so this is a userspace client instead of MCU
 * code -- same rpmsg-backed data (sensor_test/radio_test), new front end.
 *
 * Launched as the ttyGS0 respawn entry (see rootfs/skeleton/etc/inittab),
 * replacing the raw `/bin/sh` PWNCUBE had before. Menu option 3 (Debug
 * Shell) is the escape hatch back to a real shell for tools like
 * pwncube_console.py that expect one waiting on the other end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

#include "spacecan_sim.h"

#define ANSI_RESET   "\x1b[0m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_MAGENTA "\x1b[35m"

static struct termios orig_termios;

static void restore_termios(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void set_raw_mode(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    /* Force \n -> \r\n translation on output for this tty. termios output
     * settings belong to the tty device, not to our process, so this also
     * fixes plain-\n output from subprocesses we spawn (sensor_test,
     * radio_test) without touching their source. Without this, the
     * "generic" usbserial driver on the far end (its own dmesg warning:
     * "only for testing and one-off prototypes") doesn't reliably do this
     * translation on its own -- output comes out staircased/torn. */
    raw.c_oflag |= OPOST | ONLCR;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void print_banner(void) {
    printf(ANSI_GREEN
        "              \\             /\n"
        "               \\           /\n"
        "                +---------+\n"
        "                |         |\n"
        "                | [ ][ ]  |\n"
        "                |         |\n"
        "                +---------+\n"
        "               /           \\\n"
        "              /             \\\n"
        "\n"
        "              P W N S A T\n"
        "       --  BLACK HAT ARSENAL EDITION  --\n"
        ANSI_RESET);
}

/* Matches FlatSat's scPrintMenu(), which always prints the banner too --
 * every reprint (initial, M, idle refresh, back from a submenu) shows the
 * full art, not just the menu lines. */
static void print_menu(void) {
    print_banner();
    printf("\n");
    printf("==================================================\n");
    printf("PWNSAT CONSOLE: Press the number to select.\n");
    printf("  1) Sensor / Telemetry Dashboard\n");
    printf("  2) SpaceCAN Bus Console\n");
    printf("  3) Debug Shell\n");
    printf("  4) Exit console (quiet mode)\n");
    printf("Press M anytime to return here.\n");
    printf("==================================================\n");
}

/* Reads one key with a timeout (seconds). Returns 0 on timeout, -1 on EOF,
 * else the key. Lets the dashboard refresh while staying responsive. */
static int read_key_timeout(int timeout_s) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    int rv = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (rv <= 0) return 0;
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    return c;
}

static void read_uptime(unsigned long *hh, unsigned long *mm, unsigned long *ss,
                         unsigned long *total_ms) {
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0.0;
    if (f) {
        if (fscanf(f, "%lf", &up) != 1) up = 0.0;
        fclose(f);
    }
    unsigned long total_s = (unsigned long)up;
    *total_ms = (unsigned long)(up * 1000.0);
    *hh = total_s / 3600UL;
    *mm = (total_s % 3600UL) / 60UL;
    *ss = total_s % 60UL;
}

/* Ported from FlatSat's printSystemStatusDashboard() (worker.cpp) -- same
 * sections/labels, sourced from sensor_test/radio_test over rpmsg instead
 * of direct hardware reads. GPS/NAV now backed by the real NEO-6M driver
 * (Etapa 5, gps_nmea.c/.h on the MCU side) via `radio_test gps_status` --
 * a synchronous local rpmsg query, same pattern as `sensor_test all`
 * below, no SPP/RF round trip involved. */
static void print_system_status_dashboard(void) {
    unsigned long hh, mm, ss, total_ms;
    read_uptime(&hh, &mm, &ss, &total_ms);

    printf("==================================================\n");
    printf("--- SYSTEM STATUS DASHBOARD ---\n");
    printf("Satellite Uptime:    %02lu:%02lu:%02lu (%lu ms)\n", hh, mm, ss, total_ms);
    printf("\n");
    printf("--- RADIO SUBSYSTEM ---\n");
    printf("Downlink:            916 MHz\n"); /* mission.h DOWNLINK_FREQ */
    printf("Uplink:              918 MHz\n"); /* mission.h UPLINK_FREQ */
    printf("\n");
    printf("--- GPS / NAV ---\n");
    {
        int override_active = 0, uart_ok = 0, fix_valid = 0, sats = 0;
        float lat = 0.0f, lon = 0.0f, alt = 0.0f;
        int got = 0;
        FILE *gp = popen("radio_test gps_status 2>/dev/null", "r");
        if (gp) {
            char gline[256];
            while (fgets(gline, sizeof(gline), gp)) {
                if (sscanf(gline,
                           "gps_status: override=%d uart_ok=%d fix=%d sats=%d "
                           "lat=%f lon=%f alt=%f",
                           &override_active, &uart_ok, &fix_valid, &sats,
                           &lat, &lon, &alt) == 7) {
                    got = 1;
                }
            }
            pclose(gp);
        }
        if (!got) {
            printf("(no response from gps_status -- rpmsg/MCU not reachable)\n");
        } else if (!uart_ok) {
            printf("Real receiver:       not connected (UART0 silent)\n");
        } else if (!fix_valid) {
            printf("Real receiver:       connected, no satellite fix yet\n");
        } else {
            printf("Real receiver:       fix OK, %d satellite(s)\n", sats);
            printf("  Lat / Lon:         %.6f / %.6f\n", lat, lon);
            printf("  Altitude:          %.2f m\n", alt);
        }
        printf("Debug override (GPS_OVERRIDE): %s\n", override_active ? "ACTIVE" : "inactive");
    }
    printf("\n");
    printf("--- SENSOR TELEMETRY ---\n");

    float bme_t = 0.0f, bme_p = 0.0f, bme_h = 0.0f;
    float acc_x = 0.0f, acc_y = 0.0f, acc_z = 0.0f;
    FILE *p = popen("sensor_test all 2>/dev/null", "r");
    if (p) {
        char line[256];
        while (fgets(line, sizeof(line), p)) {
            sscanf(line, "bme280: temp=%f C press=%f hPa hum=%f %%RH", &bme_t, &bme_p, &bme_h);
            sscanf(line, "icm42670: accel[g]=%f,%f,%f", &acc_x, &acc_y, &acc_z);
        }
        pclose(p);
    }
    printf("Environmental Data:\n");
    printf("  Temperature:       %.2f C\n", bme_t);
    printf("  Pressure:          %.2f hPa\n", bme_p);
    printf("  Humidity:          %.2f %%RH\n", bme_h);
    printf("\n");
    printf("IMU State (raw):\n");
    printf("  Accel X/Y/Z:       %.3f / %.3f / %.3f\n", acc_x, acc_y, acc_z);
    printf("==================================================\n");
}

static void run_dashboard(void) {
    for (;;) {
        print_system_status_dashboard();
        printf("(M to return)\n");
        int key = read_key_timeout(2);
        if (key == 'm' || key == 'M') return;
        if (key == -1) return;
    }
}

static void run_spacecan(void) {
    spacecan_run(read_key_timeout);
}

static void run_debug_shell(void) {
    printf("\n" ANSI_YELLOW
        "==================================================\n"
        "  Welcome to the PWNCUBE Space Computer\n"
        "  Rockchip RV1106 -- Cortex-A7 (Linux) + RISC-V (RT-Thread)\n"
        "==================================================\n"
        ANSI_RESET
        "(type 'exit' to return to the console)\n");
    fflush(stdout);
    restore_termios();
    execlp("/bin/sh", "/bin/sh", (char *)NULL);
    /* only reached if execlp fails */
    perror("execlp /bin/sh");
    set_raw_mode();
}

static void run_quiet(void) {
    printf("\n(quiet mode -- press M to return)\n");
    for (;;) {
        int key = read_key_timeout(3600);
        if (key == 'm' || key == 'M' || key == -1) return;
    }
}

int main(void) {
    /* Unbuffered stdout: this tty (ttyGS0 over the USB gadget) doesn't
     * reliably get detected as line-buffered, so without this printf()
     * output from different calls (ours and the sensor_test/radio_test
     * children spawned via system()) can end up flushed out of order --
     * garbled/interleaved text on the other end. */
    setvbuf(stdout, NULL, _IONBF, 0);

    set_raw_mode();
    atexit(restore_termios);

    print_menu();

    for (;;) {
        int key = read_key_timeout(10);
        /* Idle at the menu: reprint every ~10s, matching FlatSat's
         * consoleWorker() auto-refresh -- so anyone connecting late (after
         * the one-time initial print already went out) still sees the
         * console appear on its own within a few seconds, no keypress
         * needed. */
        if (key == 0) { print_menu(); continue; }
        if (key == -1) break;   /* EOF on stdin -- gadget dropped, exit and let respawn restart us */
        switch (key) {
            case '1': run_dashboard(); print_menu(); break;
            case '2': run_spacecan();  print_menu(); break;
            case '3': run_debug_shell(); return 0; /* execlp replaces us on success */
            case '4': run_quiet();     print_menu(); break;
            case 'm': case 'M': print_menu(); break;
            default: break;
        }
    }
    return 0;
}
