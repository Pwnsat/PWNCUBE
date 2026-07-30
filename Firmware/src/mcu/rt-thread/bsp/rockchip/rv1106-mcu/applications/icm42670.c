/*
 * CubeSat — ICM-42670-P driver (I2C0) for the RV1106 MCU.
 *
 * Register map and power-on sequence mirror the repo's Linux MVP driver
 * (src/icm42670-kmod/icm42670.c): bank0 direct registers, sample words are
 * big-endian 16-bit. ±16 g / ±2000 dps @ 100 Hz, accel+gyro in low-noise mode.
 * No FIFO/IRQ — samples are read on demand.
 */
#include "icm42670.h"
#include "sensor_port.h"

#define REG_SIGNAL_PATH_RESET 0x02
#define SOFT_RESET            (1 << 4)
#define REG_TEMP_DATA1        0x09   /* BE16 */
#define REG_ACCEL_DATA_X1     0x0B   /* 6 x BE16 (X,Y,Z) */
#define REG_GYRO_DATA_X1      0x11   /* 6 x BE16 (X,Y,Z) */
#define REG_PWR_MGMT0         0x1F
#define GYRO_MODE_LN          (0x3 << 2)
#define ACCEL_MODE_LN         (0x3 << 0)
#define REG_GYRO_CONFIG0      0x20
#define REG_ACCEL_CONFIG0     0x21
#define CONFIG0_FS_MAX_100HZ  0x09   /* max full scale, 100 Hz ODR */
#define REG_WHO_AM_I          0x75

static bool s_inited;

static inline int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

int icm42670_read_id(uint8_t *id)
{
    return sensor_i2c_read(ICM42670_I2C_ADDR, REG_WHO_AM_I, id, 1);
}

int icm42670_init(void)
{
    uint8_t who = 0;
    int ret;

    ret = sensor_i2c_write(ICM42670_I2C_ADDR, REG_SIGNAL_PATH_RESET, SOFT_RESET);
    if (ret) return ret;
    rt_thread_mdelay(3);   /* datasheet: ~1 ms; be generous */

    ret = icm42670_read_id(&who);
    if (ret) return ret;
    if (who != ICM42670_WHOAMI_VAL)
        return -1;

    ret = sensor_i2c_write(ICM42670_I2C_ADDR, REG_ACCEL_CONFIG0, CONFIG0_FS_MAX_100HZ);
    if (ret) return ret;
    ret = sensor_i2c_write(ICM42670_I2C_ADDR, REG_GYRO_CONFIG0, CONFIG0_FS_MAX_100HZ);
    if (ret) return ret;

    ret = sensor_i2c_write(ICM42670_I2C_ADDR, REG_PWR_MGMT0,
                           GYRO_MODE_LN | ACCEL_MODE_LN);
    if (ret) return ret;
    rt_thread_mdelay(50);   /* gyro start-up */

    s_inited = true;
    return 0;
}

int icm42670_read(struct icm42670_sample *out)
{
    uint8_t a[6], g[6], t[2];
    int ret;

    if (out == NULL)
        return -1;
    if (!s_inited) {
        ret = icm42670_init();
        if (ret) return ret;
    }

    ret = sensor_i2c_read(ICM42670_I2C_ADDR, REG_ACCEL_DATA_X1, a, sizeof(a));
    if (ret) return ret;
    ret = sensor_i2c_read(ICM42670_I2C_ADDR, REG_GYRO_DATA_X1, g, sizeof(g));
    if (ret) return ret;
    ret = sensor_i2c_read(ICM42670_I2C_ADDR, REG_TEMP_DATA1, t, sizeof(t));
    if (ret) return ret;

    out->accel[0] = be16(&a[0]);
    out->accel[1] = be16(&a[2]);
    out->accel[2] = be16(&a[4]);
    out->gyro[0]  = be16(&g[0]);
    out->gyro[1]  = be16(&g[2]);
    out->gyro[2]  = be16(&g[4]);
    out->temp     = be16(&t[0]);
    return 0;
}
