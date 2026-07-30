/*
 * CubeSat — BME280 (temp/pressure/humidity) driver for the MCU, over I2C0.
 * Ported from the Bosch BME280 datasheet reference compensation (the Linux side
 * used the in-tree iio/pressure/bmp280 driver, which is regmap/IIO-bound and not
 * shareable, so the register logic is reimplemented here MCU-native).
 */
#ifndef BME280_H
#define BME280_H

#include <stdint.h>
#include <stdbool.h>

#define BME280_I2C_ADDR   0x76
#define BME280_REG_ID     0xD0
#define BME280_CHIP_ID    0x60

/* One compensated sample. */
struct bme280_sample {
    int32_t  temp_mC;      /* temperature, milli-°C            */
    uint32_t press_Pa;     /* pressure, Pa                     */
    uint32_t hum_m_pct;    /* relative humidity, milli-%RH     */
};

/* Read the chip ID register (expect BME280_CHIP_ID 0x60). 0 on success. */
int bme280_read_id(uint8_t *id);

/* Reset, read calibration, configure oversampling. 0 on success. Idempotent-ish
 * (safe to call again to re-read calibration). */
int bme280_init(void);

/* Trigger a forced measurement and return the compensated sample. 0 on ok. */
int bme280_read(struct bme280_sample *out);

#endif /* BME280_H */
