# Existing CubeSat drivers — inventory for migration

> **Nature of the document.** Study of **this CubeSat repo**
> (this repository). Paths relative to this repo. Catalog the functional Linux
> drivers and separate the
> **portable hardware logic** from the Linux-specific *glue*, the basis of the
> migration. No code is modified.

Existing and complementary user documentation: `docs/sx1262.md`,
`docs/bme280.md`, `docs/icm42670.md`. Device tree: `dts/rv1106g-sdk.dts` (includes
`dts/rv1106-sdk-ipc.dtsi`). Packaging: `pkg/available/{sx1262,icm42670}`.

> **Status: the migration in this inventory is DONE and running on hardware.** The MCU
> now owns **both SX1262 radios** (SPI) and **both I²C0 sensors** (BME280 + ICM-42670);
> Linux drives them from user space via `radio_test` / `sensor_test`. This document is
> kept as the historical inventory that separated portable logic from Linux glue; the
> per-driver notes below are updated to the achieved state. For the current picture see
> [`../../architecture/peripheral-ownership.md`](../../architecture/peripheral-ownership.md)
> and [`../../architecture/overview.md`](../../architecture/overview.md).

## Criterion: portable vs. glue

- **Portable (reusable on RISC-V):** chip register sequences, state
  machines, calibration/scaling formulas, parameter encoding. They do not
  depend on the kernel.
- **Linux glue (rewritten on the MCU side):** SPI/I²C access (spi core / regmap), GPIO
  (`gpiod_*`), IRQ (`request_irq`), char device / IIO / sysfs, device tree probe,
  synchronization primitives and `devm_*`.

---

## 1. SX1262 (×2) — LoRa over SPI

**Sources:** `src/sx1262-kmod/` (module `sx1262.ko`) and `src/sx1262-cli/` (CLI).

| File | Role |
|---------|-----|
| `sx1262_core.c` (1–194) | `spi_register_driver`, char device (`alloc_chrdev_region`, `cdev_add`), probe per radio. **[glue]** |
| `sx1262_hal.c` (1–178) | SPI transfers (`spi_write`, `spi_write_then_read`, `spi_sync`), GPIO (`gpiod_*`), DIO1 IRQ, antenna switch. **[glue + pin logic]** |
| `sx1262_cmd.c` (1–769) | Command set: registers, FIFO, frequency, TX/RX, modulation, calibration, IRQ. **[mostly portable]** |
| `sx1262_chardev.c` (1–304) | `file_operations` (open/ioctl/read/write/poll), TX/RX state machine. **[glue]** |
| `sx1262.h` (1–82) | IOCTLs (`SX1262_IOCTL_*`, magic 'L'), device struct, max. 2. **[interface]** |
| `sx1262_regs.h` (1–130) | Opcodes (0x00–0x1E), register map (0x0740–0x08F9), IRQ masks. **[portable]** |
| `../sx1262-cli/sx1262_cli.c` (1–340) | CLI: opens `/dev/sx1262-{0,1}`, dispatches via ioctl/read/write. **[now the `radio_test` IPC client]** |

**Linux dependencies:** spi core (`linux/spi/spi.h`), GPIO consumer
(`linux/gpio/consumer.h`), IRQ (`devm_request_irq`, `IRQF_TRIGGER_RISING`), char
device (`cdev`, `class_create`), sync (`completion`, `mutex`, `wait_queue`),
`devm_kzalloc`, device tree (`semtech,sx1262`).

**Portable (high reuse value):** frame construction opcode+addr+data
(e.g. reading register `[0x1D, addr_hi, addr_lo, 0,0]`), FIFO sequences,
full chip initialization (`sx1262_init`: standby → regulator → packet type →
calibration → PA → frequency → TX params → modulation → IRQ), Frf conversion
(`freq*(1<<25)/32e6`), timeout encoding (15.625 µs/tick), SF/BW/CR, IRQ
parsing. **Hardware particularities already resolved** (see `docs/sx1262.md` and project
memory): **XTAL 32 MHz (NOT TCXO)** → DIO3-as-TCXO is NOT configured; **external
antenna switch via GPIO** (TX: ANT_SW0=1/ANT_SW1=0; RX: 0/1), not via DIO2; TxClamp
(0x08D8) and TxModulation (0x0889) errata.

**Glue rewritten on top of the MCU HAL:** SPI (`spi_sync` → `HAL_SPI_*`/`drv_spi`),
GPIO (`gpiod_*` → `HAL_GPIO_*`/`rt_pin_*`), DIO1 IRQ (`request_irq` → `rt_pin_attach_irq`,
per-instance DIO1 events), and the external interface (char device/ioctl → IPC commands;
now the **RadioService** endpoint `rpmsg-radio` 0x4005, driven from Linux by `radio_test`).

### Bus / pin configuration (from `dts/rv1106-sdk-ipc.dtsi`)

| Signal | SX1262 #0 (SPI0) | SX1262 #1 (SPI1) |
|-------|------------------|------------------|
| Bus / max clk | SPI0 / 8 MHz | SPI1 / 8 MHz |
| CLK/MOSI/MISO | GPIO1_C1 / C2 / C3 (SPI0m0) | GPIO4_A7 / A1 / A0 (SPI1m0) |
| CS | GPIO1_C0 | GPIO1_C4 (to be confirmed) |
| RST | GPIO3_A6 | GPIO1_C6 |
| BUSY | GPIO3_A7 | GPIO1_C7 |
| DIO1 (IRQ) | GPIO3_A3 | GPIO1_D1 |
| ANT_SW0 / SW1 | GPIO0_A3 / A4 | GPIO1_C5 / D0 |
| Pinctrl group | `sx1262_dev0_pins` | `sx1262_dev1_pins` |

Nodes: `rv1106-sdk-ipc.dtsi` ~`:256-273` (#0), ~`:275-292` (#1). The old Linux DTS
defaults are superseded by the operational PHY now running on the MCU: **uplink
918 MHz / downlink 916 MHz, LoRa SF7 / BW250 kHz / CR4-5, 20 dBm, CRC on, IQ standard,
preamble 8, sync 0x1424 (private), RX continuous (re-arms)**, IRQ TX/RX_DONE+TIMEOUT→DIO1.

> **Former migration blocker — now RESOLVED (see doc 10 §3, doc 40):** SPI used to be
> disabled on the MCU (`RT_USING_SPI` off) with SPI0 owned by Linux in the DT. SPI is now
> enabled in RT-Thread and both SPI controllers are reassigned to the RISC-V; the MCU owns
> both SX1262 radios.

---

## 2. BME280 — T/P/H over I²C

**Driver:** kernel mainline **`bmp280`** (in-tree, `CONFIG_BMP280=y`), **with no
dedicated .ko**. Sources in `src/kernel/drivers/iio/pressure/bmp280-*.c`. Config in
`configs/kernel/rv1106_minimal_defconfig`.

**Bus:** I²C0 m0 — SCL **GPIO1_A3**, SDA **GPIO1_A4**, 400 kHz, **shared with the
ICM-42670**. Address **0x76** (SDO to GND; alt. 0x77). Chip ID 0x60 @ reg 0xD0.
DT node `bme280@76` (`compatible="bosch,bme280"`), `rv1106-sdk-ipc.dtsi` ~`:320-329`.

**Linux dependencies:** i2c core, **regmap** (`regmap_*`), **IIO** framework
(`iio_dev`, channels, `read_raw`), sysfs (`/sys/bus/iio/.../in_{temp,pressure,humidityrelative}_input`).

**Portable:** WHO_AM_I, reading of calibration coefficients (non-trivial layout,
26 bytes, mix of signed/unsigned), **Bosch compensation algorithm** (fixed point),
soft reset (0xE0=0xB6) and NVM reload wait, register map (calib 0x88–0xA1,
ctrl 0xF2–0xF5, data 0xF7–0xFE).

**Glue (rewritten):** i2c_driver/probe → HAL I²C (POLL); regmap → direct access;
IIO/sysfs → exported over IPC via the **SensorService** endpoint `rpmsg-sensor` 0x4006.

> **Migrated:** the MCU has I²C (HAL + `drv_i2c`). I²C0 (the CubeSat pins) has been
> **ceded to the MCU**, which now owns the bus and drives the BME280 as an MCU-native
> driver; Linux reads it from user space via `sensor_test`.

---

## 3. ICM-42670-P — 6-axis IMU over I²C

**Sources:** `src/icm42670-kmod/icm42670.c` (1–262), module `icm42670.ko`. Custom
IIO (the mainline `inv_icm42600` is a different family). Packaging
`pkg/available/icm42670/`.

**Bus:** I²C0 m0 (same as BME280), 400 kHz. Address **0x68** (AD0 forced to 0 via
**gpio-hog** on `&gpio0`, RK_PA5; `rv1106-sdk-ipc.dtsi` ~`:349-356`). Chip ID 0x67 @
WHO_AM_I 0x75. **INT1 = GPIO0_A1** (level-high; declared in DT but unused in this
MVP). Node `icm42670@68` (`compatible="invensense,icm42670p"`) ~`:338-356`.

**Linux dependencies:** i2c core, regmap, IIO, device tree (interrupt + gpio-hog).

**Portable:** soft reset (bit4 of 0x02, 2–3 ms wait), WHO_AM_I, configuration
(PWR_MGMT0 0x1F low-noise, ACCEL_CONFIG0 0x21, GYRO_CONFIG0 0x20, 50 ms wait),
big-endian 16-bit reading (ACCEL 0x0B–0x10, GYRO 0x11–0x16, TEMP 0x09), **fixed-point
scaling** (accel 4788400 nm/s²/LSB @±16g, gyro 1065264 nrad/s/LSB @±2000 dps, temp
1000/128 m°C/LSB with offset +3200). FS ±16 g / ±2000 dps, ODR 100 Hz.

**Glue (rewritten):** i2c_driver/regmap/IIO/sysfs → HAL I²C + exported over IPC via the
**SensorService** endpoint `rpmsg-sensor` 0x4006; the register map is ported to an
MCU-native driver. AD0 is fixed low from the firmware side.

---

## 4. Summary table — portable vs glue

| Driver | Bus | Portable logic | Linux glue | On the MCU today |
|--------|-----|-----------------|------------|------------------------|
| **SX1262 ×2** | SPI 8 MHz | SPI frames, init, calibration, radio encoding, IRQ parsing | spi core, gpiod, IRQ, chardev/ioctl, DT | **MCU-owned** (SPI enabled; both radios on RISC-V; `RadioService` 0x4005) |
| **BME280** | I²C0 400 kHz | Calib + Bosch compensation, register map | i2c, regmap, IIO/sysfs, DT | **MCU-owned** (I²C0 ceded; `SensorService` 0x4006) |
| **ICM-42670** | I²C0 400 kHz | Reset/config, BE reading, fixed-point scaling | i2c, regmap, IIO/sysfs, gpio-hog, IRQ | **MCU-owned** (I²C0 ceded; `SensorService` 0x4006) |

## 5. Points to confirm

- Actual CS of SPI1 for SX1262 #1 (`GPIO1_C4` vs `GPIO4_A2`) — trace groups in
  `dts/rv1106-pinctrl.dtsi`. *(Still worth verifying against the running wiring.)*
- SX1262 DIO2/DIO3 are unused (external switch + hardwired XTAL) — confirmed in
  code; **kept in the MCU version.**
- ICM ODR 100 Hz encoding (0x09 vs 0x08 per the datasheet table).
- **RESOLVED:** I²C0 (the CubeSat pins) was assigned exclusively to the MCU; Linux no
  longer drives the bus directly and reads the sensors via `sensor_test` over IPC.
