/*
 * CubeSat Paso 0 — RPMsg PING/ECHO service on the RV1106 RISC-V (HPMCU).
 *
 * Brings up an rpmsg-lite REMOTE endpoint and echoes every payload back to the
 * sender. Used to validate the A7<->RISC-V transport before migrating drivers.
 * The announced channel name "rpmsg-ap3-ch0" matches the in-kernel test driver
 * (drivers/rpmsg/rockchip_rpmsg_test.c) so Linux probes it automatically.
 *
 * TRANSPORT SHAPE (measured on board, see docs/migration/implementation/70-mailbox-loopback-test.md):
 *   - MCU->A7 (B2A) mailbox works: platform_notify() kicks Linux normally.
 *   - A7->MCU (A2B) mailbox payload/status regs are UNREADABLE by the SCR1, so
 *     there is NO usable RX doorbell. RX is done by POLLING: this thread drains
 *     both virtqueues in DDR (coherent; MCU runs uncached) every few ms, gated
 *     behind the first int_mux kick so we never touch vrings before Linux has
 *     initialized them. The MCU still ACKs A2B_STATUS (the write does land even
 *     though reads return 0) so the Linux mailbox TX path never sees the
 *     channel stuck busy.
 *
 * SINGLE-THREADED BRING-UP: init, link-up wait, announce and the RX poll all
 * run in this one thread — the previous split (separate poller thread started
 * alongside init) could notify a half-initialized instance.
 *
 * Shared-memory base comes from the linker (__linux_share_rpmsg_start__), which
 * MUST match the Linux device-tree rpmsg "reg"/reserved-memory base.
 * See docs/migration/implementation/60-ipc-bringup.md.
 *
 * Diagnostic markers (hpmcu_sram, readable from Linux via devmem — the
 * 0x800-0x83c range belongs to the boot/startup markers, this app uses 0x840+):
 *   0xff6ff840  app stage:   0xCAFE0101 init / 0102 remote_init ok /
 *               0103 link up / 0104 announced
 *   0xff6ff844  poll-loop heartbeat 0xCAB0xxxx (advances forever)
 *   0xff6ff848  last completed poll stage 0xE1..0xE4 (freeze bisect, set by
 *               rpmsg_rv1106_rx_poll in rpmsg_platform.c)
 *   0xff6ff84c  last int_mux status0 seen
 *   0xff6ff850  echo counter 0xEC0xxxxx (one per received message)
 *   0xff6ff854  last rpmsg_lite_send() result (0 = RL_SUCCESS)
 */
#include <rthw.h>
#include <rtthread.h>   /* pulls rtconfig.h so RT_USING_RPMSG_LITE becomes visible */

#include "ipc_test_cfg.h"

/* The raw-mailbox loopback diagnostic (ipc_mbox_test.c) and this rpmsg app are
 * mutually exclusive: only one may drive the mailbox at a time. When the raw
 * test is selected, compile this app out entirely. */
#if defined(RT_USING_RPMSG_LITE) && !(defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1))

#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "rpmsg_platform.h"

/* Must match Linux rockchip,link-id = <0x04> (master=A7=0, remote=MCU=4). */
#define PING_MASTER_ID    (0U)
#define PING_REMOTE_ID    (4U)
#define PING_EPT_ADDR     (0x4004U)
#define PING_EPT_NAME     "rpmsg-ap3-ch0"

#define MARK(addr, val)   (*(volatile unsigned int *)(addr) = (unsigned int)(val))

extern uint32_t __linux_share_rpmsg_start__[];
#define PING_SHMEM_BASE   ((void *)&__linux_share_rpmsg_start__)

extern void rpmsg_rv1106_rx_poll(void);
extern void rpmsg_rv1106_rx_wait(uint32_t ms);   /* block until an A7 kick or `ms` */

static struct rpmsg_lite_instance *s_inst;
static struct rpmsg_lite_endpoint *s_ept;
static unsigned int s_echo_count;

static int32_t ping_echo_rx(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    int32_t ret;

    (void)priv;
    MARK(0xff6ff850, 0xEC000000 | (++s_echo_count & 0xFFFFF));
    /* Echo the received payload straight back to the sender. Non-blocking: the
     * host pre-fills our TX vq with free buffers, so a buffer should always be
     * available; if not, record and drop rather than deadlock the poll loop. */
    ret = rpmsg_lite_send(s_inst, s_ept, src, (char *)payload, payload_len, RL_DONT_BLOCK);
    MARK(0xff6ff854, (unsigned int)ret);
    return RL_RELEASE;
}

static void ping_echo_thread(void *arg)
{
    volatile unsigned int n = 0;

    (void)arg;
    MARK(0xff6ff840, 0xCAFE0101);

    s_inst = rpmsg_lite_remote_init(PING_SHMEM_BASE,
                                    RL_PLATFORM_SET_LINK_ID(PING_MASTER_ID, PING_REMOTE_ID),
                                    RL_NO_FLAGS);
    if (s_inst == RT_NULL)
        return;
    MARK(0xff6ff840, 0xCAFE0102);

    /* Link-up wait, poll-driven: the tx-vq callback that sets link_state only
     * runs when WE drain the vq (no RX doorbell on this SoC), so poll here
     * instead of blocking in rpmsg_lite_wait_for_link_up(). */
    while (s_inst->link_state == 0U)
    {
        MARK(0xff6ff844, 0xCAB00000 | (n++ & 0xFFFFF));
        rpmsg_rv1106_rx_poll();
        rt_thread_mdelay(2);
    }
    MARK(0xff6ff840, 0xCAFE0103);

    s_ept = rpmsg_lite_create_ept(s_inst, PING_EPT_ADDR, ping_echo_rx, RT_NULL);
    if (s_ept == RT_NULL)
        return;

    rpmsg_ns_announce(s_inst, s_ept, PING_EPT_NAME, RL_NS_CREATE);
    MARK(0xff6ff840, 0xCAFE0104);

    /* Attach the migrated device services (extra endpoints on the same
     * instance; their callbacks are dispatched from this thread's vring drain
     * below). RadioService = SX1262 x2 (SPI); SensorService = BME280 +
     * ICM-42670 (I2C0). TelemetryService = periodic sensor SPP TX.
     * CommandService = uplink RX + TC dispatch. */
    {
        extern int radio_service_attach(struct rpmsg_lite_instance *inst);
        extern int sensor_service_attach(struct rpmsg_lite_instance *inst);
        extern int telemetry_service_attach(struct rpmsg_lite_instance *inst);
        extern int telemetry_service_init(void);
        extern int command_service_attach(struct rpmsg_lite_instance *inst);
        extern int command_service_init_default(void);
        (void)radio_service_attach(s_inst);
        (void)sensor_service_attach(s_inst);
        (void)telemetry_service_attach(s_inst);
        (void)telemetry_service_init();
        (void)command_service_attach(s_inst);
        (void)command_service_init_default();
    }

    /* Steady-state: drain the vrings whenever the A7 kicks the mailbox (the ISR
     * wakes us via rpmsg_rv1106_rx_wait), with a 2ms timeout that also runs the
     * periodic service work (radio DIO1 poll, deferred B2A reply/event flush). */
    while (1)
    {
        extern void radio_service_poll(void);
        extern void sensor_service_poll(void);
        extern void telemetry_service_poll(void);
        extern void telemetry_service_poll_flush(void);
        extern void command_service_poll_flush(void);
        extern void command_service_telemetry_worker(void);

        MARK(0xff6ff844, 0xCAB00000 | (n++ & 0xFFFFF));
        rpmsg_rv1106_rx_poll();
        telemetry_service_poll_flush();
        radio_service_poll();      /* must run before flush — sets s_evt_pending */
        command_service_poll_flush();
        sensor_service_poll();
        command_service_telemetry_worker();
        telemetry_service_poll();
        /* Interrupt-driven RX: wake immediately when the A7 kicks the mailbox
         * (rpmsg_mbox_isr releases the RX semaphore), else fall through after
         * 2ms to keep the periodic service work above on cadence. Replaces the
         * old rt_thread_mdelay(2) poll so RX latency is ~us, not up to 2ms. */
        rpmsg_rv1106_rx_wait(2);
    }
}

int ping_echo_init(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("rpmsg_echo", ping_echo_thread, RT_NULL, 3072, 18, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
    return 0;
}
INIT_APP_EXPORT(ping_echo_init);

#endif /* RT_USING_RPMSG_LITE && !IPC_RAW_MBOX_TEST */
