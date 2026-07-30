/*
 * CubeSat — BME280 driver (I2C0, forced mode) for the RV1106 MCU.
 *
 * Compensation follows the Bosch BME280 datasheet §4.2.3 fixed-point reference
 * (32-bit T/H, 64-bit P). Calibration words are read from 0x88..0xA1 and
 * 0xE1..0xE7. Forced mode: set ctrl_hum + ctrl_meas(mode=forced) per read, wait
 * for the conversion, then read the raw 20-bit T/P and 16-bit H.
 */
#include "bme280.h"
#include "sensor_port.h"

#define REG_CALIB00   0x88   /* 0x88..0xA1: T1..T3, P1..P9, (skip), H1 */
#define REG_ID        0xD0
#define REG_RESET     0xE0
#define REG_CALIB26   0xE1   /* 0xE1..0xE7: H2..H6 */
#define REG_CTRL_HUM  0xF2
#define REG_STATUS    0xF3
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG    0xF5
#define REG_PRESS_MSB 0xF7   /* 0xF7..0xFE: press(3) temp(3) hum(2) */

#define RESET_WORD    0xB6
#define OSRS_X1       0x01

/* Calibration parameters (Bosch naming). */
static uint16_t dig_T1;
static int16_t  dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
static uint8_t  dig_H1, dig_H3;
static int16_t  dig_H2, dig_H4, dig_H5;
static int8_t   dig_H6;

static int32_t  t_fine;    /* carries T context into P and H compensation */
static bool     s_calibrated;

static inline uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int16_t  s16le(const uint8_t *p) { return (int16_t)u16le(p); }

int bme280_read_id(uint8_t *id)
{
    return sensor_i2c_read(BME280_I2C_ADDR, REG_ID, id, 1);
}

static int bme280_read_calib(void)
{
    uint8_t c[26];   /* 0x88..0xA1 = 26 bytes */
    uint8_t h[7];    /* 0xE1..0xE7 = 7 bytes  */
    int ret;

    ret = sensor_i2c_read(BME280_I2C_ADDR, REG_CALIB00, c, sizeof(c));
    if (ret) return ret;
    ret = sensor_i2c_read(BME280_I2C_ADDR, REG_CALIB26, h, sizeof(h));
    if (ret) return ret;

    dig_T1 = u16le(&c[0]);
    dig_T2 = s16le(&c[2]);
    dig_T3 = s16le(&c[4]);
    dig_P1 = u16le(&c[6]);
    dig_P2 = s16le(&c[8]);
    dig_P3 = s16le(&c[10]);
    dig_P4 = s16le(&c[12]);
    dig_P5 = s16le(&c[14]);
    dig_P6 = s16le(&c[16]);
    dig_P7 = s16le(&c[18]);
    dig_P8 = s16le(&c[20]);
    dig_P9 = s16le(&c[22]);
    /* c[24] = 0xA0 reserved; c[25] = 0xA1 = H1 */
    dig_H1 = c[25];

    dig_H2 = s16le(&h[0]);                                  /* 0xE1/0xE2 */
    dig_H3 = h[2];                                          /* 0xE3      */
    dig_H4 = (int16_t)(((int8_t)h[3] << 4) | (h[4] & 0x0F));/* 0xE4[11:4],0xE5[3:0] */
    dig_H5 = (int16_t)(((int8_t)h[5] << 4) | (h[4] >> 4));  /* 0xE6[11:4],0xE5[7:4] */
    dig_H6 = (int8_t)h[6];                                  /* 0xE7      */

    s_calibrated = true;
    return 0;
}

int bme280_init(void)
{
    int ret;

    ret = sensor_i2c_write(BME280_I2C_ADDR, REG_RESET, RESET_WORD);
    if (ret) return ret;
    rt_thread_mdelay(5);   /* startup time after reset */

    ret = bme280_read_calib();
    if (ret) return ret;

    /* Humidity oversampling must be written before ctrl_meas takes effect. */
    ret = sensor_i2c_write(BME280_I2C_ADDR, REG_CTRL_HUM, OSRS_X1);
    if (ret) return ret;
    /* IIR filter off, standby irrelevant in forced mode. */
    ret = sensor_i2c_write(BME280_I2C_ADDR, REG_CONFIG, 0x00);
    if (ret) return ret;

    return 0;
}

/* Bosch datasheet reference compensation (fixed point). */
static int32_t compensate_T(int32_t adc_T)
{
    int32_t var1, var2;

    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) *
              ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
            ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    return (t_fine * 5 + 128) >> 8;   /* 0.01 °C */
}

static uint32_t compensate_P(int32_t adc_P)
{
    int64_t var1, var2, p;

    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) +
           ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0)
        return 0;   /* avoid div by zero */
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    return (uint32_t)(p >> 8);   /* Q24.8 Pa -> integer Pa */
}

static uint32_t compensate_H(int32_t adc_H)
{
    int32_t v;

    v = (t_fine - ((int32_t)76800));
    v = (((((adc_H << 14) - (((int32_t)dig_H4) << 20) - (((int32_t)dig_H5) * v)) +
           ((int32_t)16384)) >> 15) *
         (((((((v * ((int32_t)dig_H6)) >> 10) *
              (((v * ((int32_t)dig_H3)) >> 11) + ((int32_t)32768))) >> 10) +
            ((int32_t)2097152)) * ((int32_t)dig_H2) + 8192) >> 14));
    v = (v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)dig_H1)) >> 4));
    v = (v < 0 ? 0 : v);
    v = (v > 419430400 ? 419430400 : v);
    return (uint32_t)(v >> 12);   /* Q22.10 %RH */
}

int bme280_read(struct bme280_sample *out)
{
    uint8_t d[8];
    int32_t adc_T, adc_P, adc_H;
    int ret, i;

    if (out == NULL)
        return -1;
    if (!s_calibrated) {
        ret = bme280_init();
        if (ret) return ret;
    }

    /* Trigger one forced conversion (osrs_t=x1, osrs_p=x1, mode=forced=0b01). */
    ret = sensor_i2c_write(BME280_I2C_ADDR, REG_CTRL_HUM, OSRS_X1);
    if (ret) return ret;
    ret = sensor_i2c_write(BME280_I2C_ADDR, REG_CTRL_MEAS,
                           (OSRS_X1 << 5) | (OSRS_X1 << 2) | 0x01);
    if (ret) return ret;

    /* Wait for the measurement to finish (status bit3 = measuring). */
    for (i = 0; i < 20; i++) {
        uint8_t st = 0;
        rt_thread_mdelay(2);
        if (sensor_i2c_read(BME280_I2C_ADDR, REG_STATUS, &st, 1) == 0 &&
            !(st & 0x08))
            break;
    }

    ret = sensor_i2c_read(BME280_I2C_ADDR, REG_PRESS_MSB, d, sizeof(d));
    if (ret) return ret;

    adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
    adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
    adc_H = ((int32_t)d[6] << 8) | d[7];

    out->temp_mC   = compensate_T(adc_T) * 10;   /* 0.01°C -> m°C */
    out->press_Pa  = compensate_P(adc_P);
    out->hum_m_pct = (compensate_H(adc_H) * 1000U) >> 10; /* Q22.10 -> m%RH */
    return 0;
}
