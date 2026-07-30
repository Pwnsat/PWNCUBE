/*
 * CubeSat — I2C port layer for the MCU sensor drivers (BME280 + ICM-42670).
 *
 * Mirrors the SX1262 pattern (docs/migration/implementation/90-mcu-config-replication.md §6/§7): the RV1106 MCU
 * BSP never wired RT_USING_I2C0/drv_i2c, so we drive HAL_I2C directly in POLL
 * mode with the ready-made g_i2c0Dev clock/pin facts, no RT-Thread I2C
 * framework. Both sensors hang off I2C0 (0xFF310000); Linux cedes the bus in
 * the device tree (dts/rv1106-sdk-ipc.dtsi: &i2c0 status="disabled").
 *
 * Bus wiring (from the Linux DT): i2c0m0 — SCL=GPIO1_A3, SDA=GPIO1_A4, func2.
 * BME280 @0x76, ICM-42670 @0x68 (AD0 held low by a &gpio0 hog).
 */
#ifndef SENSOR_PORT_H
#define SENSOR_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <rtthread.h>
#include "hal_base.h"

/* Bring up I2C0 once (clock ungate + iomux + HAL_I2C_Init @400 kHz). Idempotent.
 * Returns 0 on success, <0 on error. */
int sensor_i2c_init(void);

/* Register read: write reg addr, repeated-start, read len bytes into buf.
 * slave is the 7-bit address (e.g. 0x76). Returns 0 or <0. */
int sensor_i2c_read(uint8_t slave, uint8_t reg, uint8_t *buf, uint16_t len);

/* Register write: write reg addr followed by one value byte. Returns 0 or <0. */
int sensor_i2c_write(uint8_t slave, uint8_t reg, uint8_t val);

#endif /* SENSOR_PORT_H */
