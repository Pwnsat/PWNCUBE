# Peripheral ownership — RISC-V MCU vs Linux

The RV1106 runs **two cores**: a Cortex-A7 (Linux) and an SCR1 (RISC-V, RT-Thread).
Several peripherals are **owned by the MCU**, not by Linux. This document is the single
reference for *which* peripherals, *how* the hand-off is enforced, and the exact pin/bus map.

See also: [`overview.md`](overview.md) (core roles), [`ipc-rpmsg.md`](ipc-rpmsg.md) (the
services), [`../build/kernel.md`](../build/kernel.md) (DTS changes).

## Who owns what

| Peripheral | Bus / address | Owner | rpmsg service (endpoint) | Linux device tree |
|---|---|---|---|---|
| SX1262 radio 0 | SPI0 | **MCU** | RadioService (`rpmsg-radio`, 0x4005) | `&spi0 { status = "disabled"; }` |
| SX1262 radio 1 | SPI1 | **MCU** | RadioService (`rpmsg-radio`, 0x4005) | `&spi1 { status = "disabled"; }` |
| BME280 | I²C0 @ 0x76 | **MCU** | SensorService (`rpmsg-sensor`, 0x4006) | `&i2c0 { status = "disabled"; }` |
| ICM-42670-P | I²C0 @ 0x68 | **MCU** | SensorService (`rpmsg-sensor`, 0x4006) | `&i2c0 { status = "disabled"; }` |
| CCSDS uplink / downlink | via the radios | **MCU** | CommandService (0x4008) / TelemetryService (0x4007) | — |

Linux drives all of these indirectly, over rpmsg, through the Linux clients
`src/radio-client/` (`radio_test`) and `src/sensor-client/` (`sensor_test`).

## The mechanism — how Linux is kept off the bus

The split has **two complementary halves** that must agree on the exact pins:

1. **Linux cedes the controller.** In the device tree (`dts/rv1106-sdk-ipc.dtsi`) the
   controller node is set to `status = "disabled"`, and the matching Linux driver/package
   is left out (`pkg/package-config`). Rebuilding the DTB frees the pins. Peripherals that
   would collide on those pins are disabled too (`&uart3/4/5`, several `&pwm*`).
2. **The MCU claims the peripheral at runtime** (the MCU has *no* device tree): enable the
   clocks (`HAL_CRU_ClkEnable`) → program the IOMUX (`HAL_PINCTRL_SetIOMUX`, using the same
   pin/function the Linux DT would have used) → drive the control GPIOs.

> **⚠️ Convention, not hardware isolation.** This ownership split is enforced *only* by the
> disabled DT node plus the MCU grabbing the IOMUX first. There is **no** `reserved-memory`,
> bus firewall, or remoteproc peripheral reservation for SPI/I²C/GPIO. Nothing physically
> stops Linux from re-enabling a controller node; correctness depends on (a) keeping the DT
> node disabled and (b) the two sides agreeing on the exact pins/functions. The child nodes
> left under the disabled `&i2c0`/`&spi0` in the DTS (`bme280@76`, `icm42670@68`, `sx1262@0`)
> are kept **as wiring documentation only** — they do not bind while the parent is disabled.

## Pin & bus map

### SPI0 — SX1262 radio 0 (RadioService)
Clocks: `PCLK_SPI0_GATE`, `CLK_SPI0_GATE`, `SCLK_IN_SPI0_GATE` (VEPUCRU). Master, mode 0,
8-bit MSB-first, 8 MHz.

| Signal | RV1106 GPIO | IOMUX |
|---|---|---|
| CS / CLK | GPIO1_C0 / C1 | spi0m0, func 4 |
| MOSI / MISO | GPIO1_C2 / C3 | spi0m0, func 6 |
| RST | GPIO3_A6 | GPIO, active-low |
| BUSY | GPIO3_A7 | GPIO in |
| DIO1 | GPIO3_A3 | GPIO + IRQ |
| ANT_SW0 / ANT_SW1 | GPIO0_A3 / A4 | GPIO out |

### SPI1 — SX1262 radio 1 (RadioService)
Clocks: `PCLK_SPI1_GATE`, `CLK_SPI1_GATE`, `SCLK_IN_SPI1_GATE` (PERICRU).

| Signal | RV1106 GPIO | IOMUX |
|---|---|---|
| CS / CLK / MISO / MOSI | GPIO4_A5 / A7 / A0 / A1 | spi1m0, func 2 |
| RST | GPIO1_C6 | GPIO, active-low |
| BUSY | GPIO1_C7 | GPIO in |
| DIO1 | GPIO1_D1 | GPIO + IRQ |
| ANT_SW0 / ANT_SW1 | GPIO1_C5 / D0 | GPIO out |

Antenna RF switch (both radios, driven by GPIO, **not** the chip's DIO2):
**TX = ANT_SW0=1, ANT_SW1=0; RX = ANT_SW0=0, ANT_SW1=1.**

### I²C0 — BME280 + ICM-42670-P (SensorService)
Clocks: `PCLK_I2C0_GATE`, `CLK_I2C0_GATE` (PERICRU), ungated before any register access. Bus
at 400 kHz.

| Signal | RV1106 GPIO | IOMUX |
|---|---|---|
| SCL | GPIO1_A3 | i2c0m0, func 2 |
| SDA | GPIO1_A4 | i2c0m0, func 2 |

Slaves: **BME280 @ 0x76**, **ICM-42670-P @ 0x68**. The ICM AD0 line is pinned by a `gpio-hog`
on `&gpio0` (`GPIO0_A5` driven low → address 0x68).

## Adding or moving a peripheral

1. **Cede on Linux:** set the controller `status = "disabled"` in the DT, drop the Linux
   package, rebuild the DTB. Disable anything that collides on the freed pins.
2. **Claim on the MCU:** enable clocks → set IOMUX (same pins/function as the Linux DT) →
   drive control GPIOs → write the driver as a `SensorService`/`RadioService`-style rpmsg
   handler.

Full step-by-step: [`../migration/implementation/90-mcu-config-replication.md`](../migration/implementation/90-mcu-config-replication.md) §6.
