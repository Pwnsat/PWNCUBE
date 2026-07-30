#ifndef __SX1262_H
#define __SX1262_H

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/completion.h>
#include <linux/mutex.h>

struct spi_device;
struct gpio_desc;

#define SX1262_DEV_NAME "sx1262"
#define SX1262_CLASS_NAME "sx1262"
#define SX1262_MAX_PACKET_LEN 255

#define MAX_DEVICES 2

struct sx1262_device {
    struct spi_device *spi;
    struct gpio_desc *reset;
    struct gpio_desc *busy;
    struct gpio_desc *dio1;
    struct gpio_desc *ant_sw0;
    struct gpio_desc *ant_sw1;
    int irq;
    struct completion dio1_completion;
    wait_queue_head_t read_wq;
    atomic_t irq_flags;
    uint8_t ant_sw_mode;
    struct mutex lock;
    uint8_t packet_type;
    uint8_t device_id;
    bool tx_in_progress;
    bool rx_in_progress;
};

#define SX1262_IOCTL_MAGIC 'L'

#define SX1262_IOCTL_RESET         _IO(SX1262_IOCTL_MAGIC, 0)
#define SX1262_IOCTL_SET_STANDBY   _IO(SX1262_IOCTL_MAGIC, 1)
#define SX1262_IOCTL_SET_SLEEP     _IO(SX1262_IOCTL_MAGIC, 2)
#define SX1262_IOCTL_SET_FREQ      _IOW(SX1262_IOCTL_MAGIC, 3, uint32_t)
#define SX1262_IOCTL_SET_POWER     _IOW(SX1262_IOCTL_MAGIC, 4, int8_t)
#define SX1262_IOCTL_SET_MODEM     _IOW(SX1262_IOCTL_MAGIC, 5, struct sx1262_modem_config)
#define SX1262_IOCTL_SET_TX        _IOW(SX1262_IOCTL_MAGIC, 6, uint32_t)
#define SX1262_IOCTL_SET_RX        _IOW(SX1262_IOCTL_MAGIC, 7, uint32_t)
#define SX1262_IOCTL_GET_STATUS    _IOR(SX1262_IOCTL_MAGIC, 8, uint8_t)
#define SX1262_IOCTL_GET_RSSI      _IOR(SX1262_IOCTL_MAGIC, 9, int16_t)
#define SX1262_IOCTL_SET_ANTSW     _IOW(SX1262_IOCTL_MAGIC, 10, uint8_t)
#define SX1262_IOCTL_SET_PACKET     _IOW(SX1262_IOCTL_MAGIC, 11, struct sx1262_packet_config)
#define SX1262_IOCTL_READ_REG       _IOWR(SX1262_IOCTL_MAGIC, 12, struct sx1262_reg_access)
#define SX1262_IOCTL_WRITE_REG      _IOW(SX1262_IOCTL_MAGIC, 13, struct sx1262_reg_access)
#define SX1262_IOCTL_GET_IRQ_STATUS _IOR(SX1262_IOCTL_MAGIC, 14, uint16_t)
#define SX1262_IOCTL_SET_CW         _IO(SX1262_IOCTL_MAGIC, 15)

struct sx1262_reg_access {
    uint16_t addr;
    uint8_t val;
};

enum sx1262_antsw_mode {
    SX1262_ANTSW_AUTO = 0,
    SX1262_ANTSW_TX   = 1,
    SX1262_ANTSW_RX   = 2,
};

struct sx1262_modem_config {
    uint8_t sf;        /* 5-12 */
    uint32_t bw;       /* 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500 */
    uint8_t cr;        /* 4/5=1, 4/6=2, 4/7=3, 4/8=4 */
    uint16_t preamble; /* 6-65535 */
};

struct sx1262_packet_config {
    uint8_t header_type; /* 0=variable, 1=fixed */
    uint8_t payload_len;
    uint8_t crc_type;    /* 0=off, 1=byte, 2=CCIT */
    uint8_t iq_inverted; /* 0=standard, 1=inverted */
};

#endif /* __SX1262_H */
