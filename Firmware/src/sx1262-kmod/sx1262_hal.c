#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/completion.h>
#include <linux/err.h>
#include "sx1262.h"
#include "sx1262_regs.h"

/* SPI: write command with optional data, no response */
int sx1262_spi_write(struct sx1262_device *dev, const uint8_t *data, size_t len)
{
    return spi_write(dev->spi, data, len);
}

/* SPI: write command, then read response */
int sx1262_spi_write_then_read(struct sx1262_device *dev,
                                const uint8_t *tx, size_t tx_len,
                                uint8_t *rx, size_t rx_len)
{
    return spi_write_then_read(dev->spi, tx, tx_len, rx, rx_len);
}

/* SPI: synchronous transfer of multiple segments */
int sx1262_spi_transfer(struct sx1262_device *dev,
                         struct spi_transfer *xfers, unsigned int num)
{
    unsigned int i;
    struct spi_message msg;
    spi_message_init(&msg);
    for (i = 0; i < num; i++)
        spi_message_add_tail(&xfers[i], &msg);
    return spi_sync(dev->spi, &msg);
}

/* Wait for BUSY pin to go low (not busy), with timeout */
int sx1262_wait_busy(struct sx1262_device *dev, unsigned long timeout_ms)
{
    unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
    while (gpiod_get_value(dev->busy)) {
        if (time_after(jiffies, timeout)) {
            dev_err(&dev->spi->dev, "BUSY timeout (dev %d) busy_val=%d\n",
                    dev->device_id, gpiod_get_value(dev->busy));
            return -ETIMEDOUT;
        }
        udelay(10);
    }
    return 0;
}

/* HW reset sequence */
int sx1262_reset(struct sx1262_device *dev)
{
    gpiod_set_value(dev->reset, 1);
    usleep_range(500, 1000);
    gpiod_set_value(dev->reset, 0);
    usleep_range(5000, 10000);
    return sx1262_wait_busy(dev, 100);
}

/* IRQ handler for DIO1 */
static irqreturn_t sx1262_dio1_irq(int irq, void *context)
{
    struct sx1262_device *dev = context;
    atomic_set(&dev->irq_flags, 1);
    complete(&dev->dio1_completion);
    wake_up_interruptible(&dev->read_wq);
    return IRQ_HANDLED;
}

/* Set ANT_SW state.
 * Verified on hardware (spectrum analyzer): TX = ant-sw0=1, ant-sw1=0 routes
 * the antenna to the PA and radiates; RX = ant-sw0=0, ant-sw1=1 routes it to
 * the LNA. */
void sx1262_set_antsw(struct sx1262_device *dev, int mode)
{
    if (!dev->ant_sw0 || !dev->ant_sw1)
        return;

    if (mode == SX1262_ANTSW_TX) {
        gpiod_set_value(dev->ant_sw0, 1);
        gpiod_set_value(dev->ant_sw1, 0);
    } else {
        gpiod_set_value(dev->ant_sw0, 0);
        gpiod_set_value(dev->ant_sw1, 1);
    }
}

/* Probe: parse DT, request GPIOs, IRQ */
struct sx1262_device *sx1262_probe(struct spi_device *spi, int id)
{
    struct device *dev = &spi->dev;
    struct sx1262_device *sdev;
    int ret;

    sdev = devm_kzalloc(dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev)
        return ERR_PTR(-ENOMEM);

    sdev->spi = spi;
    sdev->device_id = id;
    sdev->ant_sw_mode = SX1262_ANTSW_AUTO;
    mutex_init(&sdev->lock);
    init_completion(&sdev->dio1_completion);
    init_waitqueue_head(&sdev->read_wq);
    atomic_set(&sdev->irq_flags, 0);
    spi_set_drvdata(spi, sdev);

    /* Reset GPIO */
    sdev->reset = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
    if (IS_ERR(sdev->reset)) {
        dev_err(dev, "Failed to get reset GPIO\n");
        return ERR_CAST(sdev->reset);
    }
    gpiod_direction_output(sdev->reset, 1);

    /* BUSY GPIO */
    sdev->busy = devm_gpiod_get(dev, "busy", GPIOD_ASIS);
    if (IS_ERR(sdev->busy)) {
        dev_err(dev, "Failed to get busy GPIO\n");
        return ERR_CAST(sdev->busy);
    }
    gpiod_direction_input(sdev->busy);

    /* DIO1 GPIO (optional) */
    sdev->dio1 = devm_gpiod_get_optional(dev, "dio1", GPIOD_ASIS);
    if (IS_ERR(sdev->dio1)) {
        dev_err(dev, "Failed to get dio1 GPIO\n");
        return ERR_CAST(sdev->dio1);
    }
    if (sdev->dio1)
        gpiod_direction_input(sdev->dio1);

    /* ANT_SW GPIOs (optional) */
    sdev->ant_sw0 = devm_gpiod_get_optional(dev, "ant-sw0", GPIOD_ASIS);
    if (IS_ERR(sdev->ant_sw0))
        return ERR_CAST(sdev->ant_sw0);
    if (sdev->ant_sw0)
        gpiod_direction_output(sdev->ant_sw0, 0);

    sdev->ant_sw1 = devm_gpiod_get_optional(dev, "ant-sw1", GPIOD_ASIS);
    if (IS_ERR(sdev->ant_sw1))
        return ERR_CAST(sdev->ant_sw1);
    if (sdev->ant_sw1)
        gpiod_direction_output(sdev->ant_sw1, 0);

    /* Request IRQ for DIO1 */
    sdev->irq = gpiod_to_irq(sdev->dio1);
    if (sdev->irq < 0) {
        dev_warn(dev, "No IRQ for DIO1, using polling\n");
    } else {
        ret = devm_request_irq(dev, sdev->irq, sx1262_dio1_irq,
                                IRQF_TRIGGER_RISING, "sx1262-dio1", sdev);
        if (ret) {
            dev_err(dev, "Failed to request DIO1 IRQ: %d\n", ret);
            return ERR_PTR(ret);
        }
        dev_info(dev, "DIO1 IRQ %d registered\n", sdev->irq);
    }

    /* HW reset */
    ret = sx1262_reset(sdev);
    if (ret) {
        dev_err(dev, "HW reset failed\n");
        return ERR_PTR(ret);
    }

    dev_info(dev, "SX1262-%d probed successfully\n", id);
    return sdev;
}

/* Remove */
void sx1262_remove(struct sx1262_device *sdev)
{
    dev_info(&sdev->spi->dev, "SX1262-%d removed\n", sdev->device_id);
}
