/*
 * CubeSat — ICM-42670-P 6-axis IMU driver for the MCU, over I2C0.
 * Ported from the repo's custom Linux driver src/icm42670-kmod/icm42670.c
 * (bank0 direct registers, big-endian 16-bit sample words).
 */
#ifndef ICM42670_H
#define ICM42670_H

#include <stdint.h>

#define ICM42670_I2C_ADDR   0x68   /* AD0 held low by a &gpio0 hog */
#define ICM42670_REG_WHOAMI 0x75
#define ICM42670_WHOAMI_VAL 0x67

/* One raw sample set (signed 16-bit LSB counts; scale applied by the host). */
struct icm42670_sample {
    int16_t accel[3];   /* X, Y, Z  (±16 g full scale)   */
    int16_t gyro[3];    /* X, Y, Z  (±2000 dps full scale)*/
    int16_t temp;       /* raw temperature (°C ≈ raw/128 + 25) */
};

/* Read WHO_AM_I (expect 0x67). 0 on success. */
int icm42670_read_id(uint8_t *id);

/* Soft reset, verify WHO_AM_I, power on accel+gyro (low-noise). 0 on success. */
int icm42670_init(void);

/* Read accel/gyro/temp. 0 on success. */
int icm42670_read(struct icm42670_sample *out);

#endif /* ICM42670_H */
