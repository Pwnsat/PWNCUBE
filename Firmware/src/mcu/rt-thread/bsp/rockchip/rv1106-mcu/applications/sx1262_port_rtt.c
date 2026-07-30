/*
 * CubeSat — RT-Thread HAL glue for the SX1262 (mirror of Linux sx1262_hal.c).
 *
 * Drives the HAL SPI controller directly in PIO mode (no drv_spi framework:
 * the RV1106 MCU BSP never wired RT_USING_SPIx) with manual chip-select, and
 * the control pins via HAL_GPIO. Two radios are supported; board wiring comes
 * from the Linux DT (dts/rv1106-sdk-ipc.dtsi) — keep this file in sync with it:
 *
 *   Radio 0: SPI0 @0xFF500000, spi0m0 pins GPIO1_C0(cs,f4) C1(clk,f4)
 *            C2(mosi,f6) C3(miso,f6); reset=GPIO3_A6 (act-low),
 *            busy=GPIO3_A7, dio1=GPIO3_A3, ant_sw0=GPIO0_A3, ant_sw1=GPIO0_A4.
 *   Radio 1: SPI1 @0xFF510000, spi1m0 pins GPIO4_A5(cs) A7(clk) A0(miso)
 *            A1(mosi) all func2; reset=GPIO1_C6 (act-low), busy=GPIO1_C7,
 *            dio1=GPIO1_D1, ant_sw0=GPIO1_C5, ant_sw1=GPIO1_D0.
 *
 * SPI0 clocks live in VEPUCRU (PCLK/CLK/SCLK_IN_SPI0_GATE); SPI1 in PERICRU
 * (PCLK/CLK/SCLK_IN_SPI1_GATE). clk_spiN default = 200 MHz matrix clock,
 * divided internally to the 8 MHz the SX1262 wants.
 */
#include "sx1262_port.h"

#define SX_SPI_HZ        (8000000U)
#define SX_SPI_INPUT_HZ  (200000000U)   /* CLK_SPIn_SEL default: 200M matrix */

/* Per-instance board wiring (mirror of the Linux DT). */
struct sx1262_board {
    uint32_t         spi_base;
    uint32_t         clk_gates[3];
    struct sx1262_pin reset, busy, dio1, ant_sw0, ant_sw1;
};

static struct SPI_HANDLE s_spi_bus[2];

static const struct sx1262_board s_board[2] = {
    /* radio 0 — SPI0 */
    {
        .spi_base  = SPI0_BASE,
        .clk_gates = { PCLK_SPI0_GATE, CLK_SPI0_GATE, SCLK_IN_SPI0_GATE },
        .reset   = { GPIO3, GPIO_PIN_A6, true  },
        .busy    = { GPIO3, GPIO_PIN_A7, false },
        .dio1    = { GPIO3, GPIO_PIN_A3, false },
        .ant_sw0 = { GPIO0, GPIO_PIN_A3, false },
        .ant_sw1 = { GPIO0, GPIO_PIN_A4, false },
    },
    /* radio 1 — SPI1 */
    {
        .spi_base  = SPI1_BASE,
        .clk_gates = { PCLK_SPI1_GATE, CLK_SPI1_GATE, SCLK_IN_SPI1_GATE },
        .reset   = { GPIO1, GPIO_PIN_C6, true  },
        .busy    = { GPIO1, GPIO_PIN_C7, false },
        .dio1    = { GPIO1, GPIO_PIN_D1, false },
        .ant_sw0 = { GPIO1, GPIO_PIN_C5, false },
        .ant_sw1 = { GPIO1, GPIO_PIN_D0, false },
    },
};

/* ---- pin helpers ---------------------------------------------------------- */

static void sx_pin_out(const struct sx1262_pin *p, int logic)
{
    /* logic level 1 = "asserted" per DT polarity */
    eGPIO_pinLevel lvl = (p->active_low ? !logic : logic) ? GPIO_HIGH : GPIO_LOW;

    HAL_GPIO_SetPinLevel(p->bank, p->mask, lvl);
}

static int sx_pin_in(const struct sx1262_pin *p)
{
    int v = (HAL_GPIO_GetPinLevel(p->bank, p->mask) == GPIO_HIGH) ? 1 : 0;

    return p->active_low ? !v : v;
}

/* Raw DIO1 level (active-high on this board): high = an enabled IRQ is pending.
 * Used by the RadioService poll loop to detect RX_DONE/TIMEOUT without SPI. */
int sx1262_dio1_is_high(struct sx1262_device *dev)
{
    return sx_pin_in(&dev->dio1);
}

/* ---- SPI primitives -------------------------------------------------------- */

static int sx_spi_xfer(struct sx1262_device *dev, const void *tx, void *rx, uint32_t len)
{
    HAL_Status ret;

    dev->spi->config.xfmMode = (tx && rx) ? CR0_XFM_TR : (tx ? CR0_XFM_TO : CR0_XFM_RO);
    ret = HAL_SPI_Configure(dev->spi, tx, rx, len);
    if (ret != HAL_OK)
        return -1;

    ret = HAL_SPI_PioTransfer(dev->spi);

    /* Wait for the controller to finish shifting out before disabling it.
     * HAL_SPI_PioTransfer returns once the last byte is loaded into the TX
     * FIFO, NOT when it has left the wire; HAL_SPI_Stop disables the controller
     * immediately, so without this the tail byte(s) of a write can be truncated.
     * Mirrors the reference drv_spi.c rockchip_spi_wait_idle(). (RX-only
     * transfers are already drained by PioTransfer, so this returns at once.) */
    {
        rt_tick_t timeout = rt_tick_get() + rt_tick_from_millisecond(10);
        while (HAL_SPI_QueryBusState(dev->spi) != HAL_OK && timeout > rt_tick_get())
            ;
    }

    /* PioTransfer leaves the controller ENABLED; the Rockchip SPI must be
     * disabled before the next Configure() or the following transfer wedges
     * the state machine (frozen poll thread on the 2nd SPI op). Chip-select
     * (SER) is a separate register and stays asserted across this. */
    HAL_SPI_Stop(dev->spi);

    return (ret == HAL_OK) ? 0 : -1;
}

int sx1262_spi_write(struct sx1262_device *dev, const uint8_t *data, size_t len)
{
    int ret;

    HAL_SPI_SetCS(dev->spi, dev->cs, true);
    ret = sx_spi_xfer(dev, data, RT_NULL, len);
    HAL_SPI_SetCS(dev->spi, dev->cs, false);

    return ret;
}

int sx1262_spi_write_then_read(struct sx1262_device *dev,
                               const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_len)
{
    int ret;

    HAL_SPI_SetCS(dev->spi, dev->cs, true);
    ret = sx_spi_xfer(dev, tx, RT_NULL, tx_len);
    if (ret == 0 && rx_len)
        ret = sx_spi_xfer(dev, RT_NULL, rx, rx_len);
    HAL_SPI_SetCS(dev->spi, dev->cs, false);

    return ret;
}

int sx1262_spi_transfer(struct sx1262_device *dev,
                        struct spi_transfer *xfers, unsigned int num)
{
    unsigned int i;
    int ret = 0;

    HAL_SPI_SetCS(dev->spi, dev->cs, true);
    for (i = 0; i < num && ret == 0; i++)
        ret = sx_spi_xfer(dev, xfers[i].tx_buf, xfers[i].rx_buf, xfers[i].len);
    HAL_SPI_SetCS(dev->spi, dev->cs, false);

    return ret;
}

/* ---- control pins ---------------------------------------------------------- */

int sx1262_wait_busy(struct sx1262_device *dev, unsigned long timeout_ms)
{
    unsigned long waited = 0;

    /* BUSY is active-high in DT terms: wait for it to deassert (logic 0) */
    while (sx_pin_in(&dev->busy))
    {
        if (waited >= timeout_ms)
            return -1;
        rt_thread_mdelay(1);
        waited++;
    }

    return 0;
}

int sx1262_reset(struct sx1262_device *dev)
{
    sx_pin_out(&dev->reset, 1);   /* assert NRESET (active low pin -> low) */
    rt_thread_mdelay(2);
    sx_pin_out(&dev->reset, 0);   /* release */
    rt_thread_mdelay(5);

    return sx1262_wait_busy(dev, 100);
}

void sx1262_set_antsw(struct sx1262_device *dev, int mode)
{
    switch (mode)
    {
    case SX1262_ANTSW_TX:
        sx_pin_out(&dev->ant_sw0, 0);
        sx_pin_out(&dev->ant_sw1, 1);
        break;
    case SX1262_ANTSW_RX:
        sx_pin_out(&dev->ant_sw0, 1);
        sx_pin_out(&dev->ant_sw1, 0);
        break;
    default:            /* AUTO/idle: both off */
        sx_pin_out(&dev->ant_sw0, 0);
        sx_pin_out(&dev->ant_sw1, 0);
        break;
    }
}

/* ---- board bring-up --------------------------------------------------------- */

int sx1262_port_init(struct sx1262_device *dev, int instance)
{
    const struct sx1262_board *b;

    if (instance < 0 || instance > 1)
        return -1;
    b = &s_board[instance];

    /* SPI kernel/pclk/sample-in clocks (VEPUCRU for SPI0, PERICRU for SPI1) */
    HAL_CRU_ClkEnable(b->clk_gates[0]);
    HAL_CRU_ClkEnable(b->clk_gates[1]);
    HAL_CRU_ClkEnable(b->clk_gates[2]);

    if (instance == 0)
    {
        /* spi0 m0 iomux: CS0/CLK = func4, MOSI/MISO = func6 */
        HAL_PINCTRL_SetIOMUX(GPIO_BANK1, GPIO_PIN_C0 | GPIO_PIN_C1, PIN_CONFIG_MUX_FUNC4);
        HAL_PINCTRL_SetIOMUX(GPIO_BANK1, GPIO_PIN_C2 | GPIO_PIN_C3, PIN_CONFIG_MUX_FUNC6);
        /* control pins as GPIO (func0) */
        HAL_PINCTRL_SetIOMUX(GPIO_BANK3, GPIO_PIN_A3 | GPIO_PIN_A6 | GPIO_PIN_A7,
                             PIN_CONFIG_MUX_FUNC0);
        HAL_PINCTRL_SetIOMUX(GPIO_BANK0, GPIO_PIN_A3 | GPIO_PIN_A4,
                             PIN_CONFIG_MUX_FUNC0);
    }
    else
    {
        /* spi1 m0 iomux: CS0/CLK/MISO/MOSI all func2 on GPIO4 */
        HAL_PINCTRL_SetIOMUX(GPIO_BANK4,
                             GPIO_PIN_A0 | GPIO_PIN_A1 | GPIO_PIN_A5 | GPIO_PIN_A7,
                             PIN_CONFIG_MUX_FUNC2);
        /* control pins on GPIO1 as GPIO (func0) */
        HAL_PINCTRL_SetIOMUX(GPIO_BANK1,
                             GPIO_PIN_C5 | GPIO_PIN_C6 | GPIO_PIN_C7 | GPIO_PIN_D0 | GPIO_PIN_D1,
                             PIN_CONFIG_MUX_FUNC0);
    }

    dev->reset   = b->reset;
    dev->busy    = b->busy;
    dev->dio1    = b->dio1;
    dev->ant_sw0 = b->ant_sw0;
    dev->ant_sw1 = b->ant_sw1;

    HAL_GPIO_SetPinDirection(dev->reset.bank,   dev->reset.mask,   GPIO_OUT);
    HAL_GPIO_SetPinDirection(dev->busy.bank,    dev->busy.mask,    GPIO_IN);
    HAL_GPIO_SetPinDirection(dev->dio1.bank,    dev->dio1.mask,    GPIO_IN);
    HAL_GPIO_SetPinDirection(dev->ant_sw0.bank, dev->ant_sw0.mask, GPIO_OUT);
    HAL_GPIO_SetPinDirection(dev->ant_sw1.bank, dev->ant_sw1.mask, GPIO_OUT);

    sx_pin_out(&dev->reset, 0);   /* NRESET deasserted */
    dev->ant_sw_mode = SX1262_ANTSW_AUTO;
    sx1262_set_antsw(dev, SX1262_ANTSW_AUTO);

    /* SPI: master, mode 0, 8-bit MSB-first, 8 MHz, PIO */
    if (HAL_SPI_Init(&s_spi_bus[instance], b->spi_base, false) != HAL_OK)
        return -1;
    s_spi_bus[instance].maxFreq            = SX_SPI_INPUT_HZ;
    s_spi_bus[instance].config.nBytes      = CR0_DATA_FRAME_SIZE_8BIT;
    s_spi_bus[instance].config.clkPolarity = CR0_POLARITY_LOW;
    s_spi_bus[instance].config.clkPhase    = CR0_PHASE_1EDGE;
    s_spi_bus[instance].config.firstBit    = CR0_FIRSTBIT_MSB;
    s_spi_bus[instance].config.speed       = SX_SPI_HZ;
    s_spi_bus[instance].type               = SPI_POLL;

    dev->spi = &s_spi_bus[instance];
    dev->cs  = 0;
    dev->device_id = (uint8_t)instance;

    return 0;
}
