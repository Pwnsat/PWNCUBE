#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/poll.h>
#include "sx1262.h"
#include "sx1262_regs.h"

/* Forward declarations from other modules */
extern int sx1262_set_standby(struct sx1262_device *dev, uint8_t standby_cfg);
extern int sx1262_set_sleep(struct sx1262_device *dev, uint8_t sleep_cfg);
extern int sx1262_set_frequency(struct sx1262_device *dev, uint32_t freq_hz);
extern int sx1262_set_tx_params(struct sx1262_device *dev, int8_t power_dbm, uint8_t ramp_time);
extern int sx1262_set_modulation_params(struct sx1262_device *dev, uint8_t sf, uint32_t bw, uint8_t cr, bool ldro);
extern int sx1262_set_packet_params(struct sx1262_device *dev, uint16_t preamble_len, uint8_t hdr_type, uint8_t payload_len, uint8_t crc_type, uint8_t iq_inverted);
extern int sx1262_set_tx(struct sx1262_device *dev, uint32_t timeout_ms);
extern int sx1262_set_rx(struct sx1262_device *dev, uint32_t timeout_ms);
extern int sx1262_get_status(struct sx1262_device *dev, uint8_t *status);
extern int sx1262_get_rssi_inst(struct sx1262_device *dev, int16_t *rssi);
extern int sx1262_reset(struct sx1262_device *sdev);
extern int sx1262_init(struct sx1262_device *dev, uint32_t freq_hz);
extern int sx1262_read_buffer(struct sx1262_device *dev, uint8_t offset, uint8_t *buf, size_t len);
extern int sx1262_write_buffer(struct sx1262_device *dev, uint8_t offset, const uint8_t *buf, size_t len);
extern int sx1262_get_packet_status(struct sx1262_device *dev, int16_t *rssi_pkt, int8_t *snr);
extern int sx1262_get_irq_status(struct sx1262_device *dev, uint16_t *irq_status);
extern int sx1262_clear_irq_status(struct sx1262_device *dev, uint16_t irq_mask);
extern int sx1262_poll_irq(struct sx1262_device *dev, uint16_t flag, unsigned long timeout_ms);
extern int sx1262_get_rx_buffer_status(struct sx1262_device *dev, uint8_t *payload_len, uint8_t *start_ptr);
extern int sx1262_set_tx_continuous_wave(struct sx1262_device *dev);
extern int sx1262_calibrate_image_for_freq(struct sx1262_device *dev, uint32_t freq_hz);
extern int sx1262_read_register(struct sx1262_device *dev, uint16_t addr, uint8_t *val);
extern int sx1262_write_register(struct sx1262_device *dev, uint16_t addr, uint8_t val);
extern void sx1262_set_antsw(struct sx1262_device *dev, int mode);
extern struct sx1262_device *sx1262_devices[];


static int sx1262_open(struct inode *inode, struct file *filp)
{
    unsigned int minor = iminor(inode);
    if (minor >= MAX_DEVICES || !sx1262_devices[minor])
        return -ENODEV;
    filp->private_data = sx1262_devices[minor];
    return 0;
}

static long sx1262_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct sx1262_device *dev = filp->private_data;
    uint32_t u32_val;
    int8_t s8_val;
    uint8_t u8_val;
    int16_t s16_val;
    struct sx1262_modem_config mc;
    struct sx1262_packet_config pc;
    int ret = 0;

    if (!dev) return -ENODEV;

    mutex_lock(&dev->lock);

    switch (cmd) {
    case SX1262_IOCTL_RESET:
        ret = sx1262_reset(dev);
        break;

    case SX1262_IOCTL_SET_STANDBY:
        ret = sx1262_set_standby(dev, SX1262_STANDBY_RC);
        break;

    case SX1262_IOCTL_SET_SLEEP:
        ret = sx1262_set_sleep(dev, SX1262_SLEEP_RTC);
        break;

    case SX1262_IOCTL_SET_FREQ:
        if (copy_from_user(&u32_val, (void __user *)arg, sizeof(u32_val)))
            { ret = -EFAULT; break; }
        /* Datasheet: re-run image calibration for the new band, in standby. */
        sx1262_set_standby(dev, SX1262_STANDBY_RC);
        sx1262_calibrate_image_for_freq(dev, u32_val);
        ret = sx1262_set_frequency(dev, u32_val);
        break;

    case SX1262_IOCTL_SET_CW:
        ret = sx1262_set_tx_continuous_wave(dev);
        break;

    case SX1262_IOCTL_SET_POWER:
        if (copy_from_user(&s8_val, (void __user *)arg, sizeof(s8_val)))
            { ret = -EFAULT; break; }
        ret = sx1262_set_tx_params(dev, s8_val, 0x04);
        break;

    case SX1262_IOCTL_SET_MODEM:
        if (copy_from_user(&mc, (void __user *)arg, sizeof(mc)))
            { ret = -EFAULT; break; }
        ret = sx1262_set_modulation_params(dev, mc.sf, mc.bw, mc.cr, false);
        break;

    case SX1262_IOCTL_SET_PACKET:
        if (copy_from_user(&pc, (void __user *)arg, sizeof(pc)))
            { ret = -EFAULT; break; }
        ret = sx1262_set_packet_params(dev, 8, pc.header_type, pc.payload_len, pc.crc_type, pc.iq_inverted);
        break;

    case SX1262_IOCTL_SET_TX:
        if (copy_from_user(&u32_val, (void __user *)arg, sizeof(u32_val)))
            { ret = -EFAULT; break; }
        ret = sx1262_set_tx(dev, u32_val);
        break;

    case SX1262_IOCTL_SET_RX:
        if (copy_from_user(&u32_val, (void __user *)arg, sizeof(u32_val)))
            { ret = -EFAULT; break; }
        ret = sx1262_set_rx(dev, u32_val);
        break;

    case SX1262_IOCTL_GET_STATUS:
        ret = sx1262_get_status(dev, &u8_val);
        if (ret == 0)
            ret = copy_to_user((void __user *)arg, &u8_val, sizeof(u8_val)) ? -EFAULT : 0;
        break;

    case SX1262_IOCTL_GET_RSSI:
        ret = sx1262_get_rssi_inst(dev, &s16_val);
        if (ret == 0)
            ret = copy_to_user((void __user *)arg, &s16_val, sizeof(s16_val)) ? -EFAULT : 0;
        break;

    case SX1262_IOCTL_SET_ANTSW:
        if (copy_from_user(&u8_val, (void __user *)arg, sizeof(u8_val)))
            { ret = -EFAULT; break; }
        dev->ant_sw_mode = u8_val;
        if (u8_val == SX1262_ANTSW_TX)
            sx1262_set_antsw(dev, SX1262_ANTSW_TX);
        else if (u8_val == SX1262_ANTSW_RX)
            sx1262_set_antsw(dev, SX1262_ANTSW_RX);
        break;

    case SX1262_IOCTL_READ_REG: {
        struct sx1262_reg_access ra;
        if (copy_from_user(&ra, (void __user *)arg, sizeof(ra)))
            { ret = -EFAULT; break; }
        ret = sx1262_read_register(dev, ra.addr, &ra.val);
        if (ret == 0)
            ret = copy_to_user((void __user *)arg, &ra, sizeof(ra)) ? -EFAULT : 0;
        break;
    }

    case SX1262_IOCTL_WRITE_REG: {
        struct sx1262_reg_access ra;
        if (copy_from_user(&ra, (void __user *)arg, sizeof(ra)))
            { ret = -EFAULT; break; }
        ret = sx1262_write_register(dev, ra.addr, ra.val);
        break;
    }

    case SX1262_IOCTL_GET_IRQ_STATUS: {
        uint16_t irq_val;
        ret = sx1262_get_irq_status(dev, &irq_val);
        if (ret == 0)
            ret = copy_to_user((void __user *)arg, &irq_val, sizeof(irq_val)) ? -EFAULT : 0;
        break;
    }

    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&dev->lock);
    return ret;
}

static ssize_t sx1262_write(struct file *filp, const char __user *buf,
                             size_t count, loff_t *off)
{
    struct sx1262_device *dev = filp->private_data;
    uint8_t kbuf[SX1262_MAX_PACKET_LEN];
    int ret;

    if (!dev) return -ENODEV;
    if (count > SX1262_MAX_PACKET_LEN)
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    mutex_lock(&dev->lock);

    /* Write payload to FIFO */
    ret = sx1262_write_buffer(dev, 0, kbuf, count);
    if (ret) goto out;

    /* Set packet params: preamble=8, variable length, payload=count, CRC on */
    ret = sx1262_set_packet_params(dev, 8, 0, count, 1, 0);
    if (ret) goto out;

    /* Start TX with given timeout (5 second default) */
    ret = sx1262_set_tx(dev, 5000);

out:
    mutex_unlock(&dev->lock);
    return ret ? ret : count;
}

static ssize_t sx1262_read(struct file *filp, char __user *buf,
                            size_t count, loff_t *off)
{
    struct sx1262_device *dev = filp->private_data;
    uint8_t kbuf[SX1262_MAX_PACKET_LEN];
    uint16_t irq_status;
    int16_t rssi;
    int8_t snr;
    size_t read_len;
    int ret;

    if (!dev) return -ENODEV;
    if (count > SX1262_MAX_PACKET_LEN)
        count = SX1262_MAX_PACKET_LEN;

    /* Poll for RX_DONE (with timeout from set_rx call, or 10s default) */
    ret = sx1262_poll_irq(dev, SX1262_IRQ_RX_DONE | SX1262_IRQ_TIMEOUT, 10000);
    if (ret == -ETIMEDOUT) {
        return 0;  /* timeout, no data */
    }
    if (ret) return ret;

    mutex_lock(&dev->lock);

    /* Read IRQ status */
    ret = sx1262_get_irq_status(dev, &irq_status);
    if (ret) goto out;

    sx1262_clear_irq_status(dev, irq_status);

    if (irq_status & SX1262_IRQ_RX_DONE) {
        uint8_t payload_len = 0, start_ptr = 0;

        /* CRC error on the received packet: report no data */
        if (irq_status & SX1262_IRQ_CRC_ERR) {
            dev_warn(&dev->spi->dev, "RX CRC error (irq=0x%04x)\n", irq_status);
            ret = 0;
            goto out;
        }

        sx1262_get_packet_status(dev, &rssi, &snr);

        /* Query actual received length and buffer start offset */
        ret = sx1262_get_rx_buffer_status(dev, &payload_len, &start_ptr);
        if (ret) goto out;

        read_len = payload_len;
        if (read_len > count)
            read_len = count;

        /* Read exactly the received payload from the FIFO */
        ret = sx1262_read_buffer(dev, start_ptr, kbuf, read_len);
        if (ret) goto out;

        dev_info(&dev->spi->dev, "RX done: len=%u rssi=%d snr=%d\n",
                 payload_len, rssi, snr);

        mutex_unlock(&dev->lock);

        if (read_len && copy_to_user(buf, kbuf, read_len))
            return -EFAULT;
        return read_len;
    }

    ret = 0; /* No data, but no error */
out:
    mutex_unlock(&dev->lock);
    return ret;
}

static unsigned int sx1262_poll(struct file *filp, struct poll_table_struct *wait)
{
    struct sx1262_device *dev = filp->private_data;
    unsigned int mask = 0;

    if (!dev) return POLLERR;

    poll_wait(filp, &dev->read_wq, wait);

    if (atomic_read(&dev->irq_flags)) {
        mask |= POLLIN | POLLRDNORM;
        atomic_set(&dev->irq_flags, 0);
    }

    return mask;
}

const struct file_operations sx1262_fops = {
    .owner          = THIS_MODULE,
    .open           = sx1262_open,
    .unlocked_ioctl = sx1262_ioctl,
    .write          = sx1262_write,
    .read           = sx1262_read,
    .poll           = sx1262_poll,
};
