/* Portable command layer: compiles unchanged in the Linux kmod AND in the
 * RT-Thread MCU firmware (CubeSat migration, docs/migration/design/40-migration-design.md §4).
 * All OS services (SPI, GPIO, delays, logging) come from the HAL functions
 * declared below, implemented per-OS in sx1262_hal.c (Linux) or
 * sx1262_port_rtt.c (RT-Thread). */
#ifdef __KERNEL__
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/math64.h>
#include "sx1262.h"
#else
#include "sx1262_port.h"
#endif
#include "sx1262_regs.h"

extern int sx1262_spi_write(struct sx1262_device *dev, const uint8_t *data, size_t len);
extern int sx1262_spi_write_then_read(struct sx1262_device *dev,
                                        const uint8_t *tx, size_t tx_len,
                                        uint8_t *rx, size_t rx_len);
extern int sx1262_spi_transfer(struct sx1262_device *dev,
                                struct spi_transfer *xfers, unsigned int num);
extern int sx1262_wait_busy(struct sx1262_device *dev, unsigned long timeout_ms);
extern void sx1262_set_antsw(struct sx1262_device *dev, int mode);

/* Read single register: full-duplex single CS-asserted transaction.
 * Frame: cmd + addr_hi + addr_lo + NOP + NOP.
 * rx[4] = register value (after cmd, addr, and two dummies). */
int sx1262_read_register(struct sx1262_device *dev, uint16_t addr, uint8_t *val)
{
    uint8_t buf[5] = {
        SX1262_CMD_READ_REGISTER,
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        SX1262_CMD_NOP,
        SX1262_CMD_NOP
    };
    uint8_t rx[5] = { 0 };
    struct spi_transfer xfer = {
        .tx_buf = buf,
        .rx_buf = rx,
        .len = 5,
    };
    int ret;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_transfer(dev, &xfer, 1);
    if (ret) return ret;

    *val = rx[4];
    return 0;
}

/* Write single register */
int sx1262_write_register(struct sx1262_device *dev, uint16_t addr, uint8_t val)
{
    uint8_t tx[4] = { SX1262_CMD_WRITE_REGISTER, (addr >> 8) & 0xFF, addr & 0xFF, val };
    int ret;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, 4);
    if (ret) return ret;

    return sx1262_wait_busy(dev, 100);
}

/* Read multiple registers */
int sx1262_read_registers(struct sx1262_device *dev, uint16_t addr, uint8_t *buf, size_t len)
{
    uint8_t tx[3] = { SX1262_CMD_READ_REGISTER, (addr >> 8) & 0xFF, addr & 0xFF };
    size_t nop_len = len + 1; /* status byte + data (size_t: no u8 wrap at len=255) */
    uint8_t rx[260];
    int ret;

    if (nop_len > sizeof(rx))
        return -EINVAL;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write_then_read(dev, tx, 3, rx, nop_len);
    if (ret) return ret;

    memcpy(buf, rx + 1, len);
    return 0;
}

/* Write multiple registers */
int sx1262_write_registers(struct sx1262_device *dev, uint16_t addr, const uint8_t *buf, size_t len)
{
    uint8_t tx[260];
    int ret;

    if (len + 3 > sizeof(tx))
        return -EINVAL;

    tx[0] = SX1262_CMD_WRITE_REGISTER;
    tx[1] = (addr >> 8) & 0xFF;
    tx[2] = addr & 0xFF;
    memcpy(tx + 3, buf, len);

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, len + 3);
    if (ret) return ret;

    return sx1262_wait_busy(dev, 100);
}

/* Read FIFO buffer */
int sx1262_read_buffer(struct sx1262_device *dev, uint8_t offset, uint8_t *buf, size_t len)
{
    uint8_t tx[2] = { SX1262_CMD_READ_BUFFER, offset };
    uint8_t rx[260];
    int ret;

    if (len + 1 > sizeof(rx))
        return -EINVAL;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write_then_read(dev, tx, 2, rx, len + 1);
    if (ret) return ret;

    memcpy(buf, rx + 1, len);
    return 0;
}

/* Write FIFO buffer */
int sx1262_write_buffer(struct sx1262_device *dev, uint8_t offset, const uint8_t *buf, size_t len)
{
    uint8_t tx[260];
    int ret;

    if (len + 2 > sizeof(tx))
        return -EINVAL;

    tx[0] = SX1262_CMD_WRITE_BUFFER;
    tx[1] = offset;
    memcpy(tx + 2, buf, len);

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, len + 2);
    if (ret) return ret;

    return sx1262_wait_busy(dev, 100);
}

/* NOTE: this board uses a 32 MHz crystal (no TCXO) and an external antenna
 * switch driven by GPIO (see sx1262_set_antsw), so SetDIO3asTcxoCtrl and
 * SetDIO2asRfSwitchCtrl are intentionally not used. */

/* Set sleep mode */
int sx1262_set_sleep(struct sx1262_device *dev, uint8_t sleep_cfg)
{
    uint8_t tx[2] = { SX1262_CMD_SET_SLEEP, sleep_cfg };
    return sx1262_spi_write(dev, tx, 2);
}

/* Set regulator mode: 0=LDO only, 1=DC-DC + LDO */
int sx1262_set_regulator_mode(struct sx1262_device *dev, uint8_t mode)
{
    uint8_t tx[2] = { SX1262_CMD_SET_REGULATOR_MODE, mode };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set standby mode */
int sx1262_set_standby(struct sx1262_device *dev, uint8_t standby_cfg)
{
    uint8_t tx[2] = { SX1262_CMD_SET_STANDBY, standby_cfg };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set packet type (LoRa or GFSK) */
int sx1262_set_packet_type(struct sx1262_device *dev, uint8_t pkt_type)
{
    uint8_t tx[2] = { SX1262_CMD_SET_PACKET_TYPE, pkt_type };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    dev->packet_type = pkt_type;
    return sx1262_wait_busy(dev, 100);
}

/* Get packet type (debug/verify) */
int sx1262_get_packet_type(struct sx1262_device *dev, uint8_t *pkt_type)
{
    uint8_t tx[1] = { SX1262_CMD_GET_PACKET_TYPE };
    uint8_t rx[2] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 2);
    if (ret) return ret;
    *pkt_type = rx[1];
    return 0;
}

/* Set RF frequency.
 * SetRfFrequency (0x86) takes FOUR Frf bytes, MSB first:
 * Frf[31:24], Frf[23:16], Frf[15:8], Frf[7:0]. Sending only 3 drops the MSB
 * and lands the carrier on the wrong frequency (e.g. 915 -> ~768 MHz). */
int sx1262_set_frequency(struct sx1262_device *dev, uint32_t freq_hz)
{
    uint64_t rf_freq = div_u64((uint64_t)freq_hz * (1 << 25), 32000000);
    uint8_t tx[5] = {
        SX1262_CMD_SET_RF_FREQUENCY,
        (rf_freq >> 24) & 0xFF,
        (rf_freq >> 16) & 0xFF,
        (rf_freq >> 8) & 0xFF,
        rf_freq & 0xFF
    };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 5);
    if (ret) return ret;
    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    /* Image calibration is centred on the carrier (datasheet §9.2.1) and must be
     * redone when the frequency moves bands. Doing it on every SetRfFrequency
     * keeps runtime retunes correct, not just the init path. */
    return sx1262_calibrate_image_for_freq(dev, freq_hz);
}

/* Set buffer base addresses for FIFO (TX and RX) */
int sx1262_set_buffer_base_address(struct sx1262_device *dev, uint8_t tx_base, uint8_t rx_base)
{
    uint8_t tx[3] = { SX1262_CMD_SET_BUFFER_BASE_ADDRESS, tx_base, rx_base };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set fallback mode after RX/TX completion */
int sx1262_set_rx_tx_fallback_mode(struct sx1262_device *dev, uint8_t mode)
{
    uint8_t tx[2] = { SX1262_CMD_SET_RX_TX_FALLBACK_MODE, mode };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set PA config (optimal for SX1262) */
int sx1262_set_pa_config(struct sx1262_device *dev)
{
    uint8_t tx[5] = { SX1262_CMD_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 5);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set TX parameters (power dBm, ramp time).
 * On the SX1262 the power field is the signed dBm value directly
 * (-9..+22 with the high-power PA), NOT an SX127x-style offset. */
int sx1262_set_tx_params(struct sx1262_device *dev, int8_t power_dbm, uint8_t ramp_time)
{
    uint8_t tx[3];
    int ret;

    if (power_dbm > 22)  power_dbm = 22;
    if (power_dbm < -9)  power_dbm = -9;

    tx[0] = SX1262_CMD_SET_TX_PARAMS;
    tx[1] = (uint8_t)power_dbm;   /* two's-complement dBm */
    tx[2] = ramp_time;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Program output power: pick paDutyCycle/hpMax/paVal from the per-dBm table
 * (identical to the fixed 0x04/0x07 config at +22 dBm, cleaner below), issue
 * SetPaConfig + SetTxParams, then set OCP to 140 mA (reg 0x08E7 = 0x38) — the
 * SX1262 datasheet value (Table 5-2). SetPaConfig reloads OCP
 * to the 140 mA default anyway, so the write MUST come last and MUST be 0x38:
 * clamping OCP lower (e.g. 60 mA) starves the high-power PA at +22 dBm, which
 * clips mid-burst and splatters across ~1-2 MHz (worse as power rises). */
int sx1262_set_output_power(struct sx1262_device *dev, int8_t power)
{
    /* PA optimal-settings table (datasheet 13.1.14.1), index power+9, power in [-9, 22]. {paDutyCycle, hpMax, paVal}. */
    static const struct { uint8_t duty; uint8_t hp; int8_t val; } pa_opt[32] = {
        {2,2,-5},{2,1,0},{1,1,3},{1,2,0},{1,1,6},{1,2,3},{2,2,2},{4,1,6},
        {1,1,11},{2,1,11},{1,1,14},{2,1,14},{1,1,20},{1,1,22},{2,2,11},{3,1,21},
        {1,2,17},{4,2,13},{1,2,20},{1,2,22},{2,2,21},{3,2,21},{1,4,19},{1,4,20},
        {3,3,20},{2,5,19},{1,6,22},{2,5,22},{3,5,22},{3,6,22},{4,6,22},{4,7,22},
    };
    uint8_t pa[5];
    int idx, ret;

    if (power > 22)  power = 22;
    if (power < -9)  power = -9;
    idx = power + 9;

    /* SetPaConfig: paDutyCycle, hpMax, deviceSel=0x00 (SX1262), paLut=0x01 */
    pa[0] = SX1262_CMD_SET_PA_CONFIG;
    pa[1] = pa_opt[idx].duty;
    pa[2] = pa_opt[idx].hp;
    pa[3] = 0x00;
    pa[4] = 0x01;
    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, pa, 5);
    if (ret) return ret;
    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    /* SetTxParams: paVal from table, ramp 200us (0x04) */
    ret = sx1262_set_tx_params(dev, pa_opt[idx].val, 0x04);
    if (ret) return ret;

    /* OCP = 140 mA (0x38), SX1262 datasheet value (Table 5-2). Must
     * follow SetPaConfig (which reloads it) and must NOT be lower or the PA
     * clips at high power. */
    return sx1262_write_register(dev, 0x08E7, 0x38);
}

/* Set modulation parameters (LoRa) */
int sx1262_set_modulation_params(struct sx1262_device *dev, uint8_t sf, uint32_t bw, uint8_t cr, bool ldro)
{
    int ret;
    uint8_t bw_bits;
    uint8_t tx[9] = { SX1262_CMD_SET_MODULATION_PARAMS };

    /* LoRa bandwidth enum values per SX1262 datasheet (SetModulationParams) */
    switch (bw) {
        case 7800:     bw_bits = 0x00; break;
        case 10400:    bw_bits = 0x08; break;
        case 15600:    bw_bits = 0x01; break;
        case 20800:    bw_bits = 0x09; break;
        case 31250:    bw_bits = 0x02; break;
        case 41700:    bw_bits = 0x0A; break;
        case 62500:    bw_bits = 0x03; break;
        case 125000:   bw_bits = 0x04; break;
        case 250000:   bw_bits = 0x05; break;
        case 500000:   bw_bits = 0x06; break;
        default:       bw_bits = 0x04; break;  /* 125 kHz */
    }

    /* LDRO auto: mandatory when the LoRa symbol time exceeds 16.38 ms
     * (SF11/SF12 @125k, SF12 @250k, ...). the datasheet defines it the same
     * way; both link ends must agree or the packet won't demodulate. The
     * passed-in ldro is only an override for SF/BW where auto says off. */
    {
        uint64_t sym_us = ((uint64_t)(1ULL << sf) * 1000000ULL) / (bw ? bw : 1);
        if (sym_us > 16380ULL)
            ldro = true;
    }

    tx[1] = sf;         /* Spreading Factor */
    tx[2] = bw_bits;     /* Bandwidth */
    tx[3] = cr;          /* Coding Rate: 1=4/5, 2=4/6, 3=4/7, 4=4/8 */
    tx[4] = ldro ? 1 : 0; /* Low Data Rate Optimization */

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    /* LoRa SetModulationParams takes exactly 4 parameter bytes */
    ret = sx1262_spi_write(dev, tx, 5);
    if (ret) return ret;
    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    /* Errata 15.1: Tx modulation quality. Reg 0x0889 (TxModulation):
     * clear bit 2 for BW=500 kHz, set it otherwise. */
    {
        uint8_t reg = 0;
        if (sx1262_read_register(dev, 0x0889, &reg) == 0) {
            if (bw == 500000)
                reg &= ~0x04;
            else
                reg |= 0x04;
            sx1262_write_register(dev, 0x0889, reg);
        }
    }
    return 0;
}

/* Set packet parameters (LoRa) — byte order matches SX1262 datasheet */
int sx1262_set_packet_params(struct sx1262_device *dev, uint16_t preamble_len,
                              uint8_t hdr_type, uint8_t payload_len,
                              uint8_t crc_type, uint8_t iq_inverted)
{
    int ret;
    uint8_t tx[7] = {
        SX1262_CMD_SET_PACKET_PARAMS,
        (preamble_len >> 8) & 0xFF,    /* PreambleLength MSB */
        preamble_len & 0xFF,            /* PreambleLength LSB */
        hdr_type,                       /* HeaderType: 0=variable, 1=fixed */
        payload_len,                    /* PayloadLength */
        crc_type,                       /* CrcType: 0=off, 1=on */
        iq_inverted ? 0x01 : 0x00      /* InvertIQ (datasheet Table 13-70:
                                        * 0x00=standard, 0x01=inverted). Was
                                        * wrongly 0x40 (bit 6 is not InvertIQ). */
    };

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 7);
    if (ret) return ret;
    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    /* Errata 15.4: Optimizing the Inverted-IQ Operation. RegIqPolaritySetup
     * (0x0736) bit 2 must be CLEARED when IQ is inverted and SET when IQ is
     * standard; otherwise LoRa packets with inverted IQ are frequently lost.
     * Applied on every SetPacketParams so RX/TX polarity always tracks it. */
    {
        uint8_t reg = 0;
        if (sx1262_read_register(dev, 0x0736, &reg) == 0) {
            if (iq_inverted)
                reg &= ~0x04;
            else
                reg |= 0x04;
            sx1262_write_register(dev, 0x0736, reg);
        }
    }
    return 0;
}

/* LoRa sync word (regs 0x0740 MSB / 0x0741 LSB). Reset default is 0x1424
 * ("private network"); LoRaWAN/public networks use 0x3444 (SX127x lore calls
 * these 0x12/0x34). Both ends of a link MUST match or packets are never
 * detected. Like the rest of the config, it is lost on chip reset. */
int sx1262_set_sync_word(struct sx1262_device *dev, uint16_t sync)
{
    int ret = sx1262_write_register(dev, 0x0740, (uint8_t)(sync >> 8));
    if (ret) return ret;
    return sx1262_write_register(dev, 0x0741, (uint8_t)sync);
}

/* Set DIO IRQ params via the SetDioIrqParams command (opcode 0x08).
 * Routes the requested IRQs to the global IRQ register and to DIO1. */
int sx1262_set_dio_irq_params(struct sx1262_device *dev, uint16_t irq_mask)
{
    int ret;
    uint8_t irq_h = (irq_mask >> 8) & 0xFF;
    uint8_t irq_l = irq_mask & 0xFF;
    uint8_t tx[9] = {
        SX1262_CMD_SET_DIO_IRQ_PARAMS,
        irq_h, irq_l,   /* IrqMask  */
        irq_h, irq_l,   /* DIO1Mask */
        0x00, 0x00,     /* DIO2Mask */
        0x00, 0x00,     /* DIO3Mask */
    };

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 9);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Get RX buffer status: payload length and start pointer of last packet. */
int sx1262_get_rx_buffer_status(struct sx1262_device *dev, uint8_t *payload_len,
                                 uint8_t *start_ptr)
{
    uint8_t tx[1] = { 0x13 };  /* GetRxBufferStatus */
    uint8_t rx[3] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 3);
    if (ret) return ret;
    /* rx[0]=status, rx[1]=PayloadLengthRx, rx[2]=RxStartBufferPointer */
    if (payload_len) *payload_len = rx[1];
    if (start_ptr)   *start_ptr = rx[2];
    return 0;
}

/* Clear IRQ status */
int sx1262_clear_irq_status(struct sx1262_device *dev, uint16_t irq_mask)
{
    uint8_t tx[3] = {
        SX1262_CMD_CLR_IRQ_STATUS,
        (irq_mask >> 8) & 0xFF,
        irq_mask & 0xFF
    };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Get IRQ status */
int sx1262_get_irq_status(struct sx1262_device *dev, uint16_t *irq_status)
{
    uint8_t buf[4] = { SX1262_CMD_GET_IRQ_STATUS, SX1262_CMD_NOP, SX1262_CMD_NOP, SX1262_CMD_NOP };
    uint8_t rx[4] = { 0 };
    struct spi_transfer xfer = { .tx_buf = buf, .rx_buf = rx, .len = 4 };
    int ret = sx1262_wait_busy(dev, 10);
    if (ret) return ret;
    ret = sx1262_spi_transfer(dev, &xfer, 1);
    if (ret) return ret;
    *irq_status = (rx[2] << 8) | rx[3];
    return 0;
}

/* Poll IRQ status until a given flag is set (with timeout), or timeout_ms elapses */
int sx1262_poll_irq(struct sx1262_device *dev, uint16_t flag, unsigned long timeout_ms)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
    uint16_t irq;
    int ret;

    do {
        ret = sx1262_get_irq_status(dev, &irq);
        if (ret) return ret;
        if (irq & flag)
            return 0;  /* success — do NOT clear; caller reads/clears the IRQ */
        if (time_after(jiffies, deadline))
            return -ETIMEDOUT;
        usleep_range(1000, 2000);
    } while (1);
}

/* Get device errors */
int sx1262_get_dev_errors(struct sx1262_device *dev, uint16_t *errors)
{
    uint8_t buf[4] = { SX1262_CMD_GET_DEVICE_ERRORS, SX1262_CMD_NOP, SX1262_CMD_NOP, SX1262_CMD_NOP };
    uint8_t rx[4] = { 0 };
    struct spi_transfer xfer = { .tx_buf = buf, .rx_buf = rx, .len = 4 };
    int ret = sx1262_wait_busy(dev, 10);
    if (ret) return ret;
    ret = sx1262_spi_transfer(dev, &xfer, 1);
    if (ret) return ret;
    *errors = (rx[2] << 8) | rx[3];
    return 0;
}

/* Clear device errors */
int sx1262_clear_dev_errors(struct sx1262_device *dev)
{
    /* Datasheet Table 13-86: ClearDeviceErrors = opcode + 0x00 + 0x00 (3 bytes).
     * Sending only the opcode leaves the errors uncleared (command param error). */
    uint8_t tx[3] = { SX1262_CMD_CLEAR_DEV_ERRORS, 0x00, 0x00 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Get chip status */
int sx1262_get_status(struct sx1262_device *dev, uint8_t *status)
{
    uint8_t tx[1] = { SX1262_CMD_GET_STATUS };
    uint8_t rx[2] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 2);
    if (ret) return ret;
    *status = rx[1];
    return 0;
}

/* Get RSSI (instantaneous) */
int sx1262_get_rssi_inst(struct sx1262_device *dev, int16_t *rssi)
{
    uint8_t tx[1] = { SX1262_CMD_GET_RSSI_INST };
    uint8_t rx[2] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 2);
    if (ret) return ret;
    *rssi = -(int16_t)(rx[1] / 2);
    return 0;
}


/* Calibrate */
int sx1262_calibrate(struct sx1262_device *dev, uint8_t calib_params)
{
    uint8_t tx[2] = { SX1262_CMD_CALIBRATE, calib_params };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 30000); /* calibration can take up to 30ms */
}

/* Calibrate image for given frequency band (start/end) */
int sx1262_calibrate_image(struct sx1262_device *dev, uint8_t freq_band_start, uint8_t freq_band_end)
{
    /* Datasheet Table 13-19: CalibrateImage = opcode + freq1 + freq2 (3 bytes).
     * The old trailing 0x00 was a spurious param byte (command param error). */
    uint8_t tx[3] = { SX1262_CMD_CALIBRATE_IMAGE, freq_band_start, freq_band_end };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 30000);
}

/* Calibrate image picking the band that contains freq_hz (datasheet Table 9-2) */
int sx1262_calibrate_image_for_freq(struct sx1262_device *dev, uint32_t freq_hz)
{
    uint8_t start, end;

    if (freq_hz >= 902000000)       { start = 0xE1; end = 0xE9; } /* 902-928 */
    else if (freq_hz >= 863000000)  { start = 0xD7; end = 0xDB; } /* 863-870 */
    else if (freq_hz >= 779000000)  { start = 0xC1; end = 0xC5; } /* 779-787 */
    else if (freq_hz >= 470000000)  { start = 0x75; end = 0x81; } /* 470-510 */
    else                            { start = 0x6B; end = 0x6F; } /* 430-440 */

    return sx1262_calibrate_image(dev, start, end);
}

/* Set TX (timeout in ms, 0 = instant/continuous).
 * Returns when TX completes (TX_DONE IRQ flag seen via polling) or error. */
int sx1262_set_tx(struct sx1262_device *dev, uint32_t timeout_ms)
{
    int ret;
    uint32_t timeout_raw = 0;
    uint8_t tx[4] = { SX1262_CMD_SET_TX };

    if (timeout_ms > 0) {
        timeout_raw = timeout_ms * 64U;
    }

    tx[1] = (timeout_raw >> 16) & 0xFF;
    tx[2] = (timeout_raw >> 8) & 0xFF;
    tx[3] = timeout_raw & 0xFF;

    if (dev->ant_sw_mode == SX1262_ANTSW_AUTO)
        sx1262_set_antsw(dev, SX1262_ANTSW_TX);

    reinit_completion(&dev->dio1_completion);
    atomic_set(&dev->irq_flags, 0);
    dev->tx_in_progress = true;
    dev->rx_in_progress = false;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, 4);
    if (ret) return ret;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    /* Poll for TX_DONE (with chip timeout + 100ms margin) */
    if (timeout_ms > 0) {
        ret = sx1262_poll_irq(dev, SX1262_IRQ_TX_DONE, timeout_ms + 100);
        if (ret == -ETIMEDOUT)
            dev_dbg(&dev->spi->dev, "TX poll timeout\n");
        /* poll_irq no longer clears; clear TX_DONE here */
        sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
    }

    dev->tx_in_progress = false;
    return 0;
}

/* Start a continuous unmodulated carrier (for spectrum-analyzer / bring-up).
 * Stays on until SetStandby/SetRx/SetTx. ANT_SW is forced to TX. */
int sx1262_set_tx_continuous_wave(struct sx1262_device *dev)
{
    uint8_t tx[1] = { SX1262_CMD_SET_TX_CONTINUOUS };
    int ret;

    if (dev->ant_sw_mode == SX1262_ANTSW_AUTO)
        sx1262_set_antsw(dev, SX1262_ANTSW_TX);

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 1);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set RX (timeout in ms, 0 = continuous). Datasheet §14.3 order: clear IRQ ->
 * SetRx -> wait BUSY. No post-arm delay/clear (that would wipe an RxDone that
 * lands immediately) and no per-arm dev-error clear (that erases real errors). */
int sx1262_set_rx(struct sx1262_device *dev, uint32_t timeout_ms)
{
    int ret;
    uint32_t timeout_raw;
    uint8_t tx[4] = { SX1262_CMD_SET_RX };

    /* ms == 0 -> continuous RX (0xFFFFFF). Otherwise timeout in 15.625us steps. */
    if (timeout_ms == 0)
        timeout_raw = 0xFFFFFF;
    else
        timeout_raw = timeout_ms * 64U;

    tx[1] = (timeout_raw >> 16) & 0xFF;
    tx[2] = (timeout_raw >> 8) & 0xFF;
    tx[3] = timeout_raw & 0xFF;

    if (dev->ant_sw_mode == SX1262_ANTSW_AUTO)
        sx1262_set_antsw(dev, SX1262_ANTSW_RX);

    reinit_completion(&dev->dio1_completion);
    atomic_set(&dev->irq_flags, 0);
    dev->tx_in_progress = false;
    dev->rx_in_progress = true;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    /* Clear IRQ BEFORE arming — clearing after SetRx would drop an immediate RxDone. */
    sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);

    ret = sx1262_spi_write(dev, tx, 4);
    if (ret) return ret;

    return sx1262_wait_busy(dev, 100);
}

/* Initialize device: full startup sequence per the SX1262 datasheet */
int sx1262_init(struct sx1262_device *dev, uint32_t freq_hz)
{
    int ret;

    /* --- Step 1: Standby RC --- */
    ret = sx1262_set_standby(dev, SX1262_STANDBY_RC);
    if (ret) { dev_err(&dev->spi->dev, "Standby failed\n"); return ret; }

    /* --- Step 1a: Regulator mode DC-DC + LDO (recommended for SX1262) ---
     * NOTE: these modules use a plain 32 MHz crystal (XOSC starts cleanly with
     * dev_errors=0). DIO3-as-TCXO control makes XOSC fail (XOSC_START_ERR 0x20),
     * so it is intentionally NOT configured. */
    ret = sx1262_set_regulator_mode(dev, 0x01);
    if (ret) { dev_err(&dev->spi->dev, "Regulator mode failed\n"); return ret; }

    /* NOTE: on this board the RF switch is NOT driven by the chip's DIO2; the
     * schematic's DIO2 net is driven by an RV1106 GPIO (named ant-sw1 here).
     * So we do NOT call SetDio2AsRfSwitchCtrl — the switch is toggled via the
     * ant-sw GPIOs in sx1262_set_antsw(). */

    /* --- Step 2: Set packet type (LoRa) --- */
    ret = sx1262_set_packet_type(dev, SX1262_PKT_TYPE_LORA);
    if (ret) { dev_err(&dev->spi->dev, "Set packet type failed\n"); return ret; }

    /* --- Step 3: Set fallback mode to STDBY_RC --- */
    ret = sx1262_set_rx_tx_fallback_mode(dev, SX1262_FALLBACK_RC);
    if (ret) { dev_err(&dev->spi->dev, "Fallback mode failed\n"); return ret; }

    /* --- Step 4: Calibrate all blocks --- */
    ret = sx1262_calibrate(dev, 0x7F);
    if (ret) { dev_err(&dev->spi->dev, "Calibrate failed\n"); return ret; }

    /* Errata 15.2 (datasheet): better Tx resistance to antenna mismatch.
     * Reg 0x08D8 bits 4-1 = 1111 (|= 0x1E). */
    {
        uint8_t reg = 0;
        if (sx1262_read_register(dev, 0x08D8, &reg) == 0)
            sx1262_write_register(dev, 0x08D8, reg | 0x1E);
    }

    /* --- Step 7: Set frequency. sx1262_set_frequency() now runs CalibrateImage
     * AFTER SetRfFrequency internally (image is centred on the carrier; the old
     * calibrate-before-set-freq order left it tuned to the POR ~915 MHz and RX
     * only worked near 915). --- */
    ret = sx1262_set_frequency(dev, freq_hz);
    if (ret) { dev_err(&dev->spi->dev, "Set frequency failed\n"); return ret; }

    /* --- Step 8: Output power (SetPaConfig + SetTxParams + OCP 140 mA).
     * default 20 dBm. --- */
    ret = sx1262_set_output_power(dev, 20);
    if (ret) { dev_err(&dev->spi->dev, "Output power failed\n"); return ret; }

    /* --- Step 9: Modulation params. Default SF7, BW125, CR4/5; LDRO auto-computed inside set_modulation_params. --- */
    ret = sx1262_set_modulation_params(dev, 7, 125000, 1, false);
    if (ret) { dev_err(&dev->spi->dev, "Modulation params failed\n"); return ret; }

    /* --- Step 10: Set buffer base addresses --- */
    ret = sx1262_set_buffer_base_address(dev, 0x00, 0x00);
    if (ret) { dev_err(&dev->spi->dev, "Buffer base addr failed\n"); return ret; }

    /* --- Step 11: Packet params. Default preamble 12, explicit header, CRC ON, IQ standard. --- */
    ret = sx1262_set_packet_params(dev, 12, 0, 0xFF, 1, 0);
    if (ret) { dev_err(&dev->spi->dev, "Packet params failed\n"); return ret; }

    /* --- Step 11b: LoRa sync word. Default private 0x1424 (datasheet reset value);
     * host can switch to public 0x3444. --- */
    ret = sx1262_set_sync_word(dev, 0x1424);
    if (ret) { dev_err(&dev->spi->dev, "Sync word failed\n"); return ret; }

    /* --- Step 12: Clear IRQ and route TxDone/RxDone/Timeout/CrcErr to DIO1.
     * CRC_ERR must be in the mask or the poll loop's CRC-ok flag is meaningless. --- */
    sx1262_clear_irq_status(dev, 0xFFFF);
    ret = sx1262_set_dio_irq_params(dev, SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE |
                                        SX1262_IRQ_TIMEOUT | SX1262_IRQ_CRC_ERR);
    if (ret) { dev_err(&dev->spi->dev, "DIO IRQ failed\n"); return ret; }

    /* --- Step 13: Rx Boosted Gain for best sensitivity. Datasheet Table 9-3 /
     * reg map: reg 0x08AC reset = 0x94 (power-saving); 0x96 = boosted gain. --- */
    sx1262_write_register(dev, 0x08AC, 0x96);

    /* --- Step 14: Clear any device errors accumulated during calibration --- */
    sx1262_clear_dev_errors(dev);

    return 0;
}

/* Get packet status (RSSI, SNR) for the last LoRa packet.
 * GetPacketStatus (0x14) response bytes, in order: Status, RssiPkt, SnrPkt,
 * SignalRssiPkt. In this port's write_then_read the opcode is a separate phase,
 * so the read phase yields rx[0]=Status, rx[1]=RssiPkt, rx[2]=SnrPkt,
 * rx[3]=SignalRssiPkt (proven: get_rx_buffer_status reads PayloadLength at
 * rx[1] and returns the exact payload length). Read RSSI/SNR from rx[1]/rx[2] —
 * NOT rx[2]/rx[3], which would report SnrPkt as RSSI. */
int sx1262_get_packet_status(struct sx1262_device *dev, int16_t *rssi_pkt, int8_t *snr)
{
    uint8_t tx[1] = { SX1262_CMD_GET_PKT_STATUS };
    uint8_t rx[4] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 4);
    if (ret) return ret;
    *rssi_pkt = -(int16_t)(rx[1] / 2);   /* RssiPkt: -RssiPkt/2 dBm */
    *snr = (int8_t)rx[2] / 4;            /* SnrPkt:  signed, /4 dB   */
    return 0;
}
