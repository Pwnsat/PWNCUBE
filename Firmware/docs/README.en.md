# PwnCube SDK — Documentation

CubeSat target on the **Rockchip RV1106** (Cortex-A7 + SCR1 RISC-V). Start with the
architecture, then build, then the per-device and security docs.

Docs are **bilingual (EN/ES)**. Each topic is `name.en.md` + `name.es.md` with `name.md` a
symlink to the active language. Switch with `./docs/switch.sh en|es` (or no argument to
toggle).

## Start here
- [`getting-started.md`](getting-started.md) — **first time?** from zero: install dependencies, clone, build the image, and flash the board

## Architecture
- [`architecture/overview.md`](architecture/overview.md) — the two cores, roles, boot, mission
- [`architecture/ipc-rpmsg.md`](architecture/ipc-rpmsg.md) — Linux↔MCU rpmsg services & endpoints
- [`architecture/peripheral-ownership.md`](architecture/peripheral-ownership.md) — which peripherals the MCU owns, how, the pin map

## Build
- [`build/toolchain.md`](build/toolchain.md) · [`build/uboot.md`](build/uboot.md) · [`build/kernel.md`](build/kernel.md) · [`build/rootfs.md`](build/rootfs.md)
- [`build/packaging.md`](build/packaging.md) — image assembly & flashing · [`build/pkg-system.md`](build/pkg-system.md) — the `pkg/` package system

## Peripherals (MCU-owned, driven over rpmsg)
- [`peripherals/sx1262-radio.md`](peripherals/sx1262-radio.md) — dual SX1262 LoRa
- [`peripherals/bme280.md`](peripherals/bme280.md) — temperature / pressure / humidity
- [`peripherals/icm42670.md`](peripherals/icm42670.md) — 6-axis IMU
- [`peripherals/gps-neo6m.md`](peripherals/gps-neo6m.md) — real u-blox NEO-6M GPS receiver, UART0

## Security
- [`security/exploitation-guide.md`](security/exploitation-guide.md) — CCSDS telecommand attack surface, step by step

## Migration (record)
How the MCU came to own the peripherals — design decisions and on-board bring-up:
- [`migration/README.md`](migration/README.md) — index of the migration series
