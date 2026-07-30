/* See spacecan_sim.h for what this ports and from where. */
#include "spacecan_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define SC_ERROR_INCREMENT 8
#define SC_BUS_OFF_THRESHOLD 256

#define SC_FLOOD_WINDOW_MS 1000
#define SC_FLOOD_SAMPLE_COUNT 5

#define ANSI_RESET   "\x1b[0m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_RED     "\x1b[31m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_MAGENTA "\x1b[35m"

typedef struct {
    uint32_t can_id;
    uint8_t dlc;
    uint8_t buffer[SC_MAX_DATA_LEN];
} spacecan_frame_t;

typedef struct {
    uint8_t active;
    uint8_t total_frames;
    uint32_t last_can_id;
    uint8_t last_seq;
    uint8_t received_mask[SC_MAX_FRAGMENTS];
    uint8_t fragment_sizes[SC_MAX_FRAGMENTS];
    uint8_t buffer[SC_MAX_PACKET_SIZE];
    uint32_t last_update_ms;
} spacecan_reassembly_ctx_t;

typedef struct {
    uint8_t node_id;
    const char *name;
    uint16_t error_count;
    bool bus_off;
} sc_node_state_t;

static spacecan_reassembly_ctx_t s_reassembly = {0};
/* Independent simulated state -- NOT the real thruster/battery from the
 * CCSDS CommandService. See header. */
static uint8_t s_thruster0 = 0, s_thruster1 = 0;
static uint8_t s_battery_charge = 80;

static sc_node_state_t s_nodes[3] = {
    {SC_NODE_MOTOR, "MOTOR", 0, false},
    {SC_NODE_BATTERY, "BATTERY", 0, false},
    {SC_NODE_SENSOR, "SENSOR", 0, false},
};

static uint32_t s_recent_frames[SC_FLOOD_SAMPLE_COUNT] = {0};
static uint8_t s_recent_frame_idx = 0;
static uint8_t s_recent_frame_count = 0;
static bool s_bus_congested = false;

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static sc_node_state_t *scFindNode(uint8_t node_id) {
    for (int i = 0; i < 3; i++) {
        if (s_nodes[i].node_id == node_id) return &s_nodes[i];
    }
    return NULL;
}

/* ---------- Reassembly (findings #26/#27/#28, ported as-is) ---------- */

static void scReassemblyReset(spacecan_reassembly_ctx_t *ctx) {
    ctx->active = 0;
    memset(ctx->received_mask, 0, sizeof(ctx->received_mask));
}

static int scReassemblyPackets(spacecan_reassembly_ctx_t *ctx,
                                const spacecan_frame_t *frame, uint32_t now,
                                uint8_t *out_buffer, size_t *out_len) {
    if (frame->dlc < 2) return -1;

    uint8_t total = frame->buffer[0] + 1;
    uint8_t seq = frame->buffer[1];

    if (total == 0 || total > SC_MAX_FRAGMENTS) return -1;

    if (!ctx->active) {
        ctx->active = 1;
        ctx->total_frames = total;
        memset(ctx->received_mask, 0, sizeof(ctx->received_mask));
    }

    if (now - ctx->last_update_ms > SC_REASSEMBLY_TIMEOUT_MS) {
        scReassemblyReset(ctx);
    }

    if (seq >= ctx->total_frames) return -1;

    size_t payload_len = frame->dlc - SC_PAYLOAD_HEADER_LEN;

    if (frame->can_id == ctx->last_can_id && frame->buffer[1] == ctx->last_seq) {
        return 0;
    }

    /* No check that frame->can_id matches earlier fragments in this session
     * (#27), and no bound on seq*SC_MAX_CHUNK_LEN+payload_len against
     * sizeof(ctx->buffer) (#26) -- intentionally mirrors the reference. */
    ctx->last_can_id = frame->can_id;
    ctx->last_seq = frame->buffer[1];
    ctx->last_update_ms = now;
    ctx->received_mask[seq] = 1;
    ctx->fragment_sizes[seq] = (uint8_t)payload_len;

    size_t offset = (size_t)seq * SC_MAX_CHUNK_LEN;
    memcpy(&ctx->buffer[offset], &frame->buffer[SC_PAYLOAD_HEADER_LEN], payload_len);

    size_t final_size = 0;
    for (int i = 0; i < ctx->total_frames; i++) {
        if (!ctx->received_mask[i]) return 0;
        final_size += ctx->fragment_sizes[i];
    }

    /* #28: no checksum/integrity check on the reassembled packet before
     * handing it to the caller. */
    memcpy(out_buffer, ctx->buffer, final_size);
    *out_len = final_size;
    scReassemblyReset(ctx);
    return 1;
}

/* ---------- #29: arbitration/flood DoS ---------- */

static void scTrackFrameRate(void) {
    const uint32_t now = now_ms();
    s_recent_frames[s_recent_frame_idx] = now;
    s_recent_frame_idx = (uint8_t)((s_recent_frame_idx + 1) % SC_FLOOD_SAMPLE_COUNT);

    uint8_t count_in_window = 0;
    for (int i = 0; i < SC_FLOOD_SAMPLE_COUNT; i++) {
        if (s_recent_frames[i] != 0 && (now - s_recent_frames[i]) <= SC_FLOOD_WINDOW_MS) {
            count_in_window++;
        }
    }
    s_recent_frame_count = count_in_window;
    s_bus_congested = (count_in_window >= SC_FLOOD_SAMPLE_COUNT);
}

/* ---------- #23 (no origin auth) + #30 (bus-off attack) ---------- */

static void scDispatchFrame(const spacecan_frame_t *frame) {
    const uint16_t function = frame->can_id & SC_CAN_FUNCTION_MASK;
    const uint8_t node_id = frame->can_id & SC_CAN_NODE_MASK;

    if (function != SC_CANID_REQ) {
        printf("[SPACECAN] reply-family frame for node 0x%02X (no action)\n", node_id);
        return;
    }

    sc_node_state_t *node = scFindNode(node_id);
    if (node == NULL) {
        printf("[SPACECAN] frame targets unknown node 0x%02X, ignored\n", node_id);
        return;
    }

    if (node->bus_off) {
        printf("[SPACECAN] node 0x%02X is BUS-OFF (silently disconnected), frame dropped\n", node_id);
        return;
    }

    if (frame->dlc < 2 || frame->buffer[0] != 0x01) {
        /* Malformed/unrecognized command treated as a CAN protocol error.
         * Real CAN controllers add 8 to the TX/RX error counter per error;
         * at 256 the node force-disconnects (bus-off, CAN 2.0 TEC/REC) --
         * lets an attacker silence a node with zero valid/authenticated
         * commands. */
        node->error_count = (uint16_t)(node->error_count + SC_ERROR_INCREMENT);
        printf("[SPACECAN] node 0x%02X protocol error (err_count=%u/%u)\n",
               node_id, node->error_count, SC_BUS_OFF_THRESHOLD);
        if (node->error_count >= SC_BUS_OFF_THRESHOLD) {
            node->bus_off = true;
            printf("[SPACECAN] node 0x%02X entered BUS-OFF -- silently disconnected (DoS)\n", node_id);
        }
        return;
    }

    node->error_count = 0; /* a valid frame clears the error counter */

    /* #23: no check that this frame actually originated from node 0
     * (controller) -- any injected request-family frame is trusted by
     * node_id alone. */
    if (node_id == SC_NODE_MOTOR) {
        s_thruster0 = frame->buffer[1];
        printf("[SPACECAN] MOTOR node 0x%02X accepted: thruster0 power = %u (no origin check)\n",
               node_id, frame->buffer[1]);
    } else if (node_id == SC_NODE_BATTERY) {
        s_battery_charge = frame->buffer[1];
        printf("[SPACECAN] BATTERY node 0x%02X accepted: charge = %u%% (no origin check)\n",
               node_id, frame->buffer[1]);
    } else {
        printf("[SPACECAN] SENSOR node 0x%02X has no writable commands\n", node_id);
    }
}

/* ---------- Status panel ---------- */

static void scPrintBar(size_t width) {
    char bar[96];
    if (width > sizeof(bar) - 1) width = sizeof(bar) - 1;
    memset(bar, '=', width);
    bar[width] = '\0';
    printf("%s\n", bar);
}

static void scPrintStatus(void) {
    const char *title = "--- SPACECAN BUS MONITOR (SIMULATED, IN-FIRMWARE) ---";
    char line_motor[128], line_battery[128], line_sensor[128], line_bus[128], line_reasm[128];
    const char *line_cmds = "Commands: SC INJECT <id_hex> <bytes...> | SC STATUS | SC RESET <id_hex> | M";

    snprintf(line_motor, sizeof(line_motor),
             "MOTOR   (0x%02X) %-11s thruster0=%-3u thruster1=%-3u err=%u/%u",
             SC_NODE_MOTOR, s_nodes[0].bus_off ? "BUS-OFF" : "OPERATIONAL",
             s_thruster0, s_thruster1, s_nodes[0].error_count, SC_BUS_OFF_THRESHOLD);

    snprintf(line_battery, sizeof(line_battery),
             "BATTERY (0x%02X) %-11s charge=%-3u%% err=%u/%u", SC_NODE_BATTERY,
             s_nodes[1].bus_off ? "BUS-OFF" : "OPERATIONAL", s_battery_charge,
             s_nodes[1].error_count, SC_BUS_OFF_THRESHOLD);

    snprintf(line_sensor, sizeof(line_sensor),
             "SENSOR  (0x%02X) %-11s err=%u/%u (see menu option 1 for real readings)",
             SC_NODE_SENSOR, s_nodes[2].bus_off ? "BUS-OFF" : "OPERATIONAL",
             s_nodes[2].error_count, SC_BUS_OFF_THRESHOLD);

    snprintf(line_bus, sizeof(line_bus), "Bus arbitration: %s frames/1s=%u",
             s_bus_congested ? "CONGESTED (reply-family frames losing arbitration)" : "clear",
             s_recent_frame_count);

    snprintf(line_reasm, sizeof(line_reasm),
             "Reassembly ctx: active=%u total_frames=%u last_seq=%u last_can_id=0x%03X",
             s_reassembly.active, s_reassembly.total_frames, s_reassembly.last_seq,
             s_reassembly.last_can_id);

    size_t width = strlen(title);
    const char *lines[] = {line_motor, line_battery, line_sensor, line_bus, line_reasm, line_cmds};
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        size_t len = strlen(lines[i]);
        if (len > width) width = len;
    }

    scPrintBar(width);
    printf("%s\n", title);
    printf("%s%s%s\n", s_nodes[0].bus_off ? ANSI_RED : ANSI_CYAN, line_motor, ANSI_RESET);
    printf("%s%s%s\n", s_nodes[1].bus_off ? ANSI_RED : ANSI_YELLOW, line_battery, ANSI_RESET);
    printf("%s%s%s\n", s_nodes[2].bus_off ? ANSI_RED : ANSI_MAGENTA, line_sensor, ANSI_RESET);
    printf("\n");
    printf("%s%s%s\n", s_bus_congested ? ANSI_YELLOW : ANSI_GREEN, line_bus, ANSI_RESET);
    printf("%s\n", line_reasm);
    printf("\n%s\n", line_cmds);
    scPrintBar(width);
}

/* ---------- Command handling ---------- */

static void scHandleInject(char *args) {
    char *saveptr = NULL;
    char *tok = strtok_r(args, " ", &saveptr);
    if (tok == NULL) {
        printf("[SPACECAN] usage: SC INJECT <can_id_hex> <byte0_hex> [byte1_hex ...]\n");
        return;
    }

    spacecan_frame_t frame = {0};
    frame.can_id = (uint32_t)strtoul(tok, NULL, 16);

    uint8_t count = 0;
    while ((tok = strtok_r(NULL, " ", &saveptr)) != NULL && count < SC_MAX_DATA_LEN) {
        frame.buffer[count++] = (uint8_t)strtoul(tok, NULL, 16);
    }
    frame.dlc = count;

    printf("[SPACECAN] INJECT can_id=0x%03X dlc=%u data=", frame.can_id, frame.dlc);
    for (int i = 0; i < frame.dlc; i++) printf("%02X ", frame.buffer[i]);
    printf("\n");

    scTrackFrameRate();

    const bool is_reply_family = (frame.can_id & SC_CAN_FUNCTION_MASK) == SC_CANID_REP;
    if (s_bus_congested && is_reply_family) {
        printf("[SPACECAN] frame LOST ARBITRATION (bus congested by higher-priority "
               "request-family traffic) -- dropped\n");
        scPrintStatus();
        return;
    }

    scDispatchFrame(&frame);

    uint8_t reassembled[SC_MAX_PACKET_SIZE];
    size_t reassembled_len = 0;
    int ret = scReassemblyPackets(&s_reassembly, &frame, now_ms(), reassembled, &reassembled_len);
    if (ret == 1) {
        printf("[SPACECAN] packet reassembled, %u bytes:\n", (unsigned)reassembled_len);
        for (size_t i = 0; i < reassembled_len; i++) printf("%02X ", reassembled[i]);
        printf("\n");
    } else if (ret == -1) {
        printf("[SPACECAN] fragment rejected (bad dlc/seq/total)\n");
    } else {
        printf("[SPACECAN] fragment accepted, waiting for the rest\n");
    }

    scPrintStatus();
}

static void scHandleReset(char *args) {
    while (*args == ' ') args++;
    uint8_t node_id = (uint8_t)(strtoul(args, NULL, 16) & SC_CAN_NODE_MASK);
    sc_node_state_t *node = scFindNode(node_id);
    if (node == NULL) {
        printf("[SPACECAN] unknown node 0x%02X\n", node_id);
        return;
    }
    node->bus_off = false;
    node->error_count = 0;
    printf("[SPACECAN] node 0x%02X manually reset (bus-off cleared)\n", node_id);
    scPrintStatus();
}

static void scProcessLine(char *line) {
    while (*line == ' ') line++;
    size_t len = strlen(line);
    while (len > 0 && line[len - 1] == ' ') line[--len] = '\0';
    if (len == 0) {
        printf("SPACECAN> ");
        return;
    }

    if (strcmp(line, "SC STATUS") == 0) {
        scPrintStatus();
    } else if (strncmp(line, "SC INJECT ", 10) == 0) {
        scHandleInject(line + 10);
    } else if (strncmp(line, "SC RESET ", 9) == 0) {
        scHandleReset(line + 9);
    } else {
        printf("[SPACECAN] unknown command (try: SC STATUS, SC INJECT <id> <bytes>, SC RESET <id>, or M for menu)\n");
    }
    printf("SPACECAN> ");
}

void spacecan_run(int (*read_key)(int timeout_s)) {
    static char line_buf[128];
    size_t line_len = 0;

    printf("\n" ANSI_MAGENTA "--- SPACECAN BUS CONSOLE ---" ANSI_RESET "\n");
    scPrintStatus();
    printf("SPACECAN> ");

    for (;;) {
        int key = read_key(3600);
        if (key <= 0) return; /* timeout or EOF -- bail to the main menu */
        char c = (char)key;

        if (line_len == 0 && (c == 'm' || c == 'M')) return;

        if (c == '\r' || c == '\n') {
            printf("\n");
            line_buf[line_len] = '\0';
            scProcessLine(line_buf);
            line_len = 0;
            continue;
        }
        if (c == 0x7F || c == 0x08) { /* backspace/delete */
            if (line_len > 0) {
                line_len--;
                printf("\b \b");
            }
            continue;
        }
        putchar(c);
        if (line_len + 1 < sizeof(line_buf)) {
            line_buf[line_len++] = c;
        }
    }
}
