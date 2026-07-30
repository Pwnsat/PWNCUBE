#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include "sx1262.h"
#include "sx1262_regs.h"

/* Forward declarations from other modules */
extern struct sx1262_device *sx1262_probe(struct spi_device *spi, int id);
extern void sx1262_remove(struct sx1262_device *sdev);
extern int sx1262_init(struct sx1262_device *dev, uint32_t freq_hz);
extern void sx1262_set_antsw(struct sx1262_device *dev, int mode);

/* Chardev globals */
static struct class *sx1262_class;
static int sx1262_major;
struct sx1262_device *sx1262_devices[MAX_DEVICES];
static struct device *sx1262_class_devs[MAX_DEVICES];
static int device_count;

/* Forward declarations from chardev */
extern const struct file_operations sx1262_fops;

/* Create chardev for a device */
static int create_chardev(struct sx1262_device *sdev, int id)
{
    dev_t devt;

    if (id >= MAX_DEVICES)
        return -ENOMEM;

    devt = MKDEV(sx1262_major, id);
    sx1262_devices[id] = sdev;

    sx1262_class_devs[id] = device_create(sx1262_class, NULL, devt,
                                           &sdev->spi->dev, "sx1262-%d", id);
    if (IS_ERR(sx1262_class_devs[id]))
        return PTR_ERR(sx1262_class_devs[id]);

    return 0;
}

/* SPI driver probe */
static int sx1262_spi_probe(struct spi_device *spi)
{
    struct sx1262_device *sdev;
    int id = device_count;
    int ret;

    if (id >= MAX_DEVICES) {
        dev_err(&spi->dev, "Max devices reached\n");
        return -ENOMEM;
    }

    /* Setup SPI */
    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    ret = spi_setup(spi);
    if (ret) {
        dev_err(&spi->dev, "SPI setup failed\n");
        return ret;
    }

    /* Probe HAL */
    sdev = sx1262_probe(spi, id);
    if (IS_ERR(sdev))
        return PTR_ERR(sdev);

    /* Initialize the radio */
    ret = sx1262_init(sdev, 868000000);
    if (ret) {
        dev_err(&spi->dev, "Radio init failed\n");
        return ret;
    }

    /* Set default ANT_SW to RX (safe) */
    sx1262_set_antsw(sdev, SX1262_ANTSW_RX);

    /* Create chardev */
    ret = create_chardev(sdev, id);
    if (ret) {
        dev_err(&spi->dev, "Failed to create chardev\n");
        return ret;
    }

    device_count++;
    dev_info(&spi->dev, "SX1262-%d ready\n", id);
    return 0;
}

/* SPI driver remove */
static int sx1262_spi_remove(struct spi_device *spi)
{
    struct sx1262_device *sdev = spi_get_drvdata(spi);
    int id = sdev->device_id;

    if (id < MAX_DEVICES && sx1262_class_devs[id]) {
        device_destroy(sx1262_class, MKDEV(sx1262_major, id));
        sx1262_devices[id] = NULL;
        sx1262_class_devs[id] = NULL;
    }

    sx1262_remove(sdev);
    return 0;
}

static const struct of_device_id sx1262_of_match[] = {
    { .compatible = "semtech,sx1262", },
    {},
};
MODULE_DEVICE_TABLE(of, sx1262_of_match);

static struct spi_driver sx1262_spi_driver = {
    .driver = {
        .name       = "sx1262",
        .of_match_table = sx1262_of_match,
    },
    .probe  = sx1262_spi_probe,
    .remove = sx1262_spi_remove,
};

static int __init sx1262_module_init(void)
{
    dev_t devt;
    int ret;

    /* Allocate chardev region */
    ret = alloc_chrdev_region(&devt, 0, MAX_DEVICES, SX1262_DEV_NAME);
    if (ret < 0) {
        pr_err("sx1262: Failed to alloc chardev region\n");
        return ret;
    }

    sx1262_major = MAJOR(devt);

    /* Create device class */
    sx1262_class = class_create(THIS_MODULE, SX1262_CLASS_NAME);
    if (IS_ERR(sx1262_class)) {
        unregister_chrdev_region(devt, MAX_DEVICES);
        return PTR_ERR(sx1262_class);
    }

    /* Register chardev */
    {
        struct cdev *cdev = cdev_alloc();
        if (!cdev) {
            class_destroy(sx1262_class);
            unregister_chrdev_region(devt, MAX_DEVICES);
            return -ENOMEM;
        }
        cdev_init(cdev, &sx1262_fops);
        cdev->owner = THIS_MODULE;
        ret = cdev_add(cdev, devt, MAX_DEVICES);
        if (ret) {
            cdev_del(cdev);
            class_destroy(sx1262_class);
            unregister_chrdev_region(devt, MAX_DEVICES);
            return ret;
        }
    }

    /* Register SPI driver */
    ret = spi_register_driver(&sx1262_spi_driver);
    if (ret) {
        /* Cleanup chardev */
        class_destroy(sx1262_class);
        unregister_chrdev_region(devt, MAX_DEVICES);
        return ret;
    }

    pr_info("sx1262: Driver loaded, major %d\n", sx1262_major);
    return 0;
}

static void __exit sx1262_module_exit(void)
{
    spi_unregister_driver(&sx1262_spi_driver);
    class_destroy(sx1262_class);
    unregister_chrdev_region(MKDEV(sx1262_major, 0), MAX_DEVICES);
    pr_info("sx1262: Driver unloaded\n");
}

module_init(sx1262_module_init);
module_exit(sx1262_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PwnCube SDK");
MODULE_DESCRIPTION("SX1262 LoRa transceiver driver");
MODULE_VERSION("1.0");
