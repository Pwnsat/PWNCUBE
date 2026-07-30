/*
 * CubeSat — TelemetryService (thin rpmsg control interface).
 *
 * FlatSat port: the actual telemetry worker runs in command_service.c
 * (command_service_telemetry_worker). This file only provides the rpmsg
 * endpoint for Linux control (start/stop/status/config/monitor).
 *
 * Controlled over rpmsg (endpoint 0x4007 "rpmsg-telemetry") from Linux.
 */

#include <rthw.h>
#include <rtthread.h>

#include "ipc_test_cfg.h"

#if defined(RT_USING_RPMSG_LITE) && !(defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1))

#include <string.h>
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"

#define TELEM_EPT_ADDR   (0x4007U)
#define TELEM_EPT_NAME   "rpmsg-telemetry"

#define TELEM_CMD_PING        (0x01U)
#define TELEM_CMD_START       (0x02U)
#define TELEM_CMD_STOP        (0x03U)
#define TELEM_CMD_STATUS      (0x04U)
#define TELEM_CMD_CONFIG      (0x05U)
#define TELEM_CMD_MONITOR     (0x10U)

#define MARK(addr, val)   (*(volatile unsigned int *)(addr) = (unsigned int)(val))

static struct rpmsg_lite_instance   *s_inst;
static struct rpmsg_lite_endpoint   *s_ept;
static uint32_t                      s_host_addr;
static uint8_t                       s_rsp_buf[24];
static uint32_t                      s_rsp_len;
static volatile int                  s_rsp_pending;

static bool s_running;
static bool s_monitor_enabled;
static uint8_t s_monitor_buf[512];
static uint16_t s_monitor_len;
static volatile int s_monitor_pending;

void telemetry_service_poll_flush(void)
{
    if (s_rsp_pending) {
        (void)rpmsg_lite_send(s_inst, s_ept, s_host_addr,
                              (char *)s_rsp_buf, s_rsp_len, RL_DONT_BLOCK);
        s_rsp_pending = 0;
    }
    if (s_monitor_pending) {
        (void)rpmsg_lite_send(s_inst, s_ept, s_host_addr,
                              (char *)s_monitor_buf, s_monitor_len, RL_DONT_BLOCK);
        s_monitor_pending = 0;
    }
}

void telemetry_service_poll(void)
{
    /* Telemetry worker runs in command_service.c now */
}

static int32_t telem_rx(void *payload, uint32_t payload_len,
                         uint32_t src, void *priv)
{
    const uint8_t *req = (const uint8_t *)payload;
    uint8_t rsp[24];
    uint32_t rsp_len = 0;

    (void)priv;
    if (payload_len < 1U) return RL_RELEASE;

    s_host_addr = src;
    rsp[0] = req[0];

    switch (req[0]) {

    case TELEM_CMD_PING:
        rsp[1] = 0;
        rsp[2] = 'T'; rsp[3] = 'E'; rsp[4] = 'L'; rsp[5] = 'M';
        rsp[6] = 0x01;
        rsp_len = 7;
        break;

    case TELEM_CMD_START:
        s_running = true;
        rsp[1] = 0; rsp_len = 2;
        break;

    case TELEM_CMD_STOP:
        s_running = false;
        rsp[1] = 0; rsp_len = 2;
        break;

    case TELEM_CMD_STATUS:
        rsp[1] = 0;
        rsp[2] = s_running ? 1 : 0;
        rsp_len = 3;
        break;

    case TELEM_CMD_CONFIG:
        rsp[1] = 0; rsp_len = 2;
        break;

    case TELEM_CMD_MONITOR:
        s_monitor_enabled = (payload_len >= 2 && req[1] != 0);
        rsp[1] = 0; rsp_len = 2;
        break;

    default:
        rsp[1] = 0xEE;
        rsp_len = 2;
        break;
    }

    memcpy(s_rsp_buf, rsp, rsp_len);
    s_rsp_len = rsp_len;
    s_rsp_pending = 1;
    return RL_RELEASE;
}

void telemetry_push_monitor(const uint8_t *data, uint16_t len)
{
    if (!s_monitor_enabled) return;
    if (len > sizeof(s_monitor_buf)) len = sizeof(s_monitor_buf);
    memcpy(s_monitor_buf, data, len);
    s_monitor_len = len;
    s_monitor_pending = 1;
}

int telemetry_service_init(void)
{
    return 0;
}

int telemetry_service_attach(struct rpmsg_lite_instance *inst)
{
    s_inst = inst;
    s_ept = rpmsg_lite_create_ept(inst, TELEM_EPT_ADDR, telem_rx, RT_NULL);
    if (s_ept == RT_NULL) return -1;
    rpmsg_ns_announce(inst, s_ept, TELEM_EPT_NAME, RL_NS_CREATE);
    return 0;
}

#endif /* RT_USING_RPMSG_LITE && !IPC_RAW_MBOX_TEST */