# SX1262 Dual LoRa Radios

## Overview

Both Semtech SX1262 LoRa transceivers are driven by the **RISC-V MCU** running
RT-Thread. The MCU exposes them through a single **`RadioService`** on the rpmsg
channel **`rpmsg-radio`**, endpoint **`0x4005`**. A per-message `instance` byte
selects which physical radio a command targets:

- **Radio 0**: MCU SPI0
- **Radio 1**: MCU SPI1

Linux has no kernel driver for the chips on-device; it opens the rpmsg endpoint
and speaks the RadioService protocol from userspace via the **`radio_test`**
client (`src/radio-client/`). The command set (register R/W, frequency,
modulation, packet params, TX/RX, calibration, IRQ poll) mirrors the legacy
Linux driver it was ported from.

Higher-level CCSDS uplink/dispatch (`CommandService`, endpoint `0x4008`) and
telemetry downlink (`TelemetryService`, endpoint `0x4007`) sit above the radios;
see [`../architecture/ipc-rpmsg.md`](../architecture/ipc-rpmsg.md).

## Driving it from Linux

`radio_test` opens the `rpmsg-radio` endpoint and sends one framed command per
invocation. The global **`-r 0|1`** flag selects the radio (default 0). The chip
loses its configuration on power-off or reset, so a session always starts with
`init`.

Subcommands fall in **two families** (`radio_test help` labels them):

- **Native** — direct radio control over *our own* `RadioService` (rpmsg
  endpoint `0x4005`). These are the commands we created.
- **Inherited** — CCSDS telecommand/telemetry protocol **inherited from the
  ElectronicCats/FlatSat reference firmware** (which we ported for interop), via
  the `CommandService` (`0x4008`) and `TelemetryService` (`0x4007`).

**Native — RadioService (direct SX1262 control):**

| Subcommand | Purpose |
|------------|---------|
| `ping` | Verify the MCU RadioService responds (id `RDIO`) |
| `reset` | Physical chip reset (loses init, returns to standby) |
| `init <freq> [sf= bw= cr= power= pre= crc= iq= sync=]` | Init + configure TX; named params in any order |
| `freq <freq>` | Change frequency only (requires prior init) |
| `power <dbm>` | TX power in dBm, -9..22 (requires prior init) |
| `cw` / `stop` | Continuous carrier on / standby (spectrum bring-up) |
| `status` / `errors` | Chip mode (2=standby,4=FS,5=RX,6=TX) / internal error flags |
| `reg <addr>` / `wreg <addr> <val>` | Raw register read / write |
| `sync <pub\|priv\|hex>` | LoRa sync word (priv=0x1424 default, pub=0x3444) |
| `antsw <0\|1\|2>` | Antenna switch: 0=auto, 1=TX, 2=RX |
| `mod <sf> <bw_khz> <cr>` | Configure LoRa modulation |
| `pkt <pre> <hdr> <plen> <crc> <iq>` | Configure packet params |
| `pkts` / `rssi` | Last-packet RSSI+SNR / instantaneous RSSI |
| `tx <text>` | Transmit one LoRa packet (blocks until TX_DONE) |
| `rx [ms]` | Receive (ms window, default 10000, 0 = continuous) |
| `loopback <freq> <txt> [txi rxi]` | On-board radio0→radio1 test in one command |

**Inherited — CCSDS/FlatSat protocol (ported firmware):**

A telecommand (TC) is a CCSDS Space Packet; the MCU acts on it exactly as if it
had arrived over RF. The `tc*` commands inject over rpmsg (bypassing the air);
`ccsds` sends that same format **over the radio**.

| Subcommand | Endpoint | Purpose |
|------------|----------|---------|
| `ccsds <apid> [txt]` | Radio (`0x4005`) | TX a CCSDS SPP packet over the air (inherited format, native transport) |
| `tcsend <apid_hex> [pay_hex…]` | Command (`0x4008`) | Inject a plaintext TC over rpmsg (bypass RF). e.g. `tcsend 04 00 0A` = thruster0=10 |
| `tcsecsend <apid_hex> [pay_hex…]` | Command (`0x4008`) | Inject a **secured** TC (timestamp sec-hdr + AES-128-CTR/XOR/plain + CRC-16); difficulty via env `CCSDS_DIFF` (must match the receiver) |
| `tcbroad <freq> [txt]` | Command (`0x4008`) | `TC_BROADCAST_MSG` (APID 0x06) on an arbitrary frequency (430–960 MHz) |
| `cmd_ping` / `cmd_start <freq>` / `cmd_stop` | Command (`0x4008`) | Ping / start (configure uplink) / stop the service |
| `cmd_status` / `cmd_config <freq> [sf bw cr]` | Command (`0x4008`) | State (thrusters, beacon, TC count) / reconfigure the uplink |
| `cmd_listen` / `cmd_watch` | Command (`0x4008`) | Stream `EVT_TC_RX` / unified TC+effect view |
| `tlm` | Telemetry (`0x4007`) | Downlink monitor (TM/beacon/idle/responses) |

> The CCSDS SPP protocol and its attack surface are detailed in
> [`../security/exploitation-guide.md`](../security/exploitation-guide.md); the
> rpmsg endpoint routing in [`../architecture/ipc-rpmsg.md`](../architecture/ipc-rpmsg.md).

**Example invocations:**

```bash
# Configure radio 0 for the operational uplink and confirm it is alive
radio_test ping
radio_test init 918000000        # SF7 BW250 CR4/5 pre8 CRC on 20 dBm sync priv

# Transmit a packet, then listen on radio 1
radio_test -r 0 tx "hello world"
radio_test -r 1 rx 15000

# One-shot on-board loopback (inits both radios, radio 1 listens, radio 0 sends)
radio_test loopback 918000000 "ping"
```

### Operational on-air defaults

These are the parameters that actually go out over the air (distinct from the
bare-chip init values):

| Parameter | Value |
|-----------|-------|
| Uplink frequency | 918 MHz |
| Downlink frequency | 916 MHz |
| Spreading factor | SF7 |
| Bandwidth | **250 kHz** |
| Coding rate | 4/5 |
| TX power | **20 dBm** |
| CRC | **on** |
| IQ | standard |
| Preamble | **8** symbols |
| Sync word | 0x1424 (private) |

> The bare `sx1262_init` on the chip programs SF7/BW125/preamble 12/20 dBm, but
> the operational path overrides bandwidth to 250 kHz and preamble to 8 — the
> values above are what the link uses.

## Hardware reference (physical device)

These traits describe the SX1262 modules on the board, independent of who drives
them. For the full pin-ownership map (which subsystem claims each GPIO) see
[`../architecture/peripheral-ownership.md`](../architecture/peripheral-ownership.md).

Module traits (verified on the board):
- **32 MHz crystal (XTAL), NOT a TCXO** — DIO3-as-TCXO must **not** be enabled
  (enabling it makes the XOSC fail to start).
- **External antenna RF switch driven by GPIO** (not the chip's DIO2):
  **TX = ANT_SW0=1, ANT_SW1=0; RX = ANT_SW0=0, ANT_SW1=1**.
- OCP register `0x08E7 = 0x38` (140 mA).
- InvertIQ register writes `0x01` (correct; `0x40` was an old bug).
- Image calibration runs **after** `SetRfFrequency` and is re-run on every retune.
- RX is **continuous** (re-arms after each packet), not single-shot.

### Pin assignments

**Radio 0 (SPI0)**
| SX1262 Pin | RV1106 GPIO | Function |
|------------|-------------|----------|
| CS / SCK / MOSI / MISO | GPIO1_C0 / C1 / C2 / C3 | SPI0 |
| RST  | GPIO3_A6 | GPIO (active low) |
| BUSY | GPIO3_A7 | GPIO in |
| DIO1 | GPIO3_A3 | GPIO + IRQ |
| ANT_SW0 | GPIO0_A3 | GPIO out |
| ANT_SW1 | GPIO0_A4 | GPIO out |

**Radio 1 (SPI1)**
| SX1262 Pin | RV1106 GPIO | Function |
|------------|-------------|----------|
| CS / SCK / MISO / MOSI | GPIO4_A5 / A7 / A0 / A1 | SPI1 |
| RST  | GPIO1_C6 | GPIO (active low) |
| BUSY | GPIO1_C7 | GPIO in |
| DIO1 | GPIO1_D1 | GPIO + IRQ |
| ANT_SW0 | GPIO1_C5 | GPIO out |
| ANT_SW1 | GPIO1_D0 | GPIO out |

GPIO number = `bank*32 + (A=0,B=8,C=16,D=24) + index`.

---

## Legacy: Linux kernel driver (reference only)

> **This driver does NOT bind on-device.** Both SPI buses are `status = "disabled"`
> in the DTS (ceded to the MCU — see the banner), so `sx1262.ko` never probes.
> It is retained as source and as the reference the MCU command set was ported
> from. The ioctl/sysfs details below are for readers who resurrect the
> Linux-driven path.

Historically the two radios were driven by a native Linux kernel driver
(`sx1262.ko`), each exposed as a character device:

- **SX1262 #0**: SPI0 → `/dev/sx1262-0`
- **SX1262 #1**: SPI1 → `/dev/sx1262-1`

A userspace CLI (`sx1262_cli`, packaged as **sx1262**) talked to the driver
through ioctls / read() / write().

```
Userspace                  Kernel                     Hardware
sx1262_cli ──ioctl──►  sx1262.ko ──spi_sync──►  SX1262 #0 (SPI0)
  (--dev 0)              │                           │
                        ├── gpiod_set_value()  ──►  RST, ANT_SW0/1
                        ├── gpiod_get_value()  ◄──  BUSY
                        ├── request_threaded_irq() DIO1
sx1262_cli ──ioctl──►  sx1262.ko ──spi_sync──►  SX1262 #1 (SPI1)
  (--dev 1)
```

### Kernel driver (`src/sx1262-kmod/`)

| File | Purpose |
|------|---------|
| `sx1262_core.c`    | Module init, SPI driver registration, chardev/class alloc, per-radio probe |
| `sx1262_hal.c`     | SPI transfers, GPIO control (gpiod), DIO1 IRQ handler, antenna switch |
| `sx1262_cmd.c`     | SX1262 command set: register R/W, FIFO, freq, modulation, TX/RX, calibration, IRQ poll |
| `sx1262_chardev.c` | `/dev/sx1262-{0,1}`: ioctl, write (TX), read (RX), poll |
| `sx1262.h`         | Public header (ioctl definitions, structs) |
| `sx1262_regs.h`    | Command opcodes, register map, IRQ masks |

**Device nodes:** `/dev/sx1262-0`, `/dev/sx1262-1`

**ioctls** (magic `'L'`):

| Command | Arg | Description |
|---------|-----|-------------|
| `SX1262_IOCTL_RESET`         | — | Hardware reset |
| `SX1262_IOCTL_SET_STANDBY`   | — | Enter STDBY_RC |
| `SX1262_IOCTL_SET_SLEEP`     | — | Enter sleep |
| `SX1262_IOCTL_SET_FREQ`      | `uint32_t` Hz | Set frequency (re-runs image calibration for the band) |
| `SX1262_IOCTL_SET_POWER`     | `int8_t` dBm | TX power (-9..22) |
| `SX1262_IOCTL_SET_MODEM`     | `struct sx1262_modem_config` | SF / BW / CR / preamble |
| `SX1262_IOCTL_SET_PACKET`    | `struct sx1262_packet_config` | header / length / CRC / IQ |
| `SX1262_IOCTL_SET_TX`        | `uint32_t` ms | Start TX (timeout) |
| `SX1262_IOCTL_SET_RX`        | `uint32_t` ms | Start RX (0 = continuous) |
| `SX1262_IOCTL_SET_CW`        | — | Continuous unmodulated carrier (bring-up / spectrum) |
| `SX1262_IOCTL_GET_STATUS`    | `uint8_t` | Chip status byte |
| `SX1262_IOCTL_GET_RSSI`      | `int16_t` | Instantaneous RSSI |
| `SX1262_IOCTL_GET_IRQ_STATUS`| `uint16_t` | IRQ status register |
| `SX1262_IOCTL_SET_ANTSW`     | `uint8_t` | 0=auto, 1=force TX, 2=force RX |
| `SX1262_IOCTL_READ_REG` / `WRITE_REG` | `struct sx1262_reg_access` | Raw register access |

**write()** loads the FIFO, sets packet length, switches the antenna to TX, starts TX and waits for TX_DONE.
**read()** blocks until DIO1 fires (RX_DONE/timeout), then returns the exact received payload (length from GetRxBufferStatus). CRC errors return no data.
**poll(POLLIN)** waits on the DIO1 IRQ.

The radio auto-initialises in `sx1262_init()` on probe: STDBY_RC → DC-DC regulator
→ LoRa packet type → fallback STDBY_RC → calibrate → image calibration for the band
→ PA config (+ TxClamp errata) → frequency → TX params → modulation (SF7/BW125/CR4-5,
+ TxModulation errata) → buffer base → packet params → DIO1 IRQ (TxDone/RxDone/Timeout).

### Legacy CLI tool (`sx1262_cli`)

```
Usage: sx1262_cli --dev <0|1> <command> [args]

  detect                 Check the chip responds
  reset | standby | sleep
  freq <hz>              Set frequency (Hz)
  power <dbm>            TX power (-9..22)
  modem <sf> <bwhz> <cr> e.g. 7 125000 1  (SF7, BW125k, CR4/5)
  setTX <hz> <ms>        Set freq + start TX
  setRX <hz> <ms>        Set freq + start RX (0 = continuous)
  cw <hz>                Continuous carrier (stop with 'standby')
  send <hex> [ms]        Load FIFO + transmit (waits TX_DONE)
  recv [ms]              Receive one packet (poll DIO1 + read)
  rssi | status | irq
  regread <hexaddr> | regwrite <hexaddr> <hexval>
```

### Legacy implementation notes

- `SetRfFrequency` sends 4 Frf bytes (`0x86, Frf[31:24..7:0]`); a missing MSB lands
  the carrier on the wrong frequency.
- `sx1262_poll_irq()` does **not** clear the IRQ — the caller (`read()` / `set_tx()`)
  reads and clears it, so RX_DONE is not lost before `read()` consumes it.
- Errata applied: 15.1 (TxModulation, reg 0x0889) and 15.2 (TxClampConfig, reg 0x08D8).
- RX payload length comes from GetRxBufferStatus; image calibration is re-run for the
  operating band on each frequency change.
