/*
 * CubeSat — HAL_I2C glue for the MCU sensor drivers (I2C0, POLL mode).
 *
 * Recipe distilled from the reference drv_i2c.c (common/drivers/drv_i2c.c):
 *   register read  -> REG_CON_MOD_REGISTER_TX with MRXADDR=slave, MRXRADDR=reg,
 *                     then a plain-RX phase for the data bytes;
 *   register write -> REG_CON_MOD_TX with buf=[reg,val] (the HAL prepends the
 *                     slave address as the first TX byte).
 * The transfer is pumped synchronously: HAL_I2C_Transfer(...,I2C_POLL,...) then
 * spin on HAL_I2C_IRQHandler() (which reads IPD and advances the state machine)
 * until it returns something other than HAL_BUSY. No interrupts are wired.
 *
 * Safety notes (learned from the SX1262 SPI bring-up, docs/migration/implementation/90-mcu-config-replication.md):
 *   - Touching I2C0 registers before ungating its clock bus-stalls the SCR1 with
 *     no trace, so ungate PCLK/CLK_I2C0 (PERICRU) BEFORE HAL_I2C_Init.
 *   - Unlike SPI, the HAL_I2C poll path is bounded by a timeout, so a wiring
 *     fault fails the transfer instead of freezing the poll thread.
 */
#include "sensor_port.h"

#define I2C_XFER_TMO   2000U     /* ~2000 * ~10us spins ≈ 20 ms budget per xfer */

static struct I2C_HANDLE s_i2c0;
static bool s_i2c0_ready;

/* Drive one already-configured transfer to completion in poll mode. */
static int i2c_pump(void)
{
    HAL_Status err = HAL_BUSY;
    uint32_t tmo = I2C_XFER_TMO;

    HAL_I2C_Transfer(&s_i2c0, I2C_POLL, true);
    do {
        HAL_DelayUs(10);
        err = HAL_I2C_IRQHandler(&s_i2c0);
        if (err != HAL_BUSY)
            break;
    } while (--tmo);

    if (tmo == 0U || err != HAL_OK) {
        HAL_I2C_ForceStop(&s_i2c0);
        HAL_I2C_Close(&s_i2c0);
        return -1;
    }
    HAL_I2C_Close(&s_i2c0);
    return 0;
}

int sensor_i2c_read(uint8_t slave, uint8_t reg, uint8_t *buf, uint16_t len)
{
    uint32_t addr, raddr;

    if (!s_i2c0_ready || buf == NULL || len == 0U)
        return -1;

    addr  = ((uint32_t)(slave & 0x7F) << 1) | HAL_I2C_REG_MRXADDR_VALID(0);
    raddr = (uint32_t)reg | HAL_I2C_REG_MRXADDR_VALID(0);

    HAL_I2C_ConfigureMode(&s_i2c0, REG_CON_MOD_REGISTER_TX, addr, raddr);
    HAL_I2C_SetupMsg(&s_i2c0, slave, buf, len, HAL_I2C_M_RD);

    return i2c_pump();
}

int sensor_i2c_write(uint8_t slave, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };

    if (!s_i2c0_ready)
        return -1;

    /* Plain TX: the HAL prepends (slave<<1) as the first byte on the wire. */
    HAL_I2C_ConfigureMode(&s_i2c0, REG_CON_MOD_TX, 0, 0);
    HAL_I2C_SetupMsg(&s_i2c0, slave, b, sizeof(b), 0 /* write */);

    return i2c_pump();
}

int sensor_i2c_init(void)
{
    uint32_t rate;

    if (s_i2c0_ready)
        return 0;

    /* I2C0 lives in PERICRU: ungate pclk + functional clk before any reg touch. */
    HAL_CRU_ClkEnable(PCLK_I2C0_GATE);
    HAL_CRU_ClkEnable(CLK_I2C0_GATE);

    /* i2c0m0 pinmux: SCL=GPIO1_A3, SDA=GPIO1_A4, func2 (board has external
     * pull-ups; DT uses pull-none, so no internal pulls set). */
    HAL_PINCTRL_SetIOMUX(GPIO_BANK1, GPIO_PIN_A3 | GPIO_PIN_A4,
                         PIN_CONFIG_MUX_FUNC2);

    rate = HAL_CRU_ClkGetFreq(CLK_I2C0);
    if (rate == 0U)
        rate = 200000000U;   /* CLK_I2C0 default mux = 200 MHz matrix clock */

    if (HAL_I2C_Init(&s_i2c0, I2C0, rate, I2C_400K) != HAL_OK)
        return -1;

    s_i2c0_ready = true;
    return 0;
}
