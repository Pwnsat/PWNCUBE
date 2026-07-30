/*
 * dwc3_host.c — bind the ported U-Boot xHCI host stack to RT-Thread.
 *
 * The ported U-Boot host code (xhci*.c + usb_core.c + usb_hub.c) drives
 * enumeration; usb_init() -> usb_lowlevel_init() -> xhci_hcd_init(). U-Boot
 * expects the SoC glue to supply xhci_hcd_init(): point it at the DWC3 xHCI
 * register block, run the DWC3 core init and put the controller in HOST mode.
 * This mirrors xhci_dwc3.c's DM probe, minus the driver model.
 *
 * Clocks + the innosilicon USB2 PHY (0xff3e0000/GRF) are brought up on the
 * probe.c side (HAL_CRU) before dwc3_host_up() is called — same split as the
 * gadget path.
 */

#include <common.h>
#include <usb.h>
#include <usb/xhci.h>
#include <linux/usb/dwc3.h>

#define RV1106_DWC3_BASE 0xffb00000UL

/* defined (global) in the ported xhci_dwc3.c */
extern int  dwc3_core_init(struct dwc3 *dwc3_reg);
extern void dwc3_set_mode(struct dwc3 *dwc3_reg, u32 mode);

/* U-Boot's non-DM host controller hook, called from usb_lowlevel_init(). */
int xhci_hcd_init(int index, struct xhci_hccr **ret_hccr,
                  struct xhci_hcor **ret_hcor)
{
    struct xhci_hccr *hccr = (struct xhci_hccr *)RV1106_DWC3_BASE;
    struct xhci_hcor *hcor = (struct xhci_hcor *)((uintptr_t)hccr +
                             HC_LENGTH(xhci_readl(&hccr->cr_capbase)));
    struct dwc3 *dwc3_reg = (struct dwc3 *)((char *)hccr + DWC3_REG_OFFSET);
    u32 reg;

    (void)index;

    HDBG(0xA0);
    dwc3_core_init(dwc3_reg);
    HDBG(0xA1);

    /* Replicate Linux's live host golden exactly (captured with the mouse working
     * through the hub). GUCTL1 (0xffb0c11c) = 0x9505018A and GUCTL (0xffb0c12c) =
     * 0x02004010. The low 16 bits already match after dwc3_core_init; this adds
     * TX_IPGAP_LINECHECK_DIS (GUCTL1 bit28) + the device L1/L2 bits Linux sets,
     * and — the important one for host — GUCTL.HSTINAUTORETRY (bit14), the host
     * IN-transaction auto-retry Linux enables (needed for the FS mouse across the
     * hub TT). Note Linux does NOT set PARKMODE_DISABLE_HS (bit17); the earlier
     * Polling stall was the PHY pre-emphasis bug, not park mode. */
    writel(0x9505018AUL, (void *)0xffb0c11cUL);   /* GUCTL1  */
    writel(0x02004010UL, (void *)0xffb0c12cUL);   /* GUCTL (matches Linux golden) */
    /* GUCTL2: the one golden host register dwc3_core_init leaves at default and
     * that we never set. Linux's live host golden reads 0x0000040D here. Its
     * RST_ACTBITLATER bit (and disable-U2-exit-LFPS bits) affect control/split
     * transaction sequencing — a candidate for the deterministic FS DATA-IN
     * split halt. Match the golden. */
    writel(0x0000040DUL, (void *)0xffb0c19cUL);   /* GUCTL2  */

    /* UTMI 16-bit wide + the RV1106 quirks (same as dr_mode=host DTS). */
    reg = readl(&dwc3_reg->g_usb2phycfg[0]);
    reg |= DWC3_GUSB2PHYCFG_PHYIF;
    reg &= ~DWC3_GUSB2PHYCFG_USBTRDTIM_MASK;
    reg |= DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT;
    reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
    reg &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;
    reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
    writel(reg, &dwc3_reg->g_usb2phycfg[0]);

    dwc3_set_mode(dwc3_reg, DWC3_GCTL_PRTCAP_HOST);
    HDBG(0xA2);

    *ret_hccr = hccr;
    *ret_hcor = hcor;
    return 0;
}

void xhci_hcd_stop(int index)
{
    (void)index;
}

/* entry point for the usbprobe thread: enumerate the bus (hub + devices).
 * Returns the number of enumerated USB devices (root hub's children count;
 * a hub + a mouse => 2+), or a negative errno from usb_init(). */
int dwc3_host_up(void)
{
    int r = usb_init();     /* scans, enumerates everything attached */
    int n = 0;
    if (r < 0)
        return r;
    while (usb_get_dev_index(n) != NULL)
        n++;
    return n;
}

/* (idVendor<<16)|idProduct of the i-th enumerated device, 0 if none. */
unsigned int dwc3_host_dev_id(int i)
{
    struct usb_device *d = usb_get_dev_index(i);
    if (!d)
        return 0;
    return ((unsigned int)d->descriptor.idVendor << 16) | d->descriptor.idProduct;
}

/* ================================================================== *
 *  HID mouse read-back — prove data flow, not just enumeration        *
 *                                                                     *
 *  Enumeration only reads WHO the device is (descriptors + address).  *
 *  To observe the mouse *move*, the host must poll the device's       *
 *  interrupt-IN endpoint and read its HID reports. That is a periodic *
 *  IN transfer; for a Full-Speed mouse behind the High-Speed hub the  *
 *  xHC performs the periodic split to the hub's Transaction Translator*
 *  transparently, using the endpoint context the enumeration already  *
 *  built (xhci_set_configuration adds every interface endpoint, not   *
 *  just EP0). We only have to submit the transfer.                    *
 * ================================================================== */

/* Cached after dwc3_host_find_mouse(): the enumerated device + its
 * interrupt-IN endpoint. Reports land in s_mouse_report (DCACHE off ->
 * coherent with the controller, so no cache maintenance). */
static struct usb_device *s_mouse_dev;
static unsigned char       s_mouse_ep;        /* bEndpointAddress (dir bit + number) */
static int                 s_mouse_maxp;      /* transfer size to request (<= 8)     */
static int                 s_mouse_interval;  /* ep bInterval (service interval)     */
static unsigned char       s_mouse_report[8] __attribute__((aligned(64)));

/* Find the first enumerated HID device that exposes an interrupt-IN endpoint
 * (a mouse or keyboard). Caches it and best-effort switches its HID interface
 * into BOOT protocol so the report layout is the fixed [buttons][dX][dY].
 * Returns 1 if found, 0 otherwise. */
int dwc3_host_find_mouse(void)
{
    int i;

    s_mouse_dev = NULL;
    for (i = 0; ; i++) {
        struct usb_device *d = usb_get_dev_index(i);
        int ifn;

        if (!d)
            break;
        for (ifn = 0; ifn < d->config.no_of_if; ifn++) {
            struct usb_interface *iface = &d->config.if_desc[ifn];
            int epn;

            if (iface->desc.bInterfaceClass != USB_CLASS_HID)
                continue;
            for (epn = 0; epn < iface->no_of_ep; epn++) {
                struct usb_endpoint_descriptor *ep = &iface->ep_desc[epn];

                if ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
                        != USB_ENDPOINT_XFER_INT)
                    continue;
                if (!(ep->bEndpointAddress & USB_DIR_IN))
                    continue;

                /* Found the HID interrupt-IN endpoint. Cache it. */
                s_mouse_dev = d;
                s_mouse_ep = ep->bEndpointAddress;
                s_mouse_interval = ep->bInterval;
                {
                    unsigned long pipe =
                        usb_rcvintpipe(d, ep->bEndpointAddress);
                    int mp = usb_maxpacket(d, pipe);
                    /* A boot HID report is <= 8 bytes; cap the request. */
                    s_mouse_maxp = (mp > 8) ? 8 : mp;
                }

                /* Best-effort SET_PROTOCOL(boot=0) on the HID interface so the
                 * report is the fixed [buttons][dX][dY]. If the device does not
                 * support the boot protocol the control transfer STALLs; ignore
                 * it and fall back to the report-protocol layout (movement still
                 * shows up in bytes 1..2 on the vast majority of mice). */
                usb_control_msg(d, usb_sndctrlpipe(d, 0),
                        USB_REQ_SET_PROTOCOL,
                        USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                        0 /* boot protocol */,
                        iface->desc.bInterfaceNumber,
                        NULL, 0, 1000);
                return 1;
            }
        }
    }
    return 0;
}

/* (idVendor<<16)|idProduct of the cached mouse, 0 if none. */
unsigned int dwc3_host_mouse_vidpid(void)
{
    if (!s_mouse_dev)
        return 0;
    return ((unsigned int)s_mouse_dev->descriptor.idVendor << 16) |
        s_mouse_dev->descriptor.idProduct;
}

/* Packed diagnostics of the cached endpoint:
 * [31:24]=bEndpointAddress [23:16]=bInterval [15:8]=maxpacket [7:0]=devnum. */
unsigned int dwc3_host_mouse_info(void)
{
    if (!s_mouse_dev)
        return 0;
    return ((unsigned int)s_mouse_ep << 24) |
        ((unsigned int)(s_mouse_interval & 0xff) << 16) |
        ((unsigned int)(s_mouse_maxp & 0xff) << 8) |
        ((unsigned int)(s_mouse_dev->devnum & 0xff));
}

/* Submit ONE interrupt-IN transfer to the cached mouse and copy up to 8 report
 * bytes into out8 (zero-padded). Returns the number of bytes transferred
 * (>=1 on a real report), or a negative errno. xhci_bulk_tx blocks up to
 * XHCI_TIMEOUT (5 s): if the mouse moves within that window the pending TRB
 * completes immediately with the report; otherwise it times out (NAK) and the
 * caller simply polls again. */
int dwc3_host_mouse_read(unsigned char *out8)
{
    unsigned long pipe;
    int ret, n, k;

    if (!s_mouse_dev)
        return -1;

    pipe = usb_rcvintpipe(s_mouse_dev, s_mouse_ep);
    for (k = 0; k < 8; k++)
        s_mouse_report[k] = 0;

    ret = usb_int_msg(s_mouse_dev, pipe, s_mouse_report,
              s_mouse_maxp, s_mouse_interval, false);
    if (ret < 0)
        return ret;

    n = s_mouse_dev->act_len;
    if (n > 8)
        n = 8;
    for (k = 0; k < 8; k++)
        out8[k] = (k < n) ? s_mouse_report[k] : 0;
    return n;
}

/* High-duty-cycle streaming poll (see xhci_int_stream). Keeps the mouse's
 * interrupt-IN endpoint armed for window_ms and invokes cb() for every HID
 * report that completes, so a moving mouse is observed report-by-report instead
 * of missed in the gaps of a one-shot poll. Returns the number of reports
 * delivered, or -1 if no mouse is cached. */
extern int xhci_int_stream(struct usb_device *udev, unsigned long pipe,
                           void *buffer, int length, unsigned long window_ms,
                           void (*cb)(void *arg, unsigned char *buf, int len),
                           void *arg);

int dwc3_host_mouse_stream(unsigned long window_ms,
                           void (*cb)(void *arg, unsigned char *buf, int len),
                           void *arg)
{
    unsigned long pipe;

    if (!s_mouse_dev)
        return -1;
    pipe = usb_rcvintpipe(s_mouse_dev, s_mouse_ep);
    return xhci_int_stream(s_mouse_dev, pipe, s_mouse_report, s_mouse_maxp,
                           window_ms, cb, arg);
}

/* ---- tiny U-Boot/RT-Thread symbol stubs the host code references ---- */
char *env_get(const char *name) { (void)name; return 0; }   /* no U-Boot env */
long  simple_strtol(const char *c, char **e, unsigned int b) { return strtol(c, e, b); }
void *rt_console_get_device(void) { return 0; }             /* console off on MCU */
