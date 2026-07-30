# RISC-V Migration — Documentation

> **Bilingual (EN/ES).** Every document exists as `*.en.md` and `*.es.md`; the
> `*.md` link points to the active language. Switch with `./docs/switch.sh en|es`
> (or no argument to toggle). Current active language: **Spanish** (`.md` → `.es.md`).

Design **and implementation** of migrating the CubeSat flight-computer drivers
from Linux (Cortex-A7) to the **Syntacore SCR1** RISC-V coprocessor ("HPMCU",
RT-Thread), over rpmsg IPC. Covers the study of the project and of the Luckfox
SDK as a reference, the per-driver migration design, and the on-board verified
implementation.

> **This is the historical migration series. The migration is COMPLETE and
> running on hardware.** For the current-state architecture see
> [`../architecture/overview.md`](../architecture/overview.md),
> [`../architecture/ipc-rpmsg.md`](../architecture/ipc-rpmsg.md) and
> [`../architecture/peripheral-ownership.md`](../architecture/peripheral-ownership.md).

**Status:** dual boot A7+MCU **WORKING**. The MCU owns both SX1262 radios (SPI)
and both I²C0 sensors, exposed to Linux over five rpmsg endpoints —
**PingEcho** (`rpmsg-ap3-ch0`, 0x4004), **RadioService** (`rpmsg-radio`,
0x4005), **SensorService** (`rpmsg-sensor`, 0x4006), **TelemetryService**
(`rpmsg-telemetry`, 0x4007) and **CommandService** (`rpmsg-command`, 0x4008).
All **validated on hardware**; the services coexist on the same poll thread.
Linux drives them via `radio_test` / `sensor_test`.

## Project separation

| Project | Location | Role |
|---------|----------|------|
| **CubeSat** | this repository | Flight-computer firmware. This is what is developed. |
| **Luckfox SDK** | external reference (outside this repo) | **Technical reference only** for the RV1106. Never modified. |

Citations `sysdrv/...`, `project/...` → **Luckfox SDK**. Citations `src/...`,
`dts/...`, `pkg/...` → **this CubeSat repo**.

## Documentation structure

### `reference/` — Luckfox SDK study (technical reference)
| # | Document | Content |
|---|----------|---------|
| 10 | [`luckfox-riscv-boot`](reference/10-luckfox-riscv-boot.md) | How Luckfox builds, boots, loads and memory-maps the RISC-V firmware (HPMCU / RT-Thread). |
| 20 | [`luckfox-ipc`](reference/20-luckfox-ipc.md) | The real ARM↔RISC-V IPC implementation: RPMsg-Lite, Mailbox, shared memory, vrings, interrupts. |

### `design/` — Migration design
| # | Document | Content |
|---|----------|---------|
| 30 | [`cubesat-drivers`](design/30-cubesat-drivers.md) | Inventory of the Linux drivers: portable logic vs. Linux *glue*, buses, GPIO. |
| 40 | [`migration-design`](design/40-migration-design.md) | Per-driver migration design and target Linux↔RISC-V architecture. |
| 50 | [`ipc-protocol-roadmap`](design/50-ipc-protocol-roadmap.md) | Versioned IPC protocol, services, CCSDS and the implementation order. |

### `implementation/` — Implementation and on-board validation
| # | Document | Content |
|---|----------|---------|
| 60 | [`ipc-bringup`](implementation/60-ipc-bringup.md) | IPC transport status: done/verified vs. pending, memory map and co-design. |
| 70 | [`mailbox-loopback-test`](implementation/70-mailbox-loopback-test.md) | Mailbox loopback test: B2A works, A2B unreadable by the MCU, DDR shared-memory coherent. |
| 80 | [`dual-boot`](implementation/80-dual-boot.md) | **Dual boot WORKING**: definitive memory map, the 5-bug chain and their resolutions, correct flashing (UF, never `DI -b`), marker diagnostics. |
| 90 | [`mcu-config-replication`](implementation/90-mcu-config-replication.md) | **DEFINITIVE GUIDE** to MCU configuration (register by register), memory rules, BSP, rpmsg transport, the peripheral + shared-driver patterns, **RadioService** (§7bis) and **SensorService** (§7ter), endpoint routing, build/flash and the replication checklist. |

**Suggested reading path:** start with 90 (replication guide) if you'll touch
the MCU; 80 to understand dual boot; 10/20 as SoC reference; 40/50 for the design.

## Executive summary — constraints that shaped the design

Four facts verified in the code, the foundation of the migration (detail in 10/20):

1. **The RISC-V is a Syntacore SCR1, `rv32imc`/`ilp32`.** Toolchain
   `riscv-none-embed-gcc 10.2.0`. The artifact is `rtthread.bin`, loaded at
   `0x40000`.
2. **Very tight memory.** The MCU RAM is **240 KB** (`link.lds`
   `ORIGIN=0x40000, LENGTH=0x3c000`); the HPMCU shared SRAM is **8 KB**
   (`hpmcu_sram@0xff6fe000`). Every migrated driver must fit this budget.
3. **SPI/I²C on the MCU are HAL-only, not wired by the BSP.** They are driven
   with **direct HAL** (PIO/POLL), without the RT-Thread driver framework, and
   the bus is ceded from Linux in the device tree. *(Done: buses ceded, drivers
   MCU-native.)*
4. **The IPC stack existed but was NOT wired for the RV1106** (rpmsg driver
   match, porting, device tree). *(Done: rpmsg transport working via vring
   polling + blind mailbox ACK.)*
