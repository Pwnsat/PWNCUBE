# Drivers CubeSat existentes — inventario para migración

> **Naturaleza del documento.** Estudio de **este repo CubeSat**
> (este repositorio). Rutas relativas a este repo. Catalogar los drivers Linux
> funcionales y separar la
> **lógica de hardware portable** del *glue* específico de Linux, base de la
> migración. No se modifica código.

Documentación de usuario existente y complementaria: `docs/sx1262.md`,
`docs/bme280.md`, `docs/icm42670.md`. Device tree: `dts/rv1106g-sdk.dts` (incluye
`dts/rv1106-sdk-ipc.dtsi`). Empaquetado: `pkg/available/{sx1262,icm42670}`.

> **Estado: la migración de este inventario está HECHA y corriendo en hardware.** El MCU
> ahora posee **ambas radios SX1262** (SPI) y **ambos sensores I²C0** (BME280 + ICM-42670);
> Linux los maneja desde espacio de usuario vía `radio_test` / `sensor_test`. Este
> documento se conserva como el inventario histórico que separó la lógica portable del
> glue de Linux; las notas por driver de abajo están actualizadas al estado alcanzado. Para
> la foto actual ver
> [`../../architecture/peripheral-ownership.md`](../../architecture/peripheral-ownership.md)
> y [`../../architecture/overview.md`](../../architecture/overview.md).

## Criterio: portable vs. glue

- **Portable (reutilizable en RISC-V):** secuencias de registros del chip, máquinas
  de estado, fórmulas de calibración/escalado, codificación de parámetros. No
  dependen del kernel.
- **Glue de Linux (reescrito en el lado MCU):** acceso SPI/I²C (spi core / regmap), GPIO
  (`gpiod_*`), IRQ (`request_irq`), char device / IIO / sysfs, device tree probe,
  primitivas de sincronización y `devm_*`.

---

## 1. SX1262 (×2) — LoRa sobre SPI

**Fuentes:** `src/sx1262-kmod/` (módulo `sx1262.ko`) y `src/sx1262-cli/` (CLI).

| Archivo | Rol |
|---------|-----|
| `sx1262_core.c` (1–194) | `spi_register_driver`, char device (`alloc_chrdev_region`, `cdev_add`), probe por radio. **[glue]** |
| `sx1262_hal.c` (1–178) | Transferencias SPI (`spi_write`, `spi_write_then_read`, `spi_sync`), GPIO (`gpiod_*`), IRQ DIO1, switch de antena. **[glue + lógica de pines]** |
| `sx1262_cmd.c` (1–769) | Set de comandos: registros, FIFO, frecuencia, TX/RX, modulación, calibración, IRQ. **[mayormente portable]** |
| `sx1262_chardev.c` (1–304) | `file_operations` (open/ioctl/read/write/poll), máquina TX/RX. **[glue]** |
| `sx1262.h` (1–82) | IOCTLs (`SX1262_IOCTL_*`, magic 'L'), struct de dispositivo, máx. 2. **[interfaz]** |
| `sx1262_regs.h` (1–130) | Opcodes (0x00–0x1E), mapa de registros (0x0740–0x08F9), máscaras IRQ. **[portable]** |
| `../sx1262-cli/sx1262_cli.c` (1–340) | CLI: abre `/dev/sx1262-{0,1}`, despacha por ioctl/read/write. **[ahora el cliente IPC `radio_test`]** |

**Dependencias Linux:** spi core (`linux/spi/spi.h`), GPIO consumer
(`linux/gpio/consumer.h`), IRQ (`devm_request_irq`, `IRQF_TRIGGER_RISING`), char
device (`cdev`, `class_create`), sync (`completion`, `mutex`, `wait_queue`),
`devm_kzalloc`, device tree (`semtech,sx1262`).

**Portable (alto valor de reutilización):** construcción de tramas opcode+addr+datos
(p.ej. leer registro `[0x1D, addr_hi, addr_lo, 0,0]`), secuencias FIFO,
inicialización completa del chip (`sx1262_init`: standby → regulador → packet type →
calibración → PA → frecuencia → TX params → modulación → IRQ), conversión Frf
(`freq*(1<<25)/32e6`), codificación de timeouts (15.625 µs/tick), SF/BW/CR, parsing
de IRQ. **Particularidades de hardware ya resueltas** (ver `docs/sx1262.md` y memoria
del proyecto): **XTAL 32 MHz (NO TCXO)** → DIO3-as-TCXO NO se configura; **switch de
antena externo por GPIO** (TX: ANT_SW0=1/ANT_SW1=0; RX: 0/1), no por DIO2; erratas
TxClamp (0x08D8) y TxModulation (0x0889).

**Glue reescrito sobre la HAL del MCU:** SPI (`spi_sync` → `HAL_SPI_*`/`drv_spi`),
GPIO (`gpiod_*` → `HAL_GPIO_*`/`rt_pin_*`), IRQ DIO1 (`request_irq` → `rt_pin_attach_irq`,
eventos DIO1 por instancia), e interfaz al exterior (char device/ioctl → comandos IPC;
ahora el endpoint **RadioService** `rpmsg-radio` 0x4005, manejado desde Linux por `radio_test`).

### Configuración de bus / pines (de `dts/rv1106-sdk-ipc.dtsi`)

| Señal | SX1262 #0 (SPI0) | SX1262 #1 (SPI1) |
|-------|------------------|------------------|
| Bus / clk máx | SPI0 / 8 MHz | SPI1 / 8 MHz |
| CLK/MOSI/MISO | GPIO1_C1 / C2 / C3 (SPI0m0) | GPIO4_A7 / A1 / A0 (SPI1m0) |
| CS | GPIO1_C0 | GPIO1_C4 (a confirmar) |
| RST | GPIO3_A6 | GPIO1_C6 |
| BUSY | GPIO3_A7 | GPIO1_C7 |
| DIO1 (IRQ) | GPIO3_A3 | GPIO1_D1 |
| ANT_SW0 / SW1 | GPIO0_A3 / A4 | GPIO1_C5 / D0 |
| Pinctrl group | `sx1262_dev0_pins` | `sx1262_dev1_pins` |

Nodos: `rv1106-sdk-ipc.dtsi` ~`:256-273` (#0), ~`:275-292` (#1). Los antiguos defaults del
DTS de Linux quedan superados por el PHY operacional que corre ahora en el MCU: **uplink
918 MHz / downlink 916 MHz, LoRa SF7 / BW250 kHz / CR4-5, 20 dBm, CRC on, IQ estándar,
preámbulo 8, sync 0x1424 (privado), RX continuo (se re-arma)**, IRQ TX/RX_DONE+TIMEOUT→DIO1.

> **Antiguo bloqueante de migración — ahora RESUELTO (ver doc 10 §3, doc 40):** SPI solía
> estar deshabilitado en el MCU (`RT_USING_SPI` off) con SPI0 en manos de Linux en el DT.
> SPI ya está habilitado en RT-Thread y ambos controladores SPI reasignados al RISC-V; el
> MCU posee ambas radios SX1262.

---

## 2. BME280 — T/P/H sobre I²C

**Driver:** mainline **`bmp280`** del kernel (in-tree, `CONFIG_BMP280=y`), **sin .ko
propio**. Fuentes en `src/kernel/drivers/iio/pressure/bmp280-*.c`. Config en
`configs/kernel/rv1106_minimal_defconfig`.

**Bus:** I²C0 m0 — SCL **GPIO1_A3**, SDA **GPIO1_A4**, 400 kHz, **compartido con el
ICM-42670**. Dirección **0x76** (SDO a GND; alt. 0x77). Chip ID 0x60 @ reg 0xD0.
Nodo DT `bme280@76` (`compatible="bosch,bme280"`), `rv1106-sdk-ipc.dtsi` ~`:320-329`.

**Dependencias Linux:** i2c core, **regmap** (`regmap_*`), framework **IIO**
(`iio_dev`, canales, `read_raw`), sysfs (`/sys/bus/iio/.../in_{temp,pressure,humidityrelative}_input`).

**Portable:** WHO_AM_I, lectura de coeficientes de calibración (layout no trivial,
26 bytes, mezcla signed/unsigned), **algoritmo de compensación Bosch** (punto fijo),
soft reset (0xE0=0xB6) y espera de recarga NVM, mapa de registros (calib 0x88–0xA1,
ctrl 0xF2–0xF5, datos 0xF7–0xFE).

**Glue (reescrito):** i2c_driver/probe → I²C de la HAL (POLL); regmap → acceso directo;
IIO/sysfs → exportado por IPC vía el endpoint **SensorService** `rpmsg-sensor` 0x4006.

> **Migrado:** el MCU tiene I²C (HAL + `drv_i2c`). I²C0 (los pines del CubeSat) se ha
> **cedido al MCU**, que ahora posee el bus y maneja el BME280 como driver nativo del MCU;
> Linux lo lee desde espacio de usuario vía `sensor_test`.

---

## 3. ICM-42670-P — IMU 6 ejes sobre I²C

**Fuentes:** `src/icm42670-kmod/icm42670.c` (1–262), módulo `icm42670.ko`. IIO
custom (el mainline `inv_icm42600` es otra familia). Empaquetado
`pkg/available/icm42670/`.

**Bus:** I²C0 m0 (mismo que BME280), 400 kHz. Dirección **0x68** (AD0 forzado a 0 vía
**gpio-hog** en `&gpio0`, RK_PA5; `rv1106-sdk-ipc.dtsi` ~`:349-356`). Chip ID 0x67 @
WHO_AM_I 0x75. **INT1 = GPIO0_A1** (level-high; declarado en DT pero sin uso en este
MVP). Nodo `icm42670@68` (`compatible="invensense,icm42670p"`) ~`:338-356`.

**Dependencias Linux:** i2c core, regmap, IIO, device tree (interrupt + gpio-hog).

**Portable:** soft reset (bit4 de 0x02, espera 2–3 ms), WHO_AM_I, configuración
(PWR_MGMT0 0x1F low-noise, ACCEL_CONFIG0 0x21, GYRO_CONFIG0 0x20, espera 50 ms),
lectura big-endian 16-bit (ACCEL 0x0B–0x10, GYRO 0x11–0x16, TEMP 0x09), **escalado en
punto fijo** (accel 4788400 nm/s²/LSB @±16g, gyro 1065264 nrad/s/LSB @±2000 dps, temp
1000/128 m°C/LSB con offset +3200). FS ±16 g / ±2000 dps, ODR 100 Hz.

**Glue (reescrito):** i2c_driver/regmap/IIO/sysfs → I²C de la HAL + exportado por IPC vía
el endpoint **SensorService** `rpmsg-sensor` 0x4006; el mapa de registros está portado a un
driver nativo del MCU. AD0 se fija a 0 desde el lado del firmware.

---

## 4. Tabla resumen — portable vs glue

| Driver | Bus | Lógica portable | Glue Linux | En el MCU hoy |
|--------|-----|-----------------|------------|------------------------|
| **SX1262 ×2** | SPI 8 MHz | Tramas SPI, init, calibración, codificación radio, parsing IRQ | spi core, gpiod, IRQ, chardev/ioctl, DT | **Propiedad del MCU** (SPI habilitado; ambas radios en RISC-V; `RadioService` 0x4005) |
| **BME280** | I²C0 400 kHz | Calib + compensación Bosch, mapa de registros | i2c, regmap, IIO/sysfs, DT | **Propiedad del MCU** (I²C0 cedido; `SensorService` 0x4006) |
| **ICM-42670** | I²C0 400 kHz | Reset/config, lectura BE, escalado punto fijo | i2c, regmap, IIO/sysfs, gpio-hog, IRQ | **Propiedad del MCU** (I²C0 cedido; `SensorService` 0x4006) |

## 5. Puntos a confirmar

- CS real de SPI1 del SX1262 #1 (`GPIO1_C4` vs `GPIO4_A2`) — trazar grupos en
  `dts/rv1106-pinctrl.dtsi`. *(Sigue valiendo la pena verificarlo contra el cableado real.)*
- DIO2/DIO3 del SX1262 no se usan (switch externo + XTAL hardwired) — confirmado en
  código; **mantenido en la versión MCU.**
- Codificación ODR 100 Hz del ICM (0x09 vs 0x08 según tabla del datasheet).
- **RESUELTO:** I²C0 (pines del CubeSat) se asignó en exclusiva al MCU; Linux ya no maneja
  el bus directamente y lee los sensores vía `sensor_test` por IPC.
