/*
 * CubeSat — DWC3 USB2 device-mode bring-up probe (RISC-V MCU owns the USB OTG)
 *
 * PURPOSE
 *   The RV1106 carries a Synopsys DWC3 USB controller (base 0xffb00000) with a
 *   USB2 "innosilicon" PHY (analog block at 0xff3e0000 + control in the GRF at
 *   0xff000000). In the factory firmware Linux owns the USB (dwc3 in peripheral
 *   mode). This file demonstrates, step by step and measurably, that the RISC-V
 *   MCU can talk to that same controller and bring it up FROM COLD.
 *
 * HOW IT IS PROGRAMMED
 *   It does NOT run at boot (that would be dangerous: an unbounded poll could
 *   hang the core). A low-priority thread watches a "mailbox" register in
 *   hpmcu_sram that the A7 writes via `devmem`. Depending on the value it runs
 *   one of the experiments and leaves the result in other registers the A7 also
 *   reads via `devmem`.
 *
 *     devmem 0xff6ff980 32 1   -> M0: only READ the core ID (GSNPSID). Safe: a
 *                                 single read. Works with Linux OWNING the USB.
 *     devmem 0xff6ff980 32 2   -> M1: COLD BRING-UP up to "core alive in device
 *                                 mode + VBUS forced".
 *     devmem 0xff6ff980 32 3   -> M2: bring-up + DCFG + event buffer + EP0 +
 *                                 RUN_STOP, then drain the event ring and record
 *                                 the bus events (USB reset / connect-done /
 *                                 EP0 SETUP). Needs the controller free of Linux:
 *                                   echo ffb00000.usb > \
 *                                     /sys/bus/platform/drivers/dwc3/unbind
 *                                 and a USB cable to a host PC.
 *
 * WHY IT WORKS
 *   The SCR1 has flat physical access and the DCACHE is off, so 0xffb00000
 *   (DWC3), 0xff3e0000 (PHY) and 0xff000000 (GRF) are reachable like any other
 *   MMIO, and DMA buffers in MCU DDR are coherent with no cache maintenance.
 *   Clocks/resets are requested with the same HAL_CRU API the I2C/SPI drivers
 *   use. The DWC3 sequence, the PHY map and the event format were transcribed
 *   from the kernel driver (drivers/usb/dwc3/core.c, gadget.c, ep0.c and
 *   phy-rockchip-inno-usb2.c).
 *
 * SCOPE
 *   M2 here is "Layer A": it runs the controller and OBSERVES the bus (it does
 *   not yet answer descriptors, so the host will not finish enumeration). Seeing
 *   USB-reset + connect-done + a SETUP packet proves the electrical link and the
 *   event ring work; answering descriptors (full CDC -> /dev/ttyACMx) is the
 *   next layer, built on top of this.
 */

#include <rtthread.h>
#include <rthw.h>
#include <stdint.h>
#include "hal_base.h"

/* ------------------------------------------------------------------ *
 *  "Mailbox" registers in hpmcu_sram (A7 reads/writes them via devmem) *
 * ------------------------------------------------------------------ */
#define USB_REG_CMD      0xff6ff980U   /* A7->MCU: 1=M0, 2=M1, 3=M2; MCU sets 0xD0FE when done */
#define USB_REG_STAGE    0xff6ff984U   /* MCU->A7: stage code (see USB_ST_*)                   */
#define USB_REG_ID       0xff6ff988U   /* MCU->A7: raw GSNPSID (expected 0x5533xxxx)           */
#define USB_REG_VERDICT  0xff6ff98cU   /* MCU->A7: 0x5533_xxxx on match / error code           */
#define USB_REG_BVALID   0xff6ff990U   /* MCU->A7: utmi_bvalid readback (GRF 0x60 bit9)        */
#define USB_REG_DSTS     0xff6ff994U   /* MCU->A7: DSTS readback after bring-up                */
/* --- M2 event-loop diagnostics --- */
#define USB_REG_M2STAGE  0xff6ff998U   /* MCU->A7: M2 sub-stage                          */
#define USB_REG_EVCOUNT  0xff6ff99cU   /* MCU->A7: total events drained                  */
#define USB_REG_RSTCNT   0xff6ff9a0U   /* MCU->A7: USB-reset events seen                 */
#define USB_REG_CONNCNT  0xff6ff9a4U   /* MCU->A7: connect-done events (speed in [19:16])*/
#define USB_REG_SETUPCNT 0xff6ff9a8U   /* MCU->A7: EP0 SETUP packets seen                */
#define USB_REG_SETUP0   0xff6ff9acU   /* MCU->A7: SETUP bytes [3:0] (bmReqType,bReq,wVal)*/
#define USB_REG_SETUP1   0xff6ff9b0U   /* MCU->A7: SETUP bytes [7:4] (wIndex,wLength)    */
#define USB_REG_LASTEVT  0xff6ff9b4U   /* MCU->A7: last raw event dword                  */
#define USB_REG_RUNDSTS  0xff6ff9b8U   /* MCU->A7: DSTS after RUN_STOP                   */
#define USB_REG_PRIMERC  0xff6ff9bcU   /* MCU->A7: [0]=prime ok, [15:8]=STARTXFER status, [23:16]=rsc idx */

/* Stage codes: if the core hangs, the LAST value tells where it died. */
#define USB_ST_M0_ENTER   0x00000010U
#define USB_ST_M0_READ    0x00000012U
#define USB_ST_M0_DONE    0x0000001FU
#define USB_ST_M1_ENTER   0x00000020U
#define USB_ST_M1_CLOCKS  0x00000021U
#define USB_ST_M1_RESETS  0x00000022U
#define USB_ST_M1_PHYTUNE 0x00000023U
#define USB_ST_M1_PHYON   0x00000024U
#define USB_ST_M1_BVALID  0x00000025U
#define USB_ST_M1_SRST    0x00000026U
#define USB_ST_M1_SRSTOK  0x00000027U
#define USB_ST_M1_GCTL    0x00000028U
#define USB_ST_M1_DEVMODE 0x00000029U
#define USB_ST_M1_READID  0x0000002AU
#define USB_ST_M1_DONE    0x0000002FU
#define USB_ST_ERR_SRST   0x000000E1U  /* timeout waiting for CSFTRST -> 0 */

/* M2 sub-stages */
#define USB_M2_ENTER      0x00000030U
#define USB_M2_COREOK     0x00000031U
#define USB_M2_DCFG       0x00000032U
#define USB_M2_EVBUF      0x00000033U
#define USB_M2_EP0CFG     0x00000034U
#define USB_M2_EP0PRIME   0x00000035U
#define USB_M2_DEVTEN     0x00000036U
#define USB_M2_RUNNING    0x00000037U
#define USB_M2_WATCHING   0x00000038U
#define USB_M2_ERR_CORE   0x000000E2U  /* core bring-up failed */
#define USB_M2_ERR_DEPCMD 0x000000E3U  /* a DEPCMD did not complete */
#define USB_M2_ERR_RUN    0x000000E4U  /* controller did not leave halt */

#define USB_CMD_DONE      0x0000D0FEU  /* "DONE" written back into the command mailbox */

/* ------------------------------------------------------------------ *
 *  Hardware addresses                                                 *
 * ------------------------------------------------------------------ */
#define DWC3_BASE        0xffb00000U
#define GRF_BASE         0xff000000U   /* rockchip,rv1106-grf (the PHY's usbgrf) */
#define USBPHY_BASE      0xff3e0000U   /* USB2 PHY analog block                  */

/* --- DWC3: offsets from core.h (physical address = DWC3_BASE + offset) --- */
#define DWC3_GSNPSID     (DWC3_BASE + 0xc120U)  /* core ID: (x>>16)==0x5533 */
#define DWC3_GCTL        (DWC3_BASE + 0xc110U)
#define DWC3_GUSB2PHYCFG0 (DWC3_BASE + 0xc200U)
#define DWC3_GEVNTADRLO0 (DWC3_BASE + 0xc400U)
#define DWC3_GEVNTADRHI0 (DWC3_BASE + 0xc404U)
#define DWC3_GEVNTSIZ0   (DWC3_BASE + 0xc408U)
#define DWC3_GEVNTCOUNT0 (DWC3_BASE + 0xc40cU)
#define DWC3_DCFG        (DWC3_BASE + 0xc700U)
#define DWC3_DCTL        (DWC3_BASE + 0xc704U)
#define DWC3_DEVTEN      (DWC3_BASE + 0xc708U)
#define DWC3_DSTS        (DWC3_BASE + 0xc70cU)
#define DWC3_DALEPENA    (DWC3_BASE + 0xc720U)
#define DWC3_DEP_BASE(n) (DWC3_BASE + 0xc800U + ((n) * 0x10U)) /* PAR2@0,PAR1@4,PAR0@8,CMD@c */

/* GCTL */
#define GCTL_SCALEDOWN_MASK   (3U << 4)
#define GCTL_DISSCRAMBLE      (1U << 3)
#define GCTL_DSBLCLKGTNG      (1U << 0)
#define GCTL_PRTCAP_MASK      (3U << 12)
#define GCTL_PRTCAP_DEVICE    (2U << 12)   /* PRTCAPDIR = device */
/* DCTL */
#define DCTL_CSFTRST          (1U << 30)
#define DCTL_RUN_STOP         (1U << 31)
#define DCTL_ULSTCHNGREQ_MASK (0x0fU << 5)
/* DCFG */
#define DCFG_SPEED_MASK       (7U << 0)
#define DCFG_HIGHSPEED        (0U << 0)
#define DCFG_NUMP_MASK        (0x1fU << 17)
#define DCFG_NUMP(n)          (((n) & 0x1fU) << 17)
#define DCFG_IGNSTRMPP        (1U << 23)
/* DSTS */
#define DSTS_DEVCTRLHLT       (1U << 22)
/* GUSB2PHYCFG(0) */
#define G2PCFG_U2FREECLK      (1U << 30)
#define G2PCFG_USBTRDTIM_MASK (0x0fU << 10)
#define G2PCFG_USBTRDTIM_8BIT (9U << 10)
#define G2PCFG_ENBLSLPM       (1U << 8)
#define G2PCFG_SUSPHY         (1U << 6)
#define G2PCFG_PHYIF          (1U << 3)
/* GEVNTSIZ / GEVNTCOUNT */
#define GEVNTSIZ_INTMASK      (1U << 31)
#define GEVNTCOUNT_MASK       0xfffcU
/* DEPCMD */
#define DEPCMD_CMDACT         (1U << 10)
#define DEPCMD_SETEPCONFIG    0x01U
#define DEPCMD_SETTRANSFRES   0x02U
#define DEPCMD_STARTTRANSFER  0x06U
#define DEPCMD_DEPSTARTCFG    0x09U
#define DEPCMD_PARAM(x)       ((x) << 16)
/* DEPCFG param fields (gadget.h) */
#define DEPCFG_EP_TYPE(n)     (((n) & 3U) << 1)     /* control=0 */
#define DEPCFG_MPS(n)         (((n) & 0x7ffU) << 3)
#define DEPCFG_XFER_CMPL_EN   (1U << 8)
#define DEPCFG_XFER_NRDY_EN   (1U << 10)
#define DEPCFG_EP_NUMBER(n)   (((n) & 0x1fU) << 25)
#define DEPXFERCFG_NUM_RES(n) ((n) & 0xffffU)
/* TRB ctrl (core.h) */
#define TRB_CTRL_HWO          (1U << 0)
#define TRB_CTRL_LST          (1U << 1)
#define TRB_CTRL_TRBCTL(n)    (((n) & 0x3fU) << 4)
#define TRB_CTRL_ISP_IMI      (1U << 10)
#define TRB_CTRL_IOC          (1U << 11)
#define TRBCTL_CONTROL_SETUP  2U
/* Event decode */
#define EVT_IS_DEVSPEC(e)     ((e) & 1U)
#define EVT_DEV_TYPE(e)       (((e) >> 8) & 0xfU)   /* 1=USBRst 2=ConnectDone */
#define EVT_DEV_INFO(e)       (((e) >> 16) & 0x1ffU)
#define EVT_EP_NUM(e)         (((e) >> 1) & 0x1fU)
#define EVT_EP_EVENT(e)       (((e) >> 6) & 0xfU)   /* 1=XferComplete */
#define DEVEVT_RESET          1U
#define DEVEVT_CONNECTDONE    2U
#define DEPEVT_XFERCOMPLETE   1U

/* --- CRU gate/reset IDs already live in rv1106.h (encoded scheme decoded by
 *   HAL_CRU_ClkEnable/ClkResetDeassert). We do not redefine them here. */

/* --- GRF: USB2 PHY control (Rockchip hiword-mask writes; literals from the
 *   kernel rv1106_usb2phy_cfg struct). --- */
#define GRF_USBPHY_SUS    (GRF_BASE + 0x0050U)
#define GRF_USBPHY_CLKOUT (GRF_BASE + 0x0058U)
#define GRF_USBPHY_STATUS (GRF_BASE + 0x0060U)   /* utmi_bvalid = bit9 (read-only) */
#define GRF_CLKOUT_ENABLE   0x00100000U
#define GRF_SUS_NORMAL      0x01FF0000U
#define GRF_BVALID_FORCE    0xC000C000U
#define GRF_STATUS_BVALID   (1U << 9)

/* ------------------------------------------------------------------ *
 *  DMA memory in MCU DDR (DCACHE off -> coherent with the DWC3 master) *
 * ------------------------------------------------------------------ */
#define EVBUF_SIZE 4096
static uint8_t  s_evbuf[EVBUF_SIZE] __attribute__((aligned(4096)));
static uint32_t s_ep0_trb[4]        __attribute__((aligned(16)));  /* one TRB = 4 dwords */
static uint8_t  s_setup_buf[64]     __attribute__((aligned(64)));  /* EP0 SETUP data     */
static uint32_t s_evbuf_pos;         /* read cursor into the event ring */

/* ------------------------------------------------------------------ *
 *  Raw MMIO access (DCACHE off -> no cache barriers needed)           *
 * ------------------------------------------------------------------ */
static inline void mw(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t mr(uint32_t a)         { return *(volatile uint32_t *)a; }
static inline void stage(uint32_t s)          { mw(USB_REG_STAGE, s); }

/* Bounded wait (never an infinite busy-wait): returns 1 if (mr(a)&mask)==want. */
static int wait_bits(uint32_t a, uint32_t mask, uint32_t want, uint32_t ms)
{
    rt_tick_t deadline = rt_tick_get() + rt_tick_from_millisecond(ms);
    while (rt_tick_get() < deadline)
    {
        if ((mr(a) & mask) == want)
            return 1;
        rt_thread_mdelay(1);   /* yield the CPU: 1 tick. We do not burn the core. */
    }
    return ((mr(a) & mask) == want);
}

/* ================================================================== *
 *  USB2 innosilicon PHY — analog trims + leave suspend                *
 * ================================================================== */
static void usb_phy_init(void)
{
    uint32_t v;

    /* Analog trims (direct RMW on the block at 0xff3e0000), transcribed from
     * rv1106_usb2phy_tuning(): LDO/eye/ODT. */
    v = mr(USBPHY_BASE + 0x0030); v = (v & ~0x7U)        | 0x7U;        mw(USBPHY_BASE + 0x0030, v);
    /* HS Tx pre-emphasis strength is silicon-revision dependent: rev>=1 wants
     * 3'b001, rev 0 wants 3'b011 (rv1106_usb2phy_tuning + rockchip_get_cpu_version).
     * Hardcoding 3'b011 on a rev-1 part over-drives the HS eye and the device is
     * detected at FS but the HS chirp never completes (port stuck, PED=0). Read
     * the CPU version (OS_REG1[2:0] @ 0xff020204) and pick the matching strength. */
    { uint32_t pe = (mr(0xff020204U) & 0x7U) ? (0x1U << 3) : (0x3U << 3);
      v = mr(USBPHY_BASE + 0x0040); v = (v & ~(0x7U << 3)) | pe; mw(USBPHY_BASE + 0x0040, v); }
    v = mr(USBPHY_BASE + 0x0064); v = (v & ~(0xFU << 3)) | (0x0U << 3); mw(USBPHY_BASE + 0x0064, v);
    v = mr(USBPHY_BASE + 0x0100); v =  v & ~(1U << 6);                  mw(USBPHY_BASE + 0x0100, v);
    v = mr(USBPHY_BASE + 0x011c); v = (v & ~0x1FU)       | 0x17U;       mw(USBPHY_BASE + 0x011c, v);
    v = mr(USBPHY_BASE + 0x0124); v = (v & ~(0x7U << 2)) | (0x3U << 2); mw(USBPHY_BASE + 0x0124, v);
    v = mr(USBPHY_BASE + 0x01a4); v = (v & ~(0xFU << 4)) | (0x1U << 4); mw(USBPHY_BASE + 0x01a4, v);
    v = mr(USBPHY_BASE + 0x01b4); v = (v & ~(0xFU << 4)) | (0x1U << 4); mw(USBPHY_BASE + 0x01b4, v);
    v = mr(USBPHY_BASE + 0x0070); v =  v | (1U << 2);                   mw(USBPHY_BASE + 0x0070, v);
    stage(USB_ST_M1_PHYTUNE);

    /* Enable the 480 MHz clkout, then take the OTG port out of suspend. */
    mw(GRF_USBPHY_CLKOUT, GRF_CLKOUT_ENABLE);
    rt_thread_mdelay(2);
    mw(GRF_USBPHY_SUS, GRF_SUS_NORMAL);
    rt_thread_mdelay(2);
    stage(USB_ST_M1_PHYON);
}

/* ================================================================== *
 *  Shared: bring the core up to "alive in device mode + VBUS forced"  *
 *  Returns 1 on success, 0 if the core soft-reset timed out.          *
 * ================================================================== */
static int usb_core_bringup_device(void)
{
    uint32_t reg;

    /* 1) Clocks: OTG bus/ref/utmi + PHY pclk/ref. */
    HAL_CRU_ClkEnable(ACLK_USBOTG_GATE);
    HAL_CRU_ClkEnable(CLK_REF_USBOTG_GATE);
    HAL_CRU_ClkEnable(CLK_UTMI_USBOTG_GATE);
    HAL_CRU_ClkEnable(PCLK_USBPHY_GATE);
    HAL_CRU_ClkEnable(CLK_REF_USBPHY_GATE);
    stage(USB_ST_M1_CLOCKS);

    /* 2) Deassert resets: OTG AXI + PHY APB/POR/OTG. */
    HAL_CRU_ClkResetDeassert(SRST_ARESETN_USBOTG);
    HAL_CRU_ClkResetDeassert(SRST_PRESETN_USBPHY);
    HAL_CRU_ClkResetDeassert(SRST_RESETN_USBPHY_POR);
    HAL_CRU_ClkResetDeassert(SRST_RESETN_USBPHY_OTG);
    rt_thread_mdelay(2);
    stage(USB_ST_M1_RESETS);

    /* 3) PHY. */
    usb_phy_init();

    /* 4) Force VBUS/BVALID present so the DWC3 believes it is connected. */
    mw(GRF_USBPHY_CLKOUT, GRF_BVALID_FORCE);
    rt_thread_mdelay(1);
    mw(USB_REG_BVALID, mr(GRF_USBPHY_STATUS) & GRF_STATUS_BVALID);
    stage(USB_ST_M1_BVALID);

    /* 5) Core soft reset (device side): set CSFTRST, clear RUN_STOP. */
    stage(USB_ST_M1_SRST);
    reg = mr(DWC3_DCTL);
    reg |= DCTL_CSFTRST;
    reg &= ~DCTL_RUN_STOP;
    reg &= ~DCTL_ULSTCHNGREQ_MASK;
    mw(DWC3_DCTL, reg);
    if (!wait_bits(DWC3_DCTL, DCTL_CSFTRST, 0, 100))
    {
        stage(USB_ST_ERR_SRST);
        return 0;
    }
    stage(USB_ST_M1_SRSTOK);

    /* 6) DWC3 PHY config: UTMI 8-bit + RV1106 quirks. */
    reg = mr(DWC3_GUSB2PHYCFG0);
    reg &= ~G2PCFG_PHYIF;
    reg &= ~G2PCFG_USBTRDTIM_MASK;
    reg |=  G2PCFG_USBTRDTIM_8BIT;
    reg &= ~G2PCFG_SUSPHY;
    reg &= ~G2PCFG_ENBLSLPM;
    reg &= ~G2PCFG_U2FREECLK;
    mw(DWC3_GUSB2PHYCFG0, reg);

    /* 7) Global control: clear scaledown/clkgating/disscramble. */
    reg = mr(DWC3_GCTL);
    reg &= ~GCTL_SCALEDOWN_MASK;
    reg &= ~GCTL_DSBLCLKGTNG;
    reg &= ~GCTL_DISSCRAMBLE;
    mw(DWC3_GCTL, reg);
    stage(USB_ST_M1_GCTL);

    /* 8) Device mode: PRTCAPDIR = device. */
    reg = mr(DWC3_GCTL);
    reg &= ~GCTL_PRTCAP_MASK;
    reg |=  GCTL_PRTCAP_DEVICE;
    mw(DWC3_GCTL, reg);
    stage(USB_ST_M1_DEVMODE);
    return 1;
}

/* ================================================================== *
 *  M0 — read the core ID (a single read, zero risk)                   *
 * ================================================================== */
static void usb_m0_read_id(void)
{
    uint32_t id;
    stage(USB_ST_M0_ENTER);
    stage(USB_ST_M0_READ);
    id = mr(DWC3_GSNPSID);
    mw(USB_REG_ID, id);
    mw(USB_REG_VERDICT, ((id >> 16) == 0x5533U) ? id : 0xBAD00000U);
    stage(USB_ST_M0_DONE);
}

/* ================================================================== *
 *  M1 — cold bring-up up to "core alive in device mode"               *
 * ================================================================== */
static void usb_m1_cold_bringup(void)
{
    uint32_t id;
    stage(USB_ST_M1_ENTER);
    if (!usb_core_bringup_device())
    {
        mw(USB_REG_VERDICT, 0xE1E1E1E1U);
        return;
    }
    stage(USB_ST_M1_READID);
    id = mr(DWC3_GSNPSID);
    mw(USB_REG_ID, id);
    mw(USB_REG_VERDICT, ((id >> 16) == 0x5533U) ? id : 0xBAD00000U);
    mw(USB_REG_DSTS, mr(DWC3_DSTS));
    stage(USB_ST_M1_DONE);
}

/* ================================================================== *
 *  DEPCMD — issue an endpoint command and wait for CMDACT to clear     *
 *  ep = physical endpoint (0=EP0-out, 1=EP0-in). Returns 1 on success. *
 * ================================================================== */
static int depcmd(uint32_t ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
    uint32_t base = DWC3_DEP_BASE(ep);
    mw(base + 0x08, p0);   /* PAR0 */
    mw(base + 0x04, p1);   /* PAR1 */
    mw(base + 0x00, p2);   /* PAR2 */
    mw(base + 0x0c, cmd | DEPCMD_CMDACT);
    return wait_bits(base + 0x0c, DEPCMD_CMDACT, 0, 50);
}

/* Configure EP0-out (dep 0) and EP0-in (dep 1) as 64-byte control endpoints. */
static int usb_ep0_config(void)
{
    /* DEPSTARTCFG on dep0 (config number 0). */
    if (!depcmd(0, DEPCMD_DEPSTARTCFG | DEPCMD_PARAM(0), 0, 0, 0)) return 0;
    /* SETTRANSFRESOURCE = 1 for both control endpoints. */
    if (!depcmd(0, DEPCMD_SETTRANSFRES, DEPXFERCFG_NUM_RES(1), 0, 0)) return 0;
    if (!depcmd(1, DEPCMD_SETTRANSFRES, DEPXFERCFG_NUM_RES(1), 0, 0)) return 0;
    /* SETEPCONFIG (control, MPS 64) for dep0 and dep1. */
    if (!depcmd(0, DEPCMD_SETEPCONFIG,
                DEPCFG_EP_TYPE(0) | DEPCFG_MPS(64),
                DEPCFG_XFER_CMPL_EN | DEPCFG_XFER_NRDY_EN | DEPCFG_EP_NUMBER(0), 0)) return 0;
    if (!depcmd(1, DEPCMD_SETEPCONFIG,
                DEPCFG_EP_TYPE(0) | DEPCFG_MPS(64),
                DEPCFG_XFER_CMPL_EN | DEPCFG_XFER_NRDY_EN | DEPCFG_EP_NUMBER(1), 0)) return 0;
    /* Enable EP0-out and EP0-in. */
    mw(DWC3_DALEPENA, 0x3);
    return 1;
}

/* Prime EP0-out with a Control-Setup TRB expecting an 8-byte SETUP packet. */
static int usb_ep0_prime_setup(void)
{
    uint32_t trb = (uint32_t)(uintptr_t)s_ep0_trb;
    uint32_t buf = (uint32_t)(uintptr_t)s_setup_buf;
    uint32_t cmdreg, rc;

    s_ep0_trb[0] = buf;                       /* BPL: buffer pointer low  */
    s_ep0_trb[1] = 0;                         /* BPH: buffer pointer high */
    s_ep0_trb[2] = 8;                         /* size: 8 bytes            */
    s_ep0_trb[3] = TRB_CTRL_HWO | TRB_CTRL_LST | TRB_CTRL_TRBCTL(TRBCTL_CONTROL_SETUP)
                 | TRB_CTRL_ISP_IMI | TRB_CTRL_IOC;
    /* STARTTRANSFER on dep0: PAR0=TRB addr high, PAR1=TRB addr low. */
    rc = depcmd(0, DEPCMD_STARTTRANSFER, 0, trb, 0);
    /* Record diagnostics: completion flag, command status [15:12] and the
     * transfer-resource index [22:16] the controller assigned. */
    cmdreg = mr(DWC3_DEP_BASE(0) + 0x0c);
    mw(USB_REG_PRIMERC, (rc & 1U)
                        | (((cmdreg >> 12) & 0xfU) << 8)
                        | (((cmdreg >> 16) & 0x7fU) << 16));
    return rc;
}

/* ================================================================== *
 *  M2 — run the controller and watch the bus (Layer A: observe only)  *
 * ================================================================== */
static void usb_m2_run_and_watch(void)
{
    uint32_t reg;

    mw(USB_REG_M2STAGE, USB_M2_ENTER);
    mw(USB_REG_EVCOUNT, 0);  mw(USB_REG_RSTCNT, 0);
    mw(USB_REG_CONNCNT, 0);  mw(USB_REG_SETUPCNT, 0);
    s_evbuf_pos = 0;

    if (!usb_core_bringup_device())
    {
        mw(USB_REG_M2STAGE, USB_M2_ERR_CORE);
        return;
    }
    mw(USB_REG_M2STAGE, USB_M2_COREOK);

    /* DCFG: high-speed, NUMP 16, ignore stream-pp. */
    reg = mr(DWC3_DCFG);
    reg &= ~DCFG_SPEED_MASK;
    reg |=  DCFG_HIGHSPEED;
    reg &= ~DCFG_NUMP_MASK;
    reg |=  DCFG_NUMP(16);
    reg |=  DCFG_IGNSTRMPP;
    mw(DWC3_DCFG, reg);
    mw(USB_REG_M2STAGE, USB_M2_DCFG);

    /* Event buffer: one 4KiB ring in MCU DDR. */
    for (uint32_t i = 0; i < EVBUF_SIZE / 4; i++) ((volatile uint32_t *)s_evbuf)[i] = 0;
    mw(DWC3_GEVNTADRLO0, (uint32_t)(uintptr_t)s_evbuf);
    mw(DWC3_GEVNTADRHI0, 0);
    mw(DWC3_GEVNTSIZ0, EVBUF_SIZE);          /* SIZE, INTMASK=0 -> unmasked  */
    mw(DWC3_GEVNTCOUNT0, mr(DWC3_GEVNTCOUNT0) & GEVNTCOUNT_MASK); /* clear stale count */
    mw(USB_REG_M2STAGE, USB_M2_EVBUF);

    /* EP0 configuration. */
    if (!usb_ep0_config())
    {
        mw(USB_REG_M2STAGE, USB_M2_ERR_DEPCMD);
        return;
    }
    mw(USB_REG_M2STAGE, USB_M2_EP0CFG);

    /* Enable device events (reset/connect/disconnect/etc.). */
    mw(DWC3_DEVTEN, 0x00000E47U);
    mw(USB_REG_M2STAGE, USB_M2_DEVTEN);

    /* Prime EP0-out for the first SETUP. */
    if (!usb_ep0_prime_setup())
    {
        mw(USB_REG_M2STAGE, USB_M2_ERR_DEPCMD);
        return;
    }
    mw(USB_REG_M2STAGE, USB_M2_EP0PRIME);

    /* RUN_STOP: start the controller, wait for it to leave halt. */
    reg = mr(DWC3_DCTL);
    reg &= ~DCTL_ULSTCHNGREQ_MASK;
    reg |=  DCTL_RUN_STOP;
    mw(DWC3_DCTL, reg);
    if (!wait_bits(DWC3_DSTS, DSTS_DEVCTRLHLT, 0, 200))
    {
        mw(USB_REG_RUNDSTS, mr(DWC3_DSTS));
        mw(USB_REG_M2STAGE, USB_M2_ERR_RUN);
        return;
    }
    mw(USB_REG_RUNDSTS, mr(DWC3_DSTS));
    mw(USB_REG_M2STAGE, USB_M2_RUNNING);

    /*
     * Event-drain loop (Layer A). We do NOT answer descriptors yet; we only
     * decode and count what the host does so we can prove the link works.
     * Runs for a bounded window so the watcher thread stays responsive.
     */
    mw(USB_REG_M2STAGE, USB_M2_WATCHING);
    {
        uint32_t evtotal = 0, rst = 0, conn = 0, setup = 0;
        rt_tick_t deadline = rt_tick_get() + rt_tick_from_millisecond(20000); /* 20 s window */

        while (rt_tick_get() < deadline)
        {
            uint32_t nbytes = mr(DWC3_GEVNTCOUNT0) & GEVNTCOUNT_MASK;
            if (nbytes == 0)
            {
                rt_thread_mdelay(2);
                continue;
            }
            while (nbytes >= 4)
            {
                uint32_t e = ((volatile uint32_t *)s_evbuf)[s_evbuf_pos / 4];
                mw(USB_REG_LASTEVT, e);
                evtotal++;

                if (EVT_IS_DEVSPEC(e))
                {
                    uint32_t t = EVT_DEV_TYPE(e);
                    if (t == DEVEVT_RESET)
                    {
                        rst++;
                        /* A USB reset cancels the primed EP0 transfer and clears
                         * the device address. Put the address back to 0 and
                         * re-arm EP0-out so we can catch the next SETUP. */
                        uint32_t d = mr(DWC3_DCFG);
                        d &= ~(0x7fU << 3);          /* DevAddr = 0 */
                        mw(DWC3_DCFG, d);
                        usb_ep0_prime_setup();
                    }
                    else if (t == DEVEVT_CONNECTDONE)
                    {
                        conn++;
                        /* Connection done: the speed is settled. EP0 MPS stays 64.
                         * Re-arm EP0-out for the host's first GET_DESCRIPTOR. */
                        usb_ep0_prime_setup();
                        mw(USB_REG_CONNCNT, conn | ((mr(DWC3_DSTS) & 0x7U) << 16));
                    }
                }
                else if (EVT_EP_NUM(e) == 0 && EVT_EP_EVENT(e) == DEPEVT_XFERCOMPLETE)
                {
                    /* EP0-out XferComplete: the 8 SETUP bytes are in s_setup_buf. */
                    setup++;
                    mw(USB_REG_SETUP0, ((volatile uint32_t *)s_setup_buf)[0]);
                    mw(USB_REG_SETUP1, ((volatile uint32_t *)s_setup_buf)[1]);
                    /* re-arm EP0-out for the next SETUP so we keep seeing them */
                    usb_ep0_prime_setup();
                }

                /* advance ring cursor (wraps at EVBUF_SIZE) */
                s_evbuf_pos = (s_evbuf_pos + 4) & (EVBUF_SIZE - 1);
                nbytes -= 4;
                /* acknowledge 4 processed bytes to the controller */
                mw(DWC3_GEVNTCOUNT0, 4);
            }
            mw(USB_REG_EVCOUNT, evtotal);
            mw(USB_REG_RSTCNT, rst);
            if (conn) mw(USB_REG_CONNCNT, conn);
            mw(USB_REG_SETUPCNT, setup);
        }
    }
}

/* ================================================================== *
 *  M3 — hand off to the ported U-Boot DWC3 gadget stack               *
 *  Our clocks/PHY/VBUS bring-up (proven in M1) + dwc3_uboot_init +     *
 *  a minimal composite gadget, then poll the event loop so the host    *
 *  enumerates it (appears in lsusb).                                   *
 * ================================================================== */
extern int  dwc3_gadget_up(void);   /* dwc3_uboot_init + usb_composite_register */
extern void dwc3_gadget_poll(void); /* dwc3_uboot_handle_interrupt(0)           */

#define USB_M3_ENTER    0x00000040U
#define USB_M3_BRINGUP  0x00000041U
#define USB_M3_GADGETOK 0x00000042U
#define USB_M3_POLLING  0x00000043U
#define USB_M3_ERR_CORE 0x000000E5U
#define USB_M3_ERR_UP   0x000000E6U

static void usb_m3_uboot_gadget(void)
{
    int ret;
    uint32_t iters = 0;
    rt_tick_t deadline;

    mw(USB_REG_M2STAGE, USB_M3_ENTER);

    /* Clocks + PHY + forced VBUS (what dwc3_uboot_init does NOT do). */
    if (!usb_core_bringup_device())
    {
        mw(USB_REG_M2STAGE, USB_M3_ERR_CORE);
        return;
    }
    mw(USB_REG_M2STAGE, USB_M3_BRINGUP);

    /* dwc3_uboot_init (core + gadget) + register the minimal composite. */
    ret = dwc3_gadget_up();
    mw(USB_REG_VERDICT, (uint32_t)ret);
    if (ret != 0)
    {
        mw(USB_REG_M2STAGE, USB_M3_ERR_UP);
        return;
    }
    mw(USB_REG_M2STAGE, USB_M3_GADGETOK);

    /* Poll the U-Boot event loop hard for a window so enumeration completes.
     * The host drives it within ~100 ms once D+ is pulled up. */
    mw(USB_REG_M2STAGE, USB_M3_POLLING);
    deadline = rt_tick_get() + rt_tick_from_millisecond(60000);
    while (rt_tick_get() < deadline)
    {
        dwc3_gadget_poll();
        if ((++iters & 0x3fffU) == 0)
            mw(USB_REG_EVCOUNT, iters);   /* heartbeat */
    }
    mw(USB_REG_EVCOUNT, iters);
}

/* ================================================================== *
 *  M4 — U-Boot xHCI HOST: enumerate devices attached to the RV1106     *
 * ================================================================== */
extern int          dwc3_host_up(void);        /* xhci init + usb_init(); #devices */
extern unsigned int dwc3_host_dev_id(int i);   /* (idVendor<<16)|idProduct of dev i */

#define USB_M4_ENTER    0x00000050U
#define USB_M4_BRINGUP  0x00000051U
#define USB_M4_ENUM     0x00000052U

/* Host HW bring-up: clocks + resets + innosilicon PHY only. NO forced bvalid
 * (in host mode the hub sources VBUS), and NO device-mode gctl/soft-reset —
 * the ported U-Boot dwc3_core_init() does the controller core + host mode. */
static void usb_host_bringup_hw(void)
{
    HAL_CRU_ClkEnable(ACLK_USBOTG_GATE);
    HAL_CRU_ClkEnable(CLK_REF_USBOTG_GATE);
    HAL_CRU_ClkEnable(CLK_UTMI_USBOTG_GATE);
    HAL_CRU_ClkEnable(PCLK_USBPHY_GATE);
    HAL_CRU_ClkEnable(CLK_REF_USBPHY_GATE);

    /* Pulse the PHY resets (assert -> deassert) so the analog PHY re-initialises
     * from a clean POR state — same as a fresh boot. Just deasserting is a no-op
     * if Linux already left them low, so the PHY keeps whatever (bad) state it
     * was in after our controller soft-reset, and the HS chirp never completes. */
    HAL_CRU_ClkResetAssert(SRST_RESETN_USBPHY_POR);
    HAL_CRU_ClkResetAssert(SRST_RESETN_USBPHY_OTG);
    HAL_CRU_ClkResetAssert(SRST_PRESETN_USBPHY);
    rt_thread_mdelay(5);
    HAL_CRU_ClkResetDeassert(SRST_ARESETN_USBOTG);
    HAL_CRU_ClkResetDeassert(SRST_PRESETN_USBPHY);
    HAL_CRU_ClkResetDeassert(SRST_RESETN_USBPHY_POR);
    HAL_CRU_ClkResetDeassert(SRST_RESETN_USBPHY_OTG);
    rt_thread_mdelay(5);
    usb_phy_init();

    /* Match Linux's working golden exactly: GRF 0x50 reads 0x8C00 = iddig_en
     * (bit9)=0 (do NOT override the ID pin) and iddig_output (bit10)=1. The host
     * role is set by GCTL.PRTCAPDIR=HOST, not the ID pin, so forcing iddig is
     * neither needed nor what Linux does (verified: forcing it did not help the
     * downstream hub anyway). Hiword-mask write: mask bits 9,10 -> 0x0600, value
     * bit10=1 -> 0x0400. */
    mw(GRF_BASE + 0x0050U, 0x06000400U);
    rt_thread_mdelay(2);
}

static void usb_m4_uboot_host(void)
{
    int n;
    mw(USB_REG_M2STAGE, USB_M4_ENTER);

    /* Zero the hub-scan diagnostic markers first: they live in DDR that survives a
     * hot-reload, so stale values from a previous run otherwise masquerade as this
     * run's result. usb_hub.c only writes them when a port is actually scanned. */
    mw(0xff6ff9d8U, 0); mw(0xff6ff9dcU, 0); mw(0xff6ff9e0U, 0);
    mw(0xff6ff9e4U, 0); mw(0xff6ff9e8U, 0);
    mw(0xff6ff9ecU, 0); mw(0xff6ff9f0U, 0); mw(0xff6ff9f4U, 0);
    mw(0xff6ff9c8U, 0); mw(0xff6ff9ccU, 0);
    mw(0xff6ffa40U, 0); mw(0xff6ffa44U, 0); mw(0xff6ffa48U, 0); mw(0xff6ffa4cU, 0);
    mw(0xff6ffa50U, 0); mw(0xff6ffa54U, 0); mw(0xff6ffa58U, 0); mw(0xff6ffa5cU, 0);
    mw(0xff6ffa60U, 0); mw(0xff6ffa64U, 0); mw(0xff6ffa68U, 0);
    mw(0xff6ffa6cU, 0); mw(0xff6ffa70U, 0); mw(0xff6ffa74U, 0);
    mw(0xff6ffa78U, 0); mw(0xff6ffa7cU, 0); mw(0xff6ffa80U, 0); mw(0xff6ffa84U, 0);
    mw(0xff6ffa88U, 0); mw(0xff6ffa8cU, 0); mw(0xff6ffa90U, 0);
    mw(0xff6ffa94U, 0); mw(0xff6ffa98U, 0); mw(0xff6ffa9cU, 0);
    mw(0xff6ffaa0U, 0); mw(0xff6ffaa4U, 0);

    usb_host_bringup_hw();
    mw(USB_REG_M2STAGE, USB_M4_BRINGUP);

    n = dwc3_host_up();                      /* xHCI init + enumerate the bus */
    mw(USB_REG_VERDICT, (uint32_t)n);        /* #devices enumerated (root hub + tree) */
    mw(USB_REG_ID,   dwc3_host_dev_id(0));   /* dev0 vid/pid (xHCI root hub)         */
    mw(USB_REG_DSTS, dwc3_host_dev_id(1));   /* dev1 vid/pid (SL2.1A hub)            */
    mw(0xff6ff9e4U,  dwc3_host_dev_id(2));   /* dev2 vid/pid (device on the hub)     */
    mw(0xff6ff9e8U,  dwc3_host_dev_id(3));   /* dev3 vid/pid (if any)                */
    mw(USB_REG_M2STAGE, USB_M4_ENUM);
}

/* ================================================================== *
 *  M6 — poll the enumerated HID mouse and show its motion             *
 *                                                                     *
 *  Runs AFTER M4 (cmd=5) has enumerated the bus; the enumerated       *
 *  devices and the running controller persist between commands. It    *
 *  finds the mouse's interrupt-IN endpoint and submits interrupt-IN   *
 *  transfers for a bounded window. Move the mouse and watch REPORTS /  *
 *  LAST / ACCX / ACCY change — that is data flow, not just "who are    *
 *  you". ACCX/ACCY are the running sums of the per-report dX/dY, so    *
 *  they grow as you slide the mouse in one direction.                 *
 * ================================================================== */
extern int          dwc3_host_find_mouse(void);
extern unsigned int dwc3_host_mouse_vidpid(void);
extern unsigned int dwc3_host_mouse_info(void);
extern int          dwc3_host_mouse_stream(unsigned long window_ms,
                        void (*cb)(void *arg, unsigned char *buf, int len),
                        void *arg);

/* Mouse marker block (A7 reads with `devmem`). Kept away from the M4 hub-scan
 * markers (0xff6ff9xx / 0xff6ffaxx) so the two never alias. */
#define USB_REG_MSE_PRESENT 0xff6ffb00U  /* 1 if a HID interrupt-IN device was found     */
#define USB_REG_MSE_VIDPID  0xff6ffb04U  /* VID:PID of the mouse                         */
#define USB_REG_MSE_INFO    0xff6ffb08U  /* [31:24]ep [23:16]bInterval [15:8]maxp [7:0]devnum */
#define USB_REG_MSE_POLLS   0xff6ffb0cU  /* total interrupt-IN attempts (idle heartbeat) */
#define USB_REG_MSE_REPORTS 0xff6ffb10U  /* successful reports (increments as you move)  */
#define USB_REG_MSE_LAST    0xff6ffb14U  /* last report [7:0]btn [15:8]dX [23:16]dY [31:24]len */
#define USB_REG_MSE_ACCX    0xff6ffb18U  /* signed running sum of dX (grows moving right)*/
#define USB_REG_MSE_ACCY    0xff6ffb1cU  /* signed running sum of dY (grows moving down) */
#define USB_REG_MSE_RC      0xff6ffb20U  /* last usb_int_msg return code (diagnostic)    */

#define USB_M6_ENTER    0x00000060U
#define USB_M6_POLLING  0x00000061U
#define USB_M6_DONE     0x00000062U
#define USB_M6_NOMOUSE  0x000000E7U

/* Running totals updated by the per-report callback below. */
static int s_mse_reports;
static int s_mse_accx;
static int s_mse_accy;

/* Called once per HID report by the streaming poll. Boot-protocol layout is
 * [0]=buttons, [1]=dX, [2]=dY (signed). Accumulate dX/dY so ACCX/ACCY grow as
 * the mouse slides one way, and publish the raw last report. */
static void mouse_report_cb(void *arg, unsigned char *buf, int len)
{
    (void)arg;
    mw(USB_REG_MSE_POLLS, (uint32_t)(s_mse_reports + 1)); /* activity heartbeat */
    if (len < 3)
        return;
    {
        signed char dx = (signed char)buf[1];
        signed char dy = (signed char)buf[2];
        s_mse_reports++;
        s_mse_accx += dx;
        s_mse_accy += dy;
        mw(USB_REG_MSE_REPORTS, (uint32_t)s_mse_reports);
        mw(USB_REG_MSE_LAST, ((uint32_t)buf[0])
                           | (((uint32_t)buf[1] & 0xff) << 8)
                           | (((uint32_t)buf[2] & 0xff) << 16)
                           | (((uint32_t)len & 0xff) << 24));
        mw(USB_REG_MSE_ACCX, (uint32_t)s_mse_accx);
        mw(USB_REG_MSE_ACCY, (uint32_t)s_mse_accy);
    }
}

static void usb_m6_mouse_poll(void)
{
    int nrep;

    mw(USB_REG_M2STAGE, USB_M6_ENTER);

    /* Clear the marker block: it lives in DDR that survives a hot-reload, so
     * stale values from a previous run would otherwise look like this run. */
    mw(USB_REG_MSE_PRESENT, 0); mw(USB_REG_MSE_VIDPID, 0); mw(USB_REG_MSE_INFO, 0);
    mw(USB_REG_MSE_POLLS, 0);   mw(USB_REG_MSE_REPORTS, 0); mw(USB_REG_MSE_LAST, 0);
    mw(USB_REG_MSE_ACCX, 0);    mw(USB_REG_MSE_ACCY, 0);    mw(USB_REG_MSE_RC, 0);

    if (!dwc3_host_find_mouse())
    {
        mw(USB_REG_MSE_PRESENT, 0);
        mw(USB_REG_M2STAGE, USB_M6_NOMOUSE);
        return;
    }
    mw(USB_REG_MSE_PRESENT, 1);
    mw(USB_REG_MSE_VIDPID, dwc3_host_mouse_vidpid());
    mw(USB_REG_MSE_INFO,   dwc3_host_mouse_info());
    mw(USB_REG_M2STAGE, USB_M6_POLLING);

    /* Stream reports for 60 s, keeping the endpoint armed the whole time so a
     * moving mouse is caught report-by-report. The callback publishes markers. */
    s_mse_reports = 0;
    s_mse_accx = 0;
    s_mse_accy = 0;
    nrep = dwc3_host_mouse_stream(60000, mouse_report_cb, 0);
    mw(USB_REG_MSE_RC, (uint32_t)nrep);   /* total reports delivered this window */

    mw(USB_REG_M2STAGE, USB_M6_DONE);
}

/* ================================================================== *
 *  Watcher thread: wait for the A7 command and run the experiment     *
 * ================================================================== */
static void usb_probe_thread(void *arg)
{
    (void)arg;
    mw(USB_REG_CMD, 0);
    mw(USB_REG_STAGE, 0);
    mw(USB_REG_VERDICT, 0);

    for (;;)
    {
        uint32_t cmd = mr(USB_REG_CMD);
        if (cmd == 1U)      { usb_m0_read_id();       mw(USB_REG_CMD, USB_CMD_DONE); }
        else if (cmd == 2U) { usb_m1_cold_bringup();  mw(USB_REG_CMD, USB_CMD_DONE); }
        else if (cmd == 3U) { usb_m2_run_and_watch(); mw(USB_REG_CMD, USB_CMD_DONE); }
        else if (cmd == 4U) { usb_m3_uboot_gadget();  mw(USB_REG_CMD, USB_CMD_DONE); }
        else if (cmd == 5U) { usb_m4_uboot_host();    mw(USB_REG_CMD, USB_CMD_DONE); }
        else if (cmd == 6U) { usb_m6_mouse_poll();    mw(USB_REG_CMD, USB_CMD_DONE); }
        rt_thread_mdelay(200);   /* quiet polling; the A7 sets the pace */
    }
}

int usb_dwc3_probe_init(void)
{
    rt_thread_t t = rt_thread_create("usbprobe", usb_probe_thread, RT_NULL,
                                     4096, 25, 20);
    if (t != RT_NULL)
        rt_thread_startup(t);
    return 0;
}
INIT_APP_EXPORT(usb_dwc3_probe_init);
