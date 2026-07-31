# Migration design

> **Nature of the document.** Design record — now **implemented and running on
> hardware**. Based on docs 10/20/30. Decided, per driver, what was reused and
> what was adapted, and settled the Linux↔RISC-V architecture that is now in place.
> For the as-built system see [../../architecture/overview.md](../../architecture/overview.md),
> [../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md) and
> [../../architecture/peripheral-ownership.md](../../architecture/peripheral-ownership.md).

## 1. Target architecture

```
 ┌──────────────────────── Linux / Cortex-A7 ────────────────────────┐
 │  Mission software · Telemetry · Telecommands · CCSDS · Storage     │
 │  ───────────────────────────────────────────────────────────────  │
 │  Service clients (replace the current CLI/sysfs):                 │
 │    radio_test · sensor_test · telemetry-client · command-client    │
 │  ───────────────────────────────────────────────────────────────  │
 │  Transport layer:  /dev/rpmsg* (rockchip_rpmsg)                    │
 └───────────────────────────────┬───────────────────────────────────┘
                                  │  RPMsg / Mailbox / vrings (doc 20)
 ┌───────────────────────────────┴───────────────────────────────────┐
 │  RT-Thread / RISC-V (HPMCU)                                        │
 │  IPC dispatcher (cmd→handler table, rpmsg_cmd pattern)             │
 │  ───────────────────────────────────────────────────────────────  │
 │  RadioService · SensorService · TelemetryService · CommandService  │
 │  ───────────────────────────────────────────────────────────────  │
 │  Ported drivers:  sx1262 (SPI) · bme280 (I²C) · icm42670 (I²C)     │
 │  Rockchip HAL:  HAL_SPI · HAL_I2C · HAL_GPIO · HAL_PINCTRL         │
 └───────────────────────────────┬───────────────────────────────────┘
                                  │
                            Hardware (SX1262 ×2, BME280, ICM-42670)
```

**Distribution of responsibilities:**
- **Linux:** mission logic, CCSDS, storage, scheduling, events.
  **Never** accesses the hardware directly after the migration.
- **RT-Thread:** deterministic hardware control (radio, SPI, I²C, GPIO, sensors).
  Does **not** implement mission logic.

**Porting principle (by design: reuse the maximum):** for each driver the
**portable hardware logic** from doc 30 is kept (ideally in files
`*_cmd.c`/`*_regs.h` almost untouched) and **only** the access layer (SPI/
I²C/GPIO/IRQ) and the external interface (char dev/IIO → IPC) are replaced. Pattern: a thin *Portability
Interface* (driver HAL) that on Linux maps to `spi_sync`/`regmap`/
`gpiod_*` and on RT-Thread to `HAL_SPI`/`drv_i2c`/`HAL_GPIO`. Thus the same `*_cmd.c`
compiles on both sides.

## 2. IPC transport (prerequisite for everything)

Before the first driver, RPMsg was **wired** for the RV1106 (doc 20 §8), which is
now done and in production:

1. Kernel: add `rv1106` to the match table of `rockchip_rpmsg.c`.
2. Linux device tree: `rpmsg` node + `reserved-memory` (64 KB/instance) tied to
   `mailbox@0xff5c0000`.
3. RT-Thread: port `platform/RV1106/` for RPMsg-Lite (real IRQs and vring
   addresses of the RV1106).
4. Minimal MCU firmware: reduced `rv1106-mcu` BSP + `rpmsg_cmd` dispatcher that
   responds to a `PING`/`ECHO` command.
5. Integrate into the CubeSat `build.sh`: `mcu` target (RISC-V toolchain + SCons) and
   package `rtthread.bin` as `LOADER2=Hpmcu` in the rkbin flow.

**Acceptance criterion (met):** from Linux, a message sent over `/dev/rpmsg*`
returns the echo from the RISC-V. The PingEcho service is live on endpoint
`0x4004` (`rpmsg-ap3-ch0`). Note on transport shape: B2A (MCU→A7) works over IRQ;
A2B (A7→MCU) is not IRQ-readable by the MCU, so the MCU **polls the vrings**
(see [../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md)).

## 3. Design per driver

> **Status:** all four drivers are ported and live on the MCU. The MCU now owns
> **both SX1262 radios** (SPI) and **both I²C0 sensors** (BME280 + ICM-42670);
> Linux drives them through `radio_test` / `sensor_test`. The tables below record
> the decisions taken.

### 3.1 SPI (infrastructure, before the SX1262)

| Aspect | Decision |
|---------|----------|
| Linux dependencies | spi core (`spi_sync`, `spi_write_then_read`). |
| Reusable | The byte sequences are already in `sx1262_cmd.c` (they are not Linux-specific). |
| RT-Thread adaptation | Enable `RT_USING_SPI`; use `drv_spi.c` + `HAL_SPI_*`. Define bus/CS via IOMUX (`HAL_PINCTRL_SetIOMUX`). |
| HW ownership | **SPI0/SPI1 reassigned from the A7 to the RISC-V**: `&spi0/&spi1` removed from the Linux DT and configured in the MCU `board/iomux.c`. |
| IPC interface | None direct: SPI is internal to the firmware; the services use it. |
| Risk | SPI footprint and throughput from SCR1; verify `HAL_SPI_ItTransfer`/DMA. |

### 3.2 SX1262 (×2) — RadioService

| Aspect | Decision |
|---------|----------|
| Reusable (high) | `sx1262_cmd.c` and `sx1262_regs.h` almost untouched (init, calibration, Frf, modulation, IRQ). Keep the particularities: **XTAL not TCXO**, **ANT_SW via GPIO**, 0x08D8/0x0889 errata. |
| Adapt | `sx1262_hal.c`: `spi_*`→`HAL_SPI`, `gpiod_*`→`HAL_GPIO/rt_pin`, DIO1 IRQ→`rt_pin_attach_irq` (or BUSY polling). `sx1262_core/_chardev` are replaced by the service + IPC dispatcher. |
| API visible from Linux | IPC commands of the **RadioService** (reset, set_freq, set_power, set_modem, tx, rx, get_status, read/write_reg, set_antsw, get_irq) — mirror of the current `SX1262_IOCTL_*` (doc 50). |
| Two instances | Two driver contexts (SPI0/SPI1 + their own GPIOs), one `instance_id` in the IPC protocol. |
| Risk | DIO1 as MCU IRQ (map pin↔IRQ in SCR1); TX/RX_DONE latency via IPC for the mission logic. |

### 3.3 BME280 — SensorService

| Aspect | Decision |
|---------|----------|
| Reusable | Bosch compensation + register map (port from `bmp280-core.c`, or reimplement the algorithm, which is public and self-contained). |
| Adapt | regmap/i2c → `drv_i2c`/`HAL_I2C`; IIO/sysfs → IPC commands `read_sensor`. |
| API visible from Linux | `SensorService`: `read(bme280)` → {temp_m°C, pres_kPa, hum_%RH}. |
| Bus | I²C0 (GPIO1_A3/A4) routed to the MCU; shared with ICM-42670 (same bus). |
| Risk | I²C0 ownership between A7 and MCU; if the bme280 is in-tree on Linux, stop instantiating it in the Linux DT once migrated. |

### 3.4 ICM-42670 — SensorService

| Aspect | Decision |
|---------|----------|
| Reusable | Reset/config + BE reading + **fixed-point scaling** (from `icm42670.c`). |
| Adapt | regmap/i2c → `drv_i2c`/`HAL_I2C`; IIO → IPC; **AD0 gpio-hog** → pin it in the MCU `board/iomux.c`. |
| API visible | `SensorService`: `read(icm42670)` → {ax,ay,az, gx,gy,gz, temp}. INT1 (GPIO0_A1) optional for a future *streaming* mode. |
| Bus | Shares I²C0 with BME280 → a single I²C bus driver on the MCU, two devices. |

## 4. Code portability strategy (how to reuse the maximum)

The proposal, **without reorganizing the project**, is to add next to each
driver a thin portability layer, e.g.:

```
src/sx1262-kmod/
  sx1262_cmd.c     sx1262_regs.h      <- PORTABLE: no changes, or nearly none
  sx1262_port.h                       <- NEW: SPI/GPIO/delay/log macros
  sx1262_port_linux.c                 <- current glue (extracted from _hal.c/_core.c)
  sx1262_port_rtt.c                   <- NEW: RT-Thread glue (HAL_SPI/GPIO)
```

`sx1262_cmd.c` would call `sx_spi_xfer()`, `sx_gpio_set()`, `sx_delay_ms()` defined
in `sx1262_port.h`, with two implementations interchangeable at compile time. Same
pattern for BME280/ICM. This maximizes shared code and keeps the Linux driver
working during the transition.

## 5. Hardware ownership caveats (critical)

There is no automatic HW arbiter between the A7 and RISC-V for SPI/I²C/GPIO: **each peripheral
must have a single owner**, enforced by configuration:
- What migrates to the MCU is **removed from the Linux device tree** (or marked `disabled`)
  so that Linux does not claim it.
- The pins/IOMUX of those peripherals are configured in the MCU firmware `board/iomux.c`.
- For occasional shared data there is `HAL_SPINLOCK` (seen in `shmem_ipc_test`),
  but the main model is: hardware → RISC-V; data → Linux via IPC.

## 6. Decisions taken (resolved)

1. **SPI/I²C ownership:** SPI0/SPI1 and I²C0 are ceded **entirely** to the RISC-V;
   Linux has no direct access, by design. The CubeSat DT was edited accordingly.
2. **Initial scope:** the IPC transport (PING/ECHO) was implemented and validated
   in hardware **first**, then the SX1262 followed.
3. **RPMsg strategy:** `platform/RV1106/` was ported for RPMsg-Lite with the real
   RV1106 IRQs and vring addresses.
4. **Coexistence:** the migration proceeded driver-by-driver behind the
   portability layer (§4) rather than all at once.

The implementation roadmap and the concrete IPC protocol are in doc 50; the
as-built peripheral ownership is in
[../../architecture/peripheral-ownership.md](../../architecture/peripheral-ownership.md).
