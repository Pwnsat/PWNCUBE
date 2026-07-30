/*
 * dwc3_glue.c — bind the ported U-Boot DWC3 gadget stack to RT-Thread and
 * present a minimal USB device so a host enumerates it (lsusb).
 *
 * The heavy lifting (core init, EP0 chapter-9 state machine, event handling)
 * is the ported U-Boot code. This file only:
 *   - supplies the few RT-Thread-backed symbols the shim declares
 *     (rt_malloc_align, udelay, g_dwc3_tick_hz),
 *   - describes the controller instance (struct dwc3_device, base 0xffb00000),
 *   - registers a tiny composite gadget with ONE vendor interface and no data
 *     endpoints — enough to enumerate,
 *   - exposes dwc3_gadget_up() / dwc3_gadget_poll() for the usbprobe thread.
 */

#include <common.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>
#include <dwc3-uboot.h>

/* ---- RT-Thread-backed symbols the shim (dwc3_port.h) declares ---- */
unsigned long g_dwc3_tick_hz = 1000;

void udelay(unsigned long us)
{
    /* Busy-wait on the machine cycle counter is overkill here; the ported code
     * uses udelay only for short (<=1ms) reset settle waits. A calibrated spin
     * is enough. SCR1 runs ~1.2 GHz-class? No — treat conservatively. */
    volatile unsigned long n = us * 200UL;   /* ponytail: rough spin, reset waits only */
    while (n--) __asm__ volatile("nop");
}

/* ---- USB descriptors (minimal, single vendor interface) ---- */
#define PROBE_VID 0x2207        /* Rockchip (USB-IF assigned) */
#define PROBE_PID 0xdc30        /* "dwc3" — arbitrary product id */

static struct usb_device_descriptor probe_device_desc = {
    .bLength            = USB_DT_DEVICE_SIZE,
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = __constant_cpu_to_le16(0x0200),
    .bDeviceClass       = 0,      /* per-interface */
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = 64,
    .idVendor           = __constant_cpu_to_le16(PROBE_VID),
    .idProduct          = __constant_cpu_to_le16(PROBE_PID),
    .bcdDevice          = __constant_cpu_to_le16(0x0100),
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

static struct usb_interface_descriptor probe_intf_desc = {
    .bLength            = sizeof(probe_intf_desc),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,        /* filled at bind */
    .bNumEndpoints      = 0,        /* enumerate only, no data endpoints */
    .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface         = 0,
};

static struct usb_descriptor_header *probe_descriptors[] = {
    (struct usb_descriptor_header *)&probe_intf_desc,
    NULL,
};

/* ---- strings ---- */
static struct usb_string probe_strings_defs[] = {
    { 1, "PwnCube" },
    { 2, "MCU DWC3 probe" },
    { 3, "0001" },
    {  }
};
static struct usb_gadget_strings probe_strings = {
    .language = 0x0409,   /* en-US */
    .strings  = probe_strings_defs,
};
static struct usb_gadget_strings *probe_strings_arr[] = { &probe_strings, NULL };

/* ---- the function ---- */
static int probe_fn_bind(struct usb_configuration *c, struct usb_function *f)
{
    int id = usb_interface_id(c, f);
    if (id < 0)
        return id;
    probe_intf_desc.bInterfaceNumber = id;
    return 0;
}
static void probe_fn_unbind(struct usb_configuration *c, struct usb_function *f) { }
static int  probe_fn_set_alt(struct usb_function *f, unsigned intf, unsigned alt) { return 0; }
static void probe_fn_disable(struct usb_function *f) { }

static struct usb_function probe_function = {
    .name           = "probe",
    .descriptors    = probe_descriptors,   /* full-speed */
    .hs_descriptors = probe_descriptors,   /* high-speed: same (no endpoints) */
    .bind           = probe_fn_bind,
    .unbind         = probe_fn_unbind,
    .set_alt        = probe_fn_set_alt,
    .disable        = probe_fn_disable,
};

/* ---- configuration ---- */
static int probe_config_bind(struct usb_configuration *c)
{
    return usb_add_function(c, &probe_function);
}
static struct usb_configuration probe_config = {
    .label              = "probe-cfg",
    .bConfigurationValue = 1,
    .iConfiguration     = 0,
    .bmAttributes       = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
    .bind               = probe_config_bind,
};

/* ---- composite device ---- */
static int probe_composite_bind(struct usb_composite_dev *cdev)
{
    return usb_add_config(cdev, &probe_config);
}
static struct usb_composite_driver probe_driver = {
    .name  = "probe",
    .dev   = &probe_device_desc,
    .strings = probe_strings_arr,
    .bind  = probe_composite_bind,
};

/* ---- controller instance (blueprint: evb_rv1126, base -> RV1106) ---- */
static struct dwc3_device dwc3_device_data = {
    .maximum_speed        = USB_SPEED_HIGH,
    .base                 = 0xffb00000,
    .dr_mode              = USB_DR_MODE_PERIPHERAL,
    .index                = 0,
    .dis_u2_susphy_quirk  = 1,
    .usb2_phyif_utmi_width = 16,
};

/* Rockchip USB-plug helper referenced by dwc3_core_init: we never force the
 * legacy USB2-only download path, so report "not forced". */
int rkusb_force_usb2_enabled(void) { return 0; }

/* U-Boot's poll wrapper; some paths call this by name. */
int usb_gadget_handle_interrupts(int index)
{
    dwc3_uboot_handle_interrupt(index);
    return 0;
}

/* ---- entry points for the usbprobe thread ---- */
int dwc3_gadget_up(void)
{
    int ret = dwc3_uboot_init(&dwc3_device_data);
    if (ret)
        return ret;
    return usb_composite_register(&probe_driver);
}

void dwc3_gadget_poll(void)
{
    dwc3_uboot_handle_interrupt(0);
}
