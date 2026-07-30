/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2022 Rockchip Electronics Co., Ltd.
 *
 * RV1106 RISC-V (HPMCU / Syntacore SCR1) rpmsg-lite platform port — CubeSat.
 *
 * Derived from the RK3568 port, but rewritten for the RV1106:
 *   - Single RISC-V core acting as the rpmsg REMOTE (Linux/A7 is the master).
 *     There is no SMP / no interrupt affinity / no HAL_CPU_TOPOLOGY here.
 *   - One mailbox (MBOX @0xff5c0000) with a SINGLE pair of CPU interrupts
 *     (MAILBOX0_AP_IRQn / MAILBOX0_BB_IRQn) shared across all 4 channels,
 *     instead of RK3568's 8 per-channel IRQs.
 *
 * !!! UNVERIFIED ON HARDWARE !!!
 * The mailbox channel index, link-id encoding, A2B/B2A direction and the
 * env_isr()<->virtqueue mapping must be co-tuned with the Linux-side
 * rockchip_rpmsg DT (mboxes/rockchip,link-id) during on-board bring-up.
 * See docs/migration/implementation/60-ipc-bringup.md.
 */
#include <stdio.h>
#include <string.h>

#include "rpmsg_platform.h"
#include "rpmsg_env.h"

#include <rthw.h>
#include <rtthread.h>
#include "hal_base.h"

#if defined(RL_USE_ENVIRONMENT_CONTEXT) && (RL_USE_ENVIRONMENT_CONTEXT == 1)
#error "This RPMsg-Lite port requires RL_USE_ENVIRONMENT_CONTEXT set to 0"
#endif

/*
 * Mailbox channel used for the A7<->MCU rpmsg link. MUST match the channel(s)
 * the Linux rockchip_rpmsg node references via its "mboxes" property.
 * Mirrors the rk3562-amp.dtsi reference: Linux mboxes = <&mailbox 0 &mailbox 3>,
 * mbox-names "rpmsg-rx","rpmsg-tx". So Linux transmits A2B on ch3 and receives
 * B2A on ch0. The remote (this MCU), inited B2A, RECEIVES A2B on the registered
 * channel and SENDS B2A on the send channel:
 *   - RX channel (A2B from Linux, = Linux tx) -> 3
 *   - TX channel (B2A to   Linux, = Linux rx) -> 0
 * These MUST match the Linux rpmsg DT "mboxes" property.
 */
#define RL_RV1106_RX_CHAN  (3U)   /* receive A2B (master->remote); Linux "rpmsg-tx" */
#define RL_RV1106_TX_CHAN  (0U)   /* send    B2A (remote->master); Linux "rpmsg-rx" */

#define RL_MBOX_B2A 0   /* remote (this MCU) -> master (A7) */
#define RL_MBOX_A2B 1   /* master (A7) -> remote */

static int32_t isr_counter = 0;
static int32_t disable_counter = 0;
static int32_t first_notify = 0;
static void *platform_lock;
#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
static LOCK_STATIC_CONTEXT platform_lock_static_ctxt;
#endif
static rt_base_t s_isr_level;

static struct MBOX_REG *rl_pMBox = MBOX;   /* RV1106 names the instance "MBOX" */
static int32_t register_count = 0;

/*
 * Doorbell-less RX support.
 *
 * On this SCR1 the mailbox A2B_STATUS register reads back as 0 from the MCU
 * even though the A7 sees the pending bit, so HAL_MBOX_IrqHandler (which scans
 * A2B_STATUS) can never find the channel and never delivers the message. The
 * mailbox is only a doorbell anyway — the real payload travels in the vrings —
 * so we bypass A2B_STATUS entirely: a poll thread acks the doorbell and drains
 * both virtqueues directly. s_link_id is captured at init so we know which
 * vq_ids (RL_GET_VQ_ID) to service. See docs/migration/implementation/60-ipc-bringup.md.
 */
static uint32_t s_link_id = 0;
static volatile int32_t s_rx_ready = 0;
static volatile int32_t s_kicked   = 0;

/*
 * int_mux (SCR1 interrupt multiplexer) status. The mailbox BB line (vector 2)
 * appears as bit 2 of group-0 status. Unlike A2B_STATUS, the MCU CAN read this,
 * so we use it to tell whether the A7 has ever kicked us. We must NOT touch the
 * shared vrings before the A7 has set them up (doing so hangs Linux's virtio
 * probe at boot), so vring draining is gated behind the first observed kick.
 */
#define INT_MUX_STATUS0   (0xff5d0080U)
#define MBOX_BB_INTMUX_BIT (1UL << 2)

/* Latest int_mux status, exported for diagnostics. */
volatile unsigned int g_rv1106_intmux_dbg = 0;

/*
 * Interrupt-driven RX wake. The mailbox ISR releases s_rx_sem so the ping_echo
 * thread drains the vrings IMMEDIATELY on an A7 kick instead of waiting out its
 * 2 ms poll interval (RX latency drops from up to 2 ms to ~microseconds).
 *
 * env_isr() is STILL only ever called from that thread (see rpmsg_rv1106_rx_poll),
 * NOT from the ISR: the service RX callbacks (command_rx -> radio TX / reset /
 * mdelay, etc.) do blocking work that is illegal in ISR context, and calling
 * env_isr from both the ISR and the thread previously raced and corrupted the
 * link. The ISR only ACKs + wakes; the thread stays the sole vq owner.
 */
static struct rt_semaphore s_rx_sem;
static volatile int32_t    s_rx_sem_ready = 0;

/* Service both virtqueues for the established link (doorbell-less RX).
 * Stage marker at 0xff6ff848 (0xE1..0xE4) bisects any freeze to the exact
 * step; 0xff6ff84c mirrors the last int_mux status0 read. */
#define RXPOLL_STAGE(v)  (*(volatile unsigned int *)0xff6ff848 = (v))

void rpmsg_rv1106_rx_poll(void)
{
    unsigned int st;

    if (s_rx_ready == 0)
    {
        return;
    }

    RXPOLL_STAGE(0xE1);

    /* Gate: never touch the shared vrings before the first Linux kick (the
     * memory holds garbage until the host initializes it). Primary signal is
     * the mailbox ISR latch; the int_mux status read is a belt-and-braces
     * fallback for boots where the IPIC does not dispatch the vector. */
    if (s_kicked == 0)
    {
        st = *(volatile unsigned int *)INT_MUX_STATUS0;
        g_rv1106_intmux_dbg = st;
        *(volatile unsigned int *)0xff6ff84c = st;
        if ((st & MBOX_BB_INTMUX_BIT) != 0U)
        {
            s_kicked = 1;
            rl_pMBox->A2B_STATUS = 0xFU;   /* ack, same as the ISR would */
        }
    }
    RXPOLL_STAGE(0xE2);
    if (s_kicked == 0)
    {
        return;
    }

    env_isr(RL_GET_VQ_ID(s_link_id, 0)); /* remote TX vq: link-up + freed buffers */
    RXPOLL_STAGE(0xE3);
    env_isr(RL_GET_VQ_ID(s_link_id, 1)); /* remote RX vq: deliver incoming packets */
    RXPOLL_STAGE(0xE4);
}

/*
 * Block the drain thread until the mailbox ISR signals an A7 kick, or until `ms`
 * elapses (the timeout keeps the periodic service work — radio DIO1 poll, B2A
 * reply/event flush — running on its ~2ms cadence even when the link is idle).
 * Extra kick tokens from a burst are coalesced so one wake services the whole
 * batch (rx_poll drains all pending buffers) instead of spinning.
 */
void rpmsg_rv1106_rx_wait(uint32_t ms)
{
    if (s_rx_sem_ready == 0)
    {
        rt_thread_mdelay(ms);      /* pre-init fallback (should not happen) */
        return;
    }
    (void)rt_sem_take(&s_rx_sem, rt_tick_from_millisecond(ms));
    while (rt_sem_take(&s_rx_sem, 0) == RT_EOK) { }   /* coalesce burst */
}

/*
 * Mailbox BB interrupt: doorbell-ACK ONLY.
 *
 * The SCR1 cannot READ A2B_STATUS (returns 0), so HAL_MBOX_IrqHandler can never
 * identify the pending channel — and letting the ISR touch the virtqueues would
 * race the RX poll thread (that race corrupted the link after ~22 pingpongs).
 * Division of labor:
 *   ISR   = blind write-1-clear of ALL A2B channels (the write DOES land even
 *           though reads return 0) + latch s_kicked + RELEASE s_rx_sem to wake
 *           the drain thread immediately. Sub-microsecond ack, so the Linux
 *           mailbox TX path never sees the channel busy no matter how fast it
 *           kicks. It does NOT touch the virtqueues.
 *   THREAD = (ping_echo) drains both virtqueues; the only place that calls
 *           env_isr(). It now blocks on s_rx_sem and is woken by the ISR on each
 *           kick (RX latency ~microseconds), falling back to a 2ms timeout for
 *           the periodic service work. No concurrency on the vq structures.
 */
static void rpmsg_mbox_isr(int irqn, void *param)
{
    (void)irqn;
    (void)param;
    rl_pMBox->A2B_STATUS = 0xFU;   /* W1C all four channels */
    s_kicked = 1;
    if (s_rx_sem_ready)            /* wake the drain thread now, don't wait 2ms */
        rt_sem_release(&s_rx_sem);
}

/* RX callback: a mailbox message from the A7 carries link_id in CMD, magic in DATA */
static void rpmsg_remote_cb(struct MBOX_CMD_DAT *msg, void *args)
{
    uint32_t link_id;
    struct MBOX_CMD_DAT rx_msg = *msg;

    /* TRACE (no rt_kprintf: console is disabled and may hang) */
    *(volatile unsigned int *)0xff6ff824 = 0xCAFE0010;   /* remote_cb entered */
    *(volatile unsigned int *)0xff6ff828 = rx_msg.DATA;  /* expect magic 0x524D5347 */
    *(volatile unsigned int *)0xff6ff82c = rx_msg.CMD;   /* expect link_id 0x04 */

    link_id = rx_msg.CMD & 0xFFU;

    /*
     * RV1106 has a single mailbox doorbell per link, so a kick cannot tell us
     * which virtqueue the master poked. Service BOTH: queue 1 is the remote TX
     * vq whose callback sets link_state (link-up), queue 0 is the RX vq that
     * delivers incoming packets to the endpoint.
     */
    *(volatile unsigned int *)0xff6ff830 = 0xCAFE0011;   /* before env_isr(rx) */
    env_isr(RL_GET_VQ_ID(link_id, 0));
    *(volatile unsigned int *)0xff6ff834 = 0xCAFE0012;   /* after env_isr(rx) */
    env_isr(RL_GET_VQ_ID(link_id, 1));
    *(volatile unsigned int *)0xff6ff838 = 0xCAFE0013;   /* after env_isr(tx) */
}

/* Single RX client on RL_RV1106_RX_CHAN; all use the one BB mailbox IRQ. */
static struct MBOX_CLIENT mbox_clr[MBOX_CHAN_CNT] =
{
    { "rpmsg-r0", RL_PLATFORM_R_IRQ(0), rpmsg_remote_cb, (void *)MBOX_CH_0 },
    { "rpmsg-r1", RL_PLATFORM_R_IRQ(1), rpmsg_remote_cb, (void *)MBOX_CH_1 },
    { "rpmsg-r2", RL_PLATFORM_R_IRQ(2), rpmsg_remote_cb, (void *)MBOX_CH_2 },
    { "rpmsg-r3", RL_PLATFORM_R_IRQ(3), rpmsg_remote_cb, (void *)MBOX_CH_3 },
};

static void platform_global_isr_disable(void)
{
    s_isr_level = rt_hw_interrupt_disable();
}

static void platform_global_isr_enable(void)
{
    rt_hw_interrupt_enable(s_isr_level);
}

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data)
{
    /* This core is always the rpmsg REMOTE on RV1106. */
    env_register_isr(vector_id, isr_data);

    /* Remember the link; enable doorbell-less RX only once BOTH virtqueue
     * vectors are registered, so the poll can never notify a half-initialized
     * instance. */
    s_link_id = RL_GET_LINK_ID(vector_id);

    env_lock_mutex(platform_lock);

    RL_ASSERT(0 <= isr_counter);
    if (isr_counter < 2 * RL_MAX_INSTANCE_NUM)
    {
        rt_hw_interrupt_install(RL_PLATFORM_A2B_IRQ_BASE, rpmsg_mbox_isr, RT_NULL, "rpmsg-lite");

        if (register_count % 2 == 0)
        {
            /* Remote: inited B2A -> receives A2B (master->remote) and sends B2A.
             * Register the RX client on the A2B receive channel. */
            HAL_MBOX_Init(rl_pMBox, RL_MBOX_B2A);
            HAL_MBOX_RegisterClient(rl_pMBox, RL_RV1106_RX_CHAN, &mbox_clr[RL_RV1106_RX_CHAN]);
        }
        register_count++;
    }
    isr_counter++;
    if (isr_counter >= 2)
    {
        s_rx_ready = 1;
    }

    env_unlock_mutex(platform_lock);
    return 0;
}

int32_t platform_deinit_interrupt(uint32_t vector_id)
{
    env_lock_mutex(platform_lock);
    RL_ASSERT(0 < isr_counter);
    isr_counter--;
    env_unregister_isr(vector_id);
    env_unlock_mutex(platform_lock);
    return 0;
}

void platform_notify(uint32_t vector_id)
{
    uint32_t link_id;
    struct MBOX_CMD_DAT tx_msg;

    link_id = RL_GET_LINK_ID(vector_id);
    tx_msg.CMD = link_id & 0xFFU;
    tx_msg.DATA = RL_RPMSG_MAGIC;

    env_lock_mutex(platform_lock);
    HAL_MBOX_SendMsg(rl_pMBox, RL_RV1106_TX_CHAN, &tx_msg);
    env_unlock_mutex(platform_lock);
}

void platform_time_delay(uint32_t num_msec)
{
    rt_thread_mdelay(num_msec);
}

int32_t platform_in_isr(void)
{
    return (rt_interrupt_get_nest() != 0) ? 1 : 0;
}

int32_t platform_interrupt_enable(uint32_t vector_id)
{
    RL_ASSERT(0 < disable_counter);
    platform_global_isr_disable();
    disable_counter--;
    if (disable_counter < 2 * RL_MAX_INSTANCE_NUM)
        rt_hw_interrupt_umask(RL_PLATFORM_A2B_IRQ_BASE);
    platform_global_isr_enable();
    return ((int32_t)vector_id);
}

int32_t platform_interrupt_disable(uint32_t vector_id)
{
    RL_ASSERT(0 <= disable_counter);
    platform_global_isr_disable();
    if (disable_counter < 2 * RL_MAX_INSTANCE_NUM)
        rt_hw_interrupt_mask(RL_PLATFORM_A2B_IRQ_BASE);
    disable_counter++;
    platform_global_isr_enable();
    return ((int32_t)vector_id);
}

void platform_map_mem_region(uint32_t vrt_addr, uint32_t phy_addr, uint32_t size, uint32_t flags) {}
void platform_cache_all_flush_invalidate(void) {}
void platform_cache_disable(void) {}
uint32_t platform_vatopa(void *addr) { return ((uint32_t)(char *)addr); }
void *platform_patova(uint32_t addr) { return ((void *)(char *)addr); }

int32_t platform_init(void)
{
#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
    if (0 != env_create_mutex(&platform_lock, 1, &platform_lock_static_ctxt))
#else
    if (0 != env_create_mutex(&platform_lock, 1))
#endif
    {
        return -1;
    }
    /* RX-wake semaphore: mailbox ISR releases it, the ping_echo drain thread
     * waits on it (rpmsg_rv1106_rx_wait). Init before any kick can arrive. */
    rt_sem_init(&s_rx_sem, "rpmsgrx", 0, RT_IPC_FLAG_FIFO);
    s_rx_sem_ready = 1;
    return 0;
}

int32_t platform_deinit(void)
{
    env_delete_mutex(platform_lock);
    platform_lock = ((void *)0);
    return 0;
}
