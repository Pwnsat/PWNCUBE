/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2022 Fuzhou Rockchip Electronics Co., Ltd
 */

#ifndef _HAL_CONF_H_
#define _HAL_CONF_H_

#include "rtconfig.h"

#define HAL_MCU_CORE

#if defined(RT_USING_CACHE)
#define HAL_DCACHE_MODULE_ENABLED
#define HAL_ICACHE_MODULE_ENABLED
#endif

#ifdef RT_USING_CRU
#define HAL_CRU_MODULE_ENABLED
#endif

#ifdef RT_USING_DMA
#define HAL_DMA_MODULE_ENABLED
#endif

#ifdef RT_USING_I2C
#define HAL_I2C_MODULE_ENABLED
#endif

#ifdef RT_USING_MAILBOX
#define HAL_MBOX_MODULE_ENABLED
#endif

#ifdef RT_USING_PIN
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PINCTRL_MODULE_ENABLED
#endif

/* CubeSat: SX1262 radios are driven with the HAL SPI directly (PIO transfers,
 * no drv_spi framework — the RV1106 MCU BSP never wired RT_USING_SPIx). */
#define HAL_SPI_MODULE_ENABLED

#ifdef RT_USING_PWM
#define HAL_PWM_MODULE_ENABLED
#endif

#ifdef RT_USING_SARADC
#define HAL_SARADC_MODULE_ENABLED
#endif

/* CubeSat: NEO-6M GPS (Etapa 5) is driven with HAL_UART directly (poll
 * mode, gps_nmea.c) -- same reasoning as HAL_SPI_MODULE_ENABLED above:
 * RT_USING_UART would also pull common/drivers/drv_uart.c into the build,
 * which needs RT-Thread's serial component (rt_serial_t and friends, not
 * wired into this BSP) just to compile, not just to register a device. */
#define HAL_UART_MODULE_ENABLED

#endif
