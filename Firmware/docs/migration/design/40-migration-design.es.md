# Diseño de la migración

> **Naturaleza del documento.** Registro de diseño — ya **implementado y
> corriendo en placa**. Se basa en los docs 10/20/30. Decidió, por driver, qué se
> reutilizó y qué se adaptó, y fijó la arquitectura Linux↔RISC-V que hoy está en
> producción. Para el sistema tal como quedó construido ver
> [../../architecture/overview.md](../../architecture/overview.md),
> [../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md) y
> [../../architecture/peripheral-ownership.md](../../architecture/peripheral-ownership.md).

## 1. Arquitectura objetivo

```
 ┌──────────────────────── Linux / Cortex-A7 ────────────────────────┐
 │  Software de misión · Telemetría · Telecomandos · CCSDS · Storage  │
 │  ───────────────────────────────────────────────────────────────  │
 │  Clientes de servicio (reemplazan a la CLI/sysfs actuales):        │
 │    radio_test · sensor_test · telemetry-client · command-client    │
 │  ───────────────────────────────────────────────────────────────  │
 │  Capa de transporte:  /dev/rpmsg* (rockchip_rpmsg)                 │
 └───────────────────────────────┬───────────────────────────────────┘
                                  │  RPMsg / Mailbox / vrings (doc 20)
 ┌───────────────────────────────┴───────────────────────────────────┐
 │  RT-Thread / RISC-V (HPMCU)                                        │
 │  Dispatcher IPC (tabla cmd→handler, patrón rpmsg_cmd)              │
 │  ───────────────────────────────────────────────────────────────  │
 │  RadioService · SensorService · TelemetryService · CommandService  │
 │  ───────────────────────────────────────────────────────────────  │
 │  Drivers portados:  sx1262 (SPI) · bme280 (I²C) · icm42670 (I²C)   │
 │  HAL Rockchip:  HAL_SPI · HAL_I2C · HAL_GPIO · HAL_PINCTRL         │
 └───────────────────────────────┬───────────────────────────────────┘
                                  │
                            Hardware (SX1262 ×2, BME280, ICM-42670)
```

**Reparto de responsabilidades:**
- **Linux:** lógica de misión, CCSDS, almacenamiento, planificación, eventos.
  **Nunca** accede directamente al hardware tras la migración.
- **RT-Thread:** control determinista de hardware (radio, SPI, I²C, GPIO, sensores).
  **No** implementa lógica de misión.

**Principio de portado (por diseño: reutilizar el máximo):** por cada driver se
conserva la **lógica de hardware portable** del doc 30 (idealmente en archivos
`*_cmd.c`/`*_regs.h` casi intactos) y se sustituye **solo** la capa de acceso (SPI/
I²C/GPIO/IRQ) y la interfaz al exterior (char dev/IIO → IPC). Patrón: una *Interfaz
de Portabilidad* fina (HAL del driver) que en Linux mapea a `spi_sync`/`regmap`/
`gpiod_*` y en RT-Thread a `HAL_SPI`/`drv_i2c`/`HAL_GPIO`. Así el mismo `*_cmd.c`
compila en ambos lados.

## 2. Transporte IPC (prerrequisito de todo)

Antes del primer driver se **cableó** RPMsg para el RV1106 (doc 20 §8); hoy está
hecho y en producción:

1. Kernel: añadir `rv1106` a la tabla de match de `rockchip_rpmsg.c`.
2. Device tree Linux: nodo `rpmsg` + `reserved-memory` (64 KB/instancia) ligado a
   `mailbox@0xff5c0000`.
3. RT-Thread: porting `platform/RV1106/` para RPMsg-Lite (IRQs y direcciones de
   vring reales del RV1106).
4. Firmware MCU mínimo: BSP `rv1106-mcu` reducido + dispatcher `rpmsg_cmd` que
   responda a un comando `PING`/`ECHO`.
5. Integrar en `build.sh` del CubeSat: target `mcu` (toolchain RISC-V + SCons) y
   empaquetar `rtthread.bin` como `LOADER2=Hpmcu` en el flujo rkbin.

**Criterio de aceptación (cumplido):** desde Linux, un mensaje enviado por
`/dev/rpmsg*` devuelve el eco del RISC-V. El servicio PingEcho está activo en el
endpoint `0x4004` (`rpmsg-ap3-ch0`). Nota sobre la forma del transporte: B2A
(MCU→A7) funciona por IRQ; A2B (A7→MCU) no es legible por IRQ desde el MCU, así
que el MCU **sondea las vrings** (ver
[../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md)).

## 3. Diseño por driver

> **Estado:** los cuatro drivers están portados y vivos en el MCU. El MCU es ahora
> dueño de **ambas radios SX1262** (SPI) y de **ambos sensores I²C0** (BME280 +
> ICM-42670); Linux los maneja mediante `radio_test` / `sensor_test`. Las tablas
> siguientes registran las decisiones tomadas.

### 3.1 SPI (infraestructura, antes del SX1262)

| Aspecto | Decisión |
|---------|----------|
| Dependencias Linux | spi core (`spi_sync`, `spi_write_then_read`). |
| Reutilizable | Las secuencias de bytes ya están en `sx1262_cmd.c` (no son de Linux). |
| Adaptación RT-Thread | Habilitar `RT_USING_SPI`; usar `drv_spi.c` + `HAL_SPI_*`. Definir bus/CS por IOMUX (`HAL_PINCTRL_SetIOMUX`). |
| Propiedad del HW | **SPI0/SPI1 reasignados del A7 al RISC-V**: `&spi0/&spi1` quitados del DT de Linux y configurados en el `board/iomux.c` del MCU. |
| Interfaz IPC | Ninguna directa: SPI es interno al firmware; lo usan los servicios. |
| Riesgo | Footprint y throughput SPI desde SCR1; verificar `HAL_SPI_ItTransfer`/DMA. |

### 3.2 SX1262 (×2) — RadioService

| Aspecto | Decisión |
|---------|----------|
| Reutilizable (alto) | `sx1262_cmd.c` y `sx1262_regs.h` casi intactos (init, calibración, Frf, modulación, IRQ). Conservar particularidades: **XTAL no TCXO**, **ANT_SW por GPIO**, erratas 0x08D8/0x0889. |
| Adaptar | `sx1262_hal.c`: `spi_*`→`HAL_SPI`, `gpiod_*`→`HAL_GPIO/rt_pin`, IRQ DIO1→`rt_pin_attach_irq` (o polling de BUSY). `sx1262_core/_chardev` se sustituyen por el servicio + dispatcher IPC. |
| API visible desde Linux | Comandos IPC del **RadioService** (reset, set_freq, set_power, set_modem, tx, rx, get_status, read/write_reg, set_antsw, get_irq) — espejo de los `SX1262_IOCTL_*` actuales (doc 50). |
| Dos instancias | Dos contextos de driver (SPI0/SPI1 + GPIOs propios), un `instance_id` en el protocolo IPC. |
| Riesgo | DIO1 como IRQ del MCU (mapear pin↔IRQ en SCR1); latencia TX/RX_DONE vía IPC para la lógica de misión. |

### 3.3 BME280 — SensorService

| Aspecto | Decisión |
|---------|----------|
| Reutilizable | Compensación Bosch + mapa de registros (portar desde `bmp280-core.c`, o reimplementar el algoritmo, que es público y autocontenido). |
| Adaptar | regmap/i2c → `drv_i2c`/`HAL_I2C`; IIO/sysfs → comandos IPC `read_sensor`. |
| API visible desde Linux | `SensorService`: `read(bme280)` → {temp_m°C, pres_kPa, hum_%RH}. |
| Bus | I²C0 (GPIO1_A3/A4) ruteado al MCU; compartido con ICM-42670 (mismo bus). |
| Riesgo | Propiedad del I²C0 entre A7 y MCU; si el bme280 es in-tree en Linux, dejar de instanciarlo en el DT de Linux al migrarlo. |

### 3.4 ICM-42670 — SensorService

| Aspecto | Decisión |
|---------|----------|
| Reutilizable | Reset/config + lectura BE + **escalado en punto fijo** (de `icm42670.c`). |
| Adaptar | regmap/i2c → `drv_i2c`/`HAL_I2C`; IIO → IPC; **AD0 gpio-hog** → fijar pin en `board/iomux.c` del MCU. |
| API visible | `SensorService`: `read(icm42670)` → {ax,ay,az, gx,gy,gz, temp}. INT1 (GPIO0_A1) opcional para modo *streaming* futuro. |
| Bus | Comparte I²C0 con BME280 → un solo driver de bus I²C en el MCU, dos dispositivos. |

## 4. Estrategia de portabilidad de código (cómo reutilizar al máximo)

Se propone, **sin reorganizar el proyecto**, añadir junto a cada
driver una capa de portabilidad fina, p.ej.:

```
src/sx1262-kmod/
  sx1262_cmd.c     sx1262_regs.h      <- PORTABLE: sin cambios o casi
  sx1262_port.h                       <- NUEVO: macros SPI/GPIO/delay/log
  sx1262_port_linux.c                 <- glue actual (extraído de _hal.c/_core.c)
  sx1262_port_rtt.c                   <- NUEVO: glue RT-Thread (HAL_SPI/GPIO)
```

`sx1262_cmd.c` llamaría a `sx_spi_xfer()`, `sx_gpio_set()`, `sx_delay_ms()` definidos
en `sx1262_port.h`, con dos implementaciones intercambiables por compilación. Mismo
patrón para BME280/ICM. Esto maximiza el código compartido y mantiene el driver Linux
funcionando durante la transición.

## 5. Caveats de propiedad de hardware (crítico)

No hay árbitro HW automático entre A7 y RISC-V para SPI/I²C/GPIO: **cada periférico
debe tener un único dueño**, fijado por configuración:
- Lo que migra al MCU se **retira del device tree de Linux** (o se marca `disabled`)
  para que Linux no lo reclame.
- Los pines/IOMUX de esos periféricos se configuran en `board/iomux.c` del firmware MCU.
- Para datos compartidos puntuales existe `HAL_SPINLOCK` (visto en `shmem_ipc_test`),
  pero el modelo principal es: hardware → RISC-V; datos → Linux vía IPC.

## 6. Decisiones tomadas (resueltas)

1. **Propiedad de SPI/I²C:** SPI0/SPI1 e I²C0 se cedieron **por completo** al
   RISC-V; Linux no tiene acceso directo, por diseño. El DT del CubeSat se editó
   en consecuencia.
2. **Alcance inicial:** el transporte IPC (PING/ECHO) se implementó y validó en
   hardware **primero**, y luego siguió el SX1262.
3. **Estrategia RPMsg:** se portó `platform/RV1106/` para RPMsg-Lite con las IRQs
   y direcciones de vring reales del RV1106.
4. **Coexistencia:** la migración avanzó driver a driver detrás de la capa de
   portabilidad (§4), no "de golpe".

El roadmap de implementación y el protocolo IPC concreto están en el doc 50; la
propiedad de periféricos tal como quedó está en
[../../architecture/peripheral-ownership.md](../../architecture/peripheral-ownership.md).
