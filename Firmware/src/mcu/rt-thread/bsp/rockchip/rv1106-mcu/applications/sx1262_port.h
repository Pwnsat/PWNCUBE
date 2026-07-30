/*
 * CubeSat — RT-Thread port layer for the shared SX1262 command core.
 *
 * sx1262_cmd.c (src/sx1262-kmod/, compiled UNCHANGED into this firmware — see
 * docs/migration/design/40-migration-design.md §4) includes this header instead
 * of the Linux ones when built without __KERNEL__. It provides:
 *   - the tiny subset of Linux types/macros cmd.c uses (spi_transfer, delays,
 *     div_u64, dev_* logging),
 *   - the RT-Thread flavor of struct sx1262_device (HAL SPI handle + pins),
 *   - the public config structs/enums mirrored from src/sx1262-kmod/sx1262.h
 *     (KEEP IN SYNC — same field order/types, they cross the IPC boundary),
 *   - prototypes of the portable command API (implemented by sx1262_cmd.c) and
 *     of the HAL glue (implemented by sx1262_port_rtt.c).
 */
#ifndef SX1262_PORT_H
#define SX1262_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <rtthread.h>
#include "hal_base.h"

/* ---- Linux-isms used by sx1262_cmd.c ------------------------------------ */

struct spi_transfer {
    const void *tx_buf;
    void       *rx_buf;
    uint32_t    len;
};

#define msleep(ms)            rt_thread_mdelay(ms)
#define udelay(us)            HAL_DelayUs(us)
#define usleep_range(lo, hi)  HAL_DelayUs(lo)
#define div_u64(a, b)         ((uint64_t)(a) / (uint32_t)(b))

/* jiffies-style timekeeping on RT-Thread ticks */
#define jiffies                 ((unsigned long)rt_tick_get())
#define msecs_to_jiffies(ms)    ((unsigned long)rt_tick_from_millisecond(ms))
#define time_after(a, b)        ((long)((b) - (a)) < 0)

/* Linux IRQ bookkeeping used by set_tx/set_rx. On the MCU the DIO1 line is
 * polled (sx1262_poll_irq), so the completion is a no-op and irq_flags is a
 * plain int. */
#define reinit_completion(c)    do { (void)(c); } while (0)
#define atomic_set(p, v)        (*(volatile int *)(p) = (v))

/* Console is disabled on the MCU (Linux owns the UART); logging is dropped.
 * The arguments are swallowed by the preprocessor, never evaluated. */
#define dev_info(d, ...)      do { } while (0)
#define dev_err(d, ...)       do { } while (0)
#define dev_dbg(d, ...)       do { } while (0)
#define dev_warn(d, ...)      do { } while (0)

/* ---- Device context (RT-Thread flavor) ---------------------------------- */

/* GPIO pin reference: HAL bank instance + pin mask. */
struct sx1262_pin {
    struct GPIO_REG *bank;
    uint32_t         mask;      /* GPIO_PIN_xy */
    bool             active_low;
};

struct sx1262_device {
    struct SPI_HANDLE *spi;     /* HAL SPI handle (PIO mode) */
    char               cs;      /* chip-select index on that controller */

    struct sx1262_pin  reset;   /* out */
    struct sx1262_pin  busy;    /* in  */
    struct sx1262_pin  dio1;    /* in  */
    struct sx1262_pin  ant_sw0; /* out */
    struct sx1262_pin  ant_sw1; /* out */

    uint8_t  ant_sw_mode;
    uint8_t  packet_type;
    uint8_t  device_id;
    bool     tx_in_progress;
    bool     rx_in_progress;

    /* Linux-compat fields referenced by the shared cmd core (polled here). */
    int      dio1_completion;
    int      irq_flags;
};

/* ---- Public config structs/enums (MIRROR of src/sx1262-kmod/sx1262.h) --- */

enum sx1262_antsw_mode {
    SX1262_ANTSW_AUTO = 0,
    SX1262_ANTSW_TX   = 1,
    SX1262_ANTSW_RX   = 2,
};

struct sx1262_modem_config {
    uint8_t  sf;        /* 5-12 */
    uint32_t bw;        /* kHz*10 code, see sx1262.h */
    uint8_t  cr;        /* 4/5=1 .. 4/8=4 */
    uint16_t preamble;  /* 6-65535 */
};

struct sx1262_packet_config {
    uint8_t header_type; /* 0=variable, 1=fixed */
    uint8_t payload_len;
    uint8_t crc_type;    /* 0=off, 1=byte, 2=CCIT */
    uint8_t iq_inverted; /* 0=standard, 1=inverted */
};

struct sx1262_reg_access {
    uint16_t addr;
    uint8_t  val;
};

/* ---- HAL glue (sx1262_port_rtt.c) ---------------------------------------- */

int  sx1262_spi_write(struct sx1262_device *dev, const uint8_t *data, size_t len);
int  sx1262_spi_write_then_read(struct sx1262_device *dev,
                                const uint8_t *tx, size_t tx_len,
                                uint8_t *rx, size_t rx_len);
int  sx1262_spi_transfer(struct sx1262_device *dev,
                         struct spi_transfer *xfers, unsigned int num);
int  sx1262_wait_busy(struct sx1262_device *dev, unsigned long timeout_ms);
int  sx1262_reset(struct sx1262_device *dev);
void sx1262_set_antsw(struct sx1262_device *dev, int mode);
int  sx1262_dio1_is_high(struct sx1262_device *dev);

/* Board bring-up: clocks + iomux + pins + HAL SPI init for radio N (0/1).
 * Returns 0 on success. */
int  sx1262_port_init(struct sx1262_device *dev, int instance);

/* ---- Portable command API (sx1262_cmd.c, exact signatures) --------------- */

int sx1262_read_register(struct sx1262_device *dev, uint16_t addr, uint8_t *val);
int sx1262_write_register(struct sx1262_device *dev, uint16_t addr, uint8_t val);
int sx1262_read_registers(struct sx1262_device *dev, uint16_t addr, uint8_t *buf, size_t len);
int sx1262_write_registers(struct sx1262_device *dev, uint16_t addr, const uint8_t *buf, size_t len);
int sx1262_read_buffer(struct sx1262_device *dev, uint8_t offset, uint8_t *buf, size_t len);
int sx1262_write_buffer(struct sx1262_device *dev, uint8_t offset, const uint8_t *buf, size_t len);
int sx1262_set_sleep(struct sx1262_device *dev, uint8_t sleep_cfg);
int sx1262_set_regulator_mode(struct sx1262_device *dev, uint8_t mode);
int sx1262_set_standby(struct sx1262_device *dev, uint8_t standby_cfg);
int sx1262_set_packet_type(struct sx1262_device *dev, uint8_t pkt_type);
int sx1262_get_packet_type(struct sx1262_device *dev, uint8_t *pkt_type);
int sx1262_set_frequency(struct sx1262_device *dev, uint32_t freq_hz);
int sx1262_set_buffer_base_address(struct sx1262_device *dev, uint8_t tx_base, uint8_t rx_base);
int sx1262_set_rx_tx_fallback_mode(struct sx1262_device *dev, uint8_t mode);
int sx1262_set_pa_config(struct sx1262_device *dev);
int sx1262_set_tx_params(struct sx1262_device *dev, int8_t power_dbm, uint8_t ramp_time);
int sx1262_set_output_power(struct sx1262_device *dev, int8_t power);
int sx1262_set_modulation_params(struct sx1262_device *dev, uint8_t sf, uint32_t bw, uint8_t cr, bool ldro);
int sx1262_set_packet_params(struct sx1262_device *dev, uint16_t preamble_len,
                             uint8_t header_type, uint8_t payload_len,
                             uint8_t crc_type, uint8_t invert_iq);
int sx1262_set_sync_word(struct sx1262_device *dev, uint16_t sync);
int sx1262_set_dio_irq_params(struct sx1262_device *dev, uint16_t irq_mask);
int sx1262_get_rx_buffer_status(struct sx1262_device *dev, uint8_t *payload_len, uint8_t *rx_start);
int sx1262_clear_irq_status(struct sx1262_device *dev, uint16_t irq_mask);
int sx1262_get_irq_status(struct sx1262_device *dev, uint16_t *irq_status);
int sx1262_poll_irq(struct sx1262_device *dev, uint16_t flag, unsigned long timeout_ms);
int sx1262_get_dev_errors(struct sx1262_device *dev, uint16_t *errors);
int sx1262_clear_dev_errors(struct sx1262_device *dev);
int sx1262_get_status(struct sx1262_device *dev, uint8_t *status);
int sx1262_get_rssi_inst(struct sx1262_device *dev, int16_t *rssi);
int sx1262_calibrate(struct sx1262_device *dev, uint8_t calib_params);
int sx1262_calibrate_image(struct sx1262_device *dev, uint8_t freq_band_start, uint8_t freq_band_end);
int sx1262_calibrate_image_for_freq(struct sx1262_device *dev, uint32_t freq_hz);
int sx1262_set_tx(struct sx1262_device *dev, uint32_t timeout_ms);
int sx1262_set_tx_continuous_wave(struct sx1262_device *dev);
int sx1262_set_rx(struct sx1262_device *dev, uint32_t timeout_ms);
int sx1262_init(struct sx1262_device *dev, uint32_t freq_hz);
int sx1262_get_packet_status(struct sx1262_device *dev, int16_t *rssi_pkt, int8_t *snr);

#endif /* SX1262_PORT_H */
