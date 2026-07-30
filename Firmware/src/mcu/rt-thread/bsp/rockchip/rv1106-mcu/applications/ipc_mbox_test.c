/*
 * CubeSat — RAW MAILBOX IPC loopback diagnostic (RV1106 RISC-V / SCR1 HPMCU).
 *
 * Mirrors the proven Luckfox AMP reference pattern (see riscv2arm.md): raw
 * mailbox messaging + a shared scratch region, with NO rpmsg-lite and NO
 * vrings. Two goals:
 *
 *   1. Prove the MCU->ARM (B2A) doorbell path — the direction Rockchip actually
 *      exercises (stream.c HAL_MBOX_Init(pmbox, false) + B2A_INTEN). The MCU
 *      writes an advancing counter to a spare B2A channel; Linux reads it with
 *      `devmem`.
 *
 *   2. Discover which mailbox RX registers the SCR1 can actually READ. We know
 *      A2B_STATUS reads back 0 on this core even when the A7 sees the pending
 *      bit (that is why HAL_MBOX_IrqHandler never finds the channel). This test
 *      publishes the MCU's view of every candidate RX register to the 8 KB
 *      hpmcu_sram scratch so we can compare it, over serial, with the A7 view.
 *
 * SAFETY: this touches ONLY the mailbox registers (0xff5c0000 block) and the
 * hpmcu_sram scratch (0xff6ff900+). It NEVER touches the DDR vrings
 * (0x0ff00000) — that is what corrupted Linux's virtio probe before. So this
 * firmware is safe to run from boot regardless of Linux state.
 *
 * Observe from Linux (all reads via busybox devmem):
 *   MCU heartbeat (B2A ch2 DATA) : devmem 0xff5c0044 32   -> advancing 0xC0DExxxx
 *   MCU scratch  (what MCU reads):
 *     0xff6ff900  thread heartbeat        (advancing 0xB000xxxx)
 *     0xff6ff904  A2B_STATUS  as MCU sees (A7 sees 0x08 when a kick is pending)
 *     0xff6ff908  A2B_CMD(3)  as MCU sees (A7 wrote 0x00000004 = link_id)
 *     0xff6ff90c  A2B_DAT(3)  as MCU sees (A7 wrote 0x524D5347 = "RMSG")
 *     0xff6ff910  int_mux status0 as MCU sees (0xff5d0080)
 *     0xff6ff914  A2B_INTEN   as MCU sees (sanity: should read 0x08)
 *     0xff6ff918  kick-detected latch     (0x600Dxxxx once DAT(3) == magic)
 *
 * Send a controlled A2B kick to the MCU from Linux (mimics the kernel):
 *   devmem 0xff5c0024 32 0x524D5347   # A2B_DAT(3) = magic
 *   devmem 0xff5c0020 32 0x00000004   # A2B_CMD(3) = link_id  (raises doorbell)
 * Then re-read 0xff6ff908 / 0xff6ff90c / 0xff6ff918 to see if the MCU read it.
 */
#include <rthw.h>
#include <rtthread.h>

#include "ipc_test_cfg.h"

#if defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1)

#define REG(a)          (*(volatile unsigned int *)(unsigned long)(a))

/* RV1106 mailbox @0xff5c0000 (offsets per hal_mbox / rockchip-mailbox.c). */
#define MBOX_BASE       0xff5c0000UL
#define A2B_INTEN       (MBOX_BASE + 0x00)
#define A2B_STATUS      (MBOX_BASE + 0x04)
#define A2B_CMD(x)      (MBOX_BASE + 0x08 + (x) * 8)
#define A2B_DAT(x)      (MBOX_BASE + 0x0c + (x) * 8)
#define B2A_CMD(x)      (MBOX_BASE + 0x30 + (x) * 8)
#define B2A_DAT(x)      (MBOX_BASE + 0x34 + (x) * 8)

/* SCR1 interrupt multiplexer — group-0 status; mailbox BB line is bit 2. */
#define INTMUX_STATUS0  0xff5d0080UL

#define RX_CHAN         3U      /* A2B channel Linux transmits rpmsg on */
#define HB_CHAN         2U      /* spare B2A channel for our heartbeat (kernel
                                 * listens on B2A ch0 only, so ch2 is inert) */
#define KICK_MAGIC      0x524D5347U  /* "RMSG" — the DATA Linux sends on A2B */

/* hpmcu_sram scratch window (8 KB, shared, MCU-readable and A7-readable). */
#define SCRATCH(off)    REG(0xff6ff900UL + (off))

static void ipc_test_thread(void *arg)
{
    unsigned int n = 0;
    unsigned int kick_latch = 0;

    (void)arg;

    /* NOTE: we deliberately DO NOT enable A2B_INTEN here. Enabling the mailbox
     * receive IRQ without a working SCR1 handler made the core trap and hang
     * ~400 ms after boot, exactly when Linux sends its first kick. We poll
     * everything from a thread, so no mailbox IRQ is needed. */

    (void)kick_latch;
    while (1)
    {
        /* CubeSat ISOLATION: absolute minimum — heartbeat to hpmcu_sram + mdelay.
         * NO mailbox reads, NO DDR reads, NO int_mux, NO peripheral access. If the
         * MCU still crashes here, the fault is intrinsic (scheduler/timer/stack),
         * not from anything the thread touches. */
        SCRATCH(0x00) = 0xB0000000U | (n & 0xffffU);   /* thread heartbeat */
        n++;
        rt_thread_mdelay(200);
    }
}

/*
 * Idle-thread hook: runs whenever no other thread is ready. It advances a
 * counter (0x34) independently of the ipctest thread. Diagnostic for the
 * ~400 ms freeze:
 *   heartbeat(0x00) frozen + idle(0x34) ADVANCING -> RTOS tick/CPU alive, the
 *       ipctest thread is merely blocked (e.g. tick stopped so mdelay never
 *       returns and idle runs forever) -> points at the TIMER clock.
 *   heartbeat(0x00) frozen + idle(0x34) FROZEN     -> the CPU core itself
 *       stopped (core clock gated / MCU held in reset by Linux).
 */
static void ipc_test_idle_hook(void)
{
    static volatile unsigned int c = 0;
    SCRATCH(0x34) = 0x1D1E0000U | ((c++) & 0xffffU);
}

int ipc_mbox_test_init(void)
{
    rt_thread_t tid;

    /* Boot marker so we can confirm the test app (not ping_echo) is running. */
    SCRATCH(0x1c) = 0x1EE70001U;

    /* Idle counter to distinguish CPU-halt from tick-stop at the ~400 ms freeze. */
    rt_thread_idle_sethook(ipc_test_idle_hook);

    tid = rt_thread_create("ipctest", ipc_test_thread, RT_NULL, 2048, 15, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
    return 0;
}
INIT_APP_EXPORT(ipc_mbox_test_init);

#endif /* IPC_RAW_MBOX_TEST */
