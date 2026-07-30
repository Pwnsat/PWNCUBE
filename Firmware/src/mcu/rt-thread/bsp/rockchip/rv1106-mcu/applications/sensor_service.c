/*
 * CubeSat — SensorService (BME280 + ICM-42670 on I2C0) over rpmsg. Second
 * migrated peripheral service on the MCU (docs/migration/implementation/90-mcu-config-replication.md §10 checklist),
 * built to the same shape as radio_service.c.
 *
 * Channel: "rpmsg-sensor" (ept 0x4006) on the shared rpmsg-lite instance. Linux
 * binds it to rpmsg_char in rcS -> /dev/rpmsg_ctrl1, RPMSG_CREATE_EPT(dst=0x4006)
 * -> /dev/rpmsg1. Client: src/sensor-client/sensor_test.c.
 *
 * Protocol v1 — request [0]=cmd [1..]=args; reply mirrors cmd with [1]=err
 * (0 ok; 0x11 = sensor/I2C error; 0xEE = unknown cmd):
 *   0x01 PING              -> [0x01,0,'S','E','N','S',ver]
 *   0x02 WHOAMI  chip      -> [0x02,err,id]        chip: 0=BME280,1=ICM42670
 *   0x10 BME280_READ       -> [0x10,err, t(i32 LE m°C), p(u32 LE Pa), h(u32 LE m%RH)]
 *   0x20 IMU_READ          -> [0x20,err, ax,ay,az,gx,gy,gz,temp (7 x i16 LE)]
 *
 * Threading: handlers run in the rpmsg poll thread (ping_echo.c). Reads are
 * short, synchronous I2C polls. Replies are DEFERRED (queued in the callback,
 * flushed by sensor_service_poll() after the vring drain) — same rule as radio.
 *
 * Diag markers: 0xff6ff888 = last cmd, 0xff6ff88c = last handler err.
 */
#include <rthw.h>
#include <rtthread.h>

#include "ipc_test_cfg.h"

#if defined(RT_USING_RPMSG_LITE) && !(defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1))

#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "sensor_port.h"
#include "bme280.h"
#include "icm42670.h"

#define SENSOR_EPT_ADDR   (0x4006U)
#define SENSOR_EPT_NAME   "rpmsg-sensor"

#define SENSOR_CMD_PING        (0x01U)
#define SENSOR_CMD_WHOAMI      (0x02U)
#define SENSOR_CMD_BME_READ    (0x10U)
#define SENSOR_CMD_IMU_READ    (0x20U)

#define SENSOR_ERR_IO          (0x11U)

#define MARK(addr, val)  (*(volatile unsigned int *)(addr) = (unsigned int)(val))

static struct rpmsg_lite_instance *s_ss_inst;
static struct rpmsg_lite_endpoint *s_ss_ept;
static uint32_t s_ss_host_addr;
static bool     s_i2c_up;

/* Deferred reply (same rationale as radio_service.c). */
static uint8_t  s_ss_rsp[24];
static uint32_t s_ss_rsp_len;
static volatile int s_ss_rsp_pending;

static void put_i32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_i16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static int sensor_lazy_i2c(void)
{
    if (s_i2c_up)
        return 0;
    if (sensor_i2c_init() != 0)
        return -1;
    s_i2c_up = true;
    return 0;
}

/* Flush a deferred reply — called each poll tick from ping_echo_thread. */
void sensor_service_poll(void)
{
    if (s_ss_rsp_pending) {
        (void)rpmsg_lite_send(s_ss_inst, s_ss_ept, s_ss_host_addr,
                              (char *)s_ss_rsp, s_ss_rsp_len, RL_DONT_BLOCK);
        s_ss_rsp_pending = 0;
    }
}

static int32_t sensor_rx(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    const uint8_t *req = (const uint8_t *)payload;
    uint8_t rsp[24];
    uint32_t rsp_len = 0;
    int err;

    (void)priv;
    if (payload_len < 1U)
        return RL_RELEASE;

    s_ss_host_addr = src;
    MARK(0xff6ff888, 0xAE000000U | req[0]);

    rsp[0] = req[0];

    switch (req[0]) {
    case SENSOR_CMD_PING:
        rsp[1] = 0;
        rsp[2] = 'S'; rsp[3] = 'E'; rsp[4] = 'N'; rsp[5] = 'S';
        rsp[6] = 0x01;
        rsp_len = 7;
        break;

    case SENSOR_CMD_WHOAMI: {
        uint8_t chip, id = 0xFF;

        if (payload_len < 2U) return RL_RELEASE;
        chip = req[1];
        err = sensor_lazy_i2c();
        if (err == 0)
            err = (chip == 0) ? bme280_read_id(&id) : icm42670_read_id(&id);
        rsp[1] = err ? SENSOR_ERR_IO : 0;
        rsp[2] = id;
        rsp_len = 3;
        break;
    }

    case SENSOR_CMD_BME_READ: {
        struct bme280_sample s = {0};

        err = sensor_lazy_i2c();
        if (err == 0)
            err = bme280_read(&s);
        rsp[1] = err ? SENSOR_ERR_IO : 0;
        put_i32(&rsp[2],  s.temp_mC);
        put_i32(&rsp[6],  (int32_t)s.press_Pa);
        put_i32(&rsp[10], (int32_t)s.hum_m_pct);
        rsp_len = 14;
        break;
    }

    case SENSOR_CMD_IMU_READ: {
        struct icm42670_sample s = {0};

        err = sensor_lazy_i2c();
        if (err == 0)
            err = icm42670_read(&s);
        rsp[1] = err ? SENSOR_ERR_IO : 0;
        put_i16(&rsp[2],  s.accel[0]);
        put_i16(&rsp[4],  s.accel[1]);
        put_i16(&rsp[6],  s.accel[2]);
        put_i16(&rsp[8],  s.gyro[0]);
        put_i16(&rsp[10], s.gyro[1]);
        put_i16(&rsp[12], s.gyro[2]);
        put_i16(&rsp[14], s.temp);
        rsp_len = 16;
        break;
    }

    default:
        rsp[1] = 0xEE;
        rsp_len = 2;
        break;
    }

    MARK(0xff6ff88c, 0xAE100000U | rsp[1]);
    memcpy(s_ss_rsp, rsp, rsp_len);
    s_ss_rsp_len = rsp_len;
    s_ss_rsp_pending = 1;
    return RL_RELEASE;
}

/* Called by ping_echo.c once the link is up (after the radio service attaches). */
int sensor_service_attach(struct rpmsg_lite_instance *inst)
{
    s_ss_inst = inst;
    s_ss_ept = rpmsg_lite_create_ept(inst, SENSOR_EPT_ADDR, sensor_rx, RT_NULL);
    if (s_ss_ept == RT_NULL)
        return -1;
    rpmsg_ns_announce(inst, s_ss_ept, SENSOR_EPT_NAME, RL_NS_CREATE);
    MARK(0xff6ff888, 0xAE000001U);   /* service attached+announced */
    return 0;
}

#endif /* RT_USING_RPMSG_LITE && !IPC_RAW_MBOX_TEST */
