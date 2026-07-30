// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal IIO driver for the TDK InvenSense ICM-42670-P 6-axis IMU (I2C).
 *
 * Exposes accelerometer (x/y/z), gyroscope (x/y/z) and temperature as IIO
 * channels read on demand from the chip's user-bank-0 data registers. No FIFO
 * or triggered-buffer support — values are polled via sysfs:
 *   /sys/bus/iio/devices/iio:deviceN/in_accel_{x,y,z}_raw   (* in_accel_scale)
 *   /sys/bus/iio/devices/iio:deviceN/in_anglvel_{x,y,z}_raw (* in_anglvel_scale)
 *   /sys/bus/iio/devices/iio:deviceN/in_temp_raw            ((raw+offset)*scale)
 *
 * The ICM-42670-P register map differs from the ICM-426xx family handled by the
 * in-tree inv_icm42600 driver, hence this dedicated module.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>

/* User bank 0 registers (directly accessible, no MREG indirection) */
#define ICM42670_REG_SIGNAL_PATH_RESET	0x02
#define ICM42670_SOFT_RESET		BIT(4)
#define ICM42670_REG_TEMP_DATA1		0x09	/* big-endian 16-bit */
#define ICM42670_REG_ACCEL_DATA_X1	0x0b	/* 6x big-endian 16-bit */
#define ICM42670_REG_GYRO_DATA_X1	0x11	/* 6x big-endian 16-bit */
#define ICM42670_REG_PWR_MGMT0		0x1f
#define ICM42670_GYRO_MODE_LN		(0x3 << 2)
#define ICM42670_ACCEL_MODE_LN		(0x3 << 0)
#define ICM42670_REG_GYRO_CONFIG0	0x20
#define ICM42670_REG_ACCEL_CONFIG0	0x21
/* FS in bits[6:5], ODR in bits[3:0]; 0x09 = 100 Hz, FS=00 (max range) */
#define ICM42670_CONFIG0_FS_MAX_100HZ	0x09
#define ICM42670_REG_WHO_AM_I		0x75
#define ICM42670_WHO_AM_I_VAL		0x67

struct icm42670_data {
	struct regmap *regmap;
	struct mutex lock;	/* serialize multi-byte reads/config */
};

static const struct regmap_config icm42670_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x7f,
};

#define ICM42670_ACCEL_CHAN(_axis, _mod) {				\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_mod,					\
	.address = ICM42670_REG_ACCEL_DATA_X1 + (_axis) * 2,		\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

#define ICM42670_GYRO_CHAN(_axis, _mod) {				\
	.type = IIO_ANGL_VEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_mod,					\
	.address = ICM42670_REG_GYRO_DATA_X1 + (_axis) * 2,		\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec icm42670_channels[] = {
	ICM42670_ACCEL_CHAN(0, X),
	ICM42670_ACCEL_CHAN(1, Y),
	ICM42670_ACCEL_CHAN(2, Z),
	ICM42670_GYRO_CHAN(0, X),
	ICM42670_GYRO_CHAN(1, Y),
	ICM42670_GYRO_CHAN(2, Z),
	{
		.type = IIO_TEMP,
		.address = ICM42670_REG_TEMP_DATA1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
	},
};

/* Read one big-endian signed 16-bit sample from register pair at @reg. */
static int icm42670_read_sample(struct icm42670_data *data, u8 reg, int *val)
{
	__be16 raw;
	int ret;

	mutex_lock(&data->lock);
	ret = regmap_bulk_read(data->regmap, reg, &raw, sizeof(raw));
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	*val = (s16)be16_to_cpu(raw);
	return 0;
}

static int icm42670_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct icm42670_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = icm42670_read_sample(data, chan->address, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			/* +/-16g FS: 16 * 9.80665 / 32768 m/s^2 per LSB */
			*val = 0;
			*val2 = 4788400;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_ANGL_VEL:
			/* +/-2000 dps FS: 2000 * (pi/180) / 32768 rad/s per LSB */
			*val = 0;
			*val2 = 1065264;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_TEMP:
			/* degC = raw/128 + 25 -> milli-degC scale = 1000/128 */
			*val = 1000;
			*val2 = 128;
			return IIO_VAL_FRACTIONAL;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		/* temp only: (raw + 3200) * (1000/128) = milli-degC */
		*val = 3200;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_info icm42670_info = {
	.read_raw = icm42670_read_raw,
};

static int icm42670_power_on(struct icm42670_data *data)
{
	struct device *dev = regmap_get_device(data->regmap);
	unsigned int who;
	int ret;

	/* Soft reset, then wait for the device to come back up. */
	ret = regmap_write(data->regmap, ICM42670_REG_SIGNAL_PATH_RESET,
			   ICM42670_SOFT_RESET);
	if (ret)
		return ret;
	usleep_range(2000, 3000);

	ret = regmap_read(data->regmap, ICM42670_REG_WHO_AM_I, &who);
	if (ret)
		return ret;
	if (who != ICM42670_WHO_AM_I_VAL) {
		dev_err(dev, "unexpected WHO_AM_I 0x%02x (want 0x%02x)\n",
			who, ICM42670_WHO_AM_I_VAL);
		return -ENODEV;
	}

	/* Max full-scale range at 100 Hz for both accel and gyro. */
	ret = regmap_write(data->regmap, ICM42670_REG_ACCEL_CONFIG0,
			   ICM42670_CONFIG0_FS_MAX_100HZ);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, ICM42670_REG_GYRO_CONFIG0,
			   ICM42670_CONFIG0_FS_MAX_100HZ);
	if (ret)
		return ret;

	/* Turn accel + gyro on in low-noise mode. */
	ret = regmap_write(data->regmap, ICM42670_REG_PWR_MGMT0,
			   ICM42670_GYRO_MODE_LN | ICM42670_ACCEL_MODE_LN);
	if (ret)
		return ret;
	/* Gyro needs up to ~45 ms to start; give both time to settle. */
	msleep(50);

	return 0;
}

static int icm42670_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct icm42670_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	mutex_init(&data->lock);

	data->regmap = devm_regmap_init_i2c(client, &icm42670_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	i2c_set_clientdata(client, indio_dev);

	ret = icm42670_power_on(data);
	if (ret)
		return ret;

	indio_dev->name = "icm42670";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &icm42670_info;
	indio_dev->channels = icm42670_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm42670_channels);

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int icm42670_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct icm42670_data *data = iio_priv(indio_dev);

	/* Power accel + gyro back off. */
	regmap_write(data->regmap, ICM42670_REG_PWR_MGMT0, 0x00);
	return 0;
}

static const struct i2c_device_id icm42670_id[] = {
	{ "icm42670", 0 },
	{ "icm42670p", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, icm42670_id);

static const struct of_device_id icm42670_of_match[] = {
	{ .compatible = "invensense,icm42670p" },
	{ .compatible = "invensense,icm42670" },
	{ }
};
MODULE_DEVICE_TABLE(of, icm42670_of_match);

static struct i2c_driver icm42670_driver = {
	.driver = {
		.name = "icm42670",
		.of_match_table = icm42670_of_match,
	},
	.probe = icm42670_probe,
	.remove = icm42670_remove,
	.id_table = icm42670_id,
};
module_i2c_driver(icm42670_driver);

MODULE_AUTHOR("PWNCube SDK");
MODULE_DESCRIPTION("TDK InvenSense ICM-42670-P 6-axis IMU (I2C, minimal IIO)");
MODULE_LICENSE("GPL");
