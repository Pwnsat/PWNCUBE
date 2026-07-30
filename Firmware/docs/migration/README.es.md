# Migración a RISC-V — Documentación

> **Bilingüe (EN/ES).** Cada documento existe como `*.en.md` y `*.es.md`; el
> enlace `*.md` apunta al idioma activo. Cambia con `./docs/switch.sh en|es`
> (o sin argumento para alternar). Idioma activo actual: **Español**.

Diseño **e implementación** de la migración de los drivers de la computadora de
vuelo CubeSat desde Linux (Cortex-A7) al coprocesador RISC-V **Syntacore SCR1**
("HPMCU", RT-Thread), sobre IPC rpmsg. Cubre el estudio del proyecto y del SDK
Luckfox como referencia, el diseño de la migración por driver, y la
implementación verificada en placa.

> **Esta es la serie histórica de la migración. La migración está COMPLETA y
> corriendo en hardware.** Para la arquitectura del estado actual ver
> [`../architecture/overview.md`](../architecture/overview.md),
> [`../architecture/ipc-rpmsg.md`](../architecture/ipc-rpmsg.md) y
> [`../architecture/peripheral-ownership.md`](../architecture/peripheral-ownership.md).

**Estado:** arranque dual A7+MCU **FUNCIONANDO**. El MCU posee ambas radios
SX1262 (SPI) y ambos sensores I²C0, expuestos a Linux por cinco endpoints
rpmsg — **PingEcho** (`rpmsg-ap3-ch0`, 0x4004), **RadioService**
(`rpmsg-radio`, 0x4005), **SensorService** (`rpmsg-sensor`, 0x4006),
**TelemetryService** (`rpmsg-telemetry`, 0x4007) y **CommandService**
(`rpmsg-command`, 0x4008). Todos **validados en placa**; los servicios
coexisten en el mismo hilo de poll. Linux los maneja vía `radio_test` /
`sensor_test`.

## Separación de proyectos

| Proyecto | Ubicación | Rol |
|----------|-----------|-----|
| **CubeSat** | este repositorio | Firmware de la computadora de vuelo. Es lo que se desarrolla. |
| **SDK Luckfox** | referencia externa (fuera de este repo) | **Solo referencia técnica** del RV1106. No se modifica. |

Citas `sysdrv/...`, `project/...` → **SDK Luckfox**. Citas `src/...`, `dts/...`,
`pkg/...` → **este repo CubeSat**.

## Estructura de la documentación

### `reference/` — Estudio del SDK Luckfox (referencia técnica)
| # | Documento | Contenido |
|---|-----------|-----------|
| 10 | [`luckfox-riscv-boot`](reference/10-luckfox-riscv-boot.md) | Cómo Luckfox compila, arranca, carga y mapea en memoria el firmware RISC-V (HPMCU / RT-Thread). |
| 20 | [`luckfox-ipc`](reference/20-luckfox-ipc.md) | Implementación real de IPC ARM↔RISC-V: RPMsg-Lite, Mailbox, memoria compartida, vrings, interrupciones. |

### `design/` — Diseño de la migración
| # | Documento | Contenido |
|---|-----------|-----------|
| 30 | [`cubesat-drivers`](design/30-cubesat-drivers.md) | Inventario de los drivers Linux: lógica portable vs. *glue* de Linux, buses, GPIO. |
| 40 | [`migration-design`](design/40-migration-design.md) | Diseño de la migración por driver y arquitectura objetivo Linux↔RISC-V. |
| 50 | [`ipc-protocol-roadmap`](design/50-ipc-protocol-roadmap.md) | Protocolo IPC versionado, servicios, CCSDS y orden de implementación. |

### `implementation/` — Implementación y validación en placa
| # | Documento | Contenido |
|---|-----------|-----------|
| 60 | [`ipc-bringup`](implementation/60-ipc-bringup.md) | Estado del transporte IPC: hecho/verificado vs. pendiente, mapa de memoria y co-diseño. |
| 70 | [`mailbox-loopback-test`](implementation/70-mailbox-loopback-test.md) | Test de loopback del mailbox: B2A funciona, A2B ilegible por el MCU, shared-memory DDR coherente. |
| 80 | [`dual-boot`](implementation/80-dual-boot.md) | **Arranque dual FUNCIONANDO**: mapa de memoria definitivo, la cadena de 5 bugs y sus resoluciones, flasheo correcto (UF, nunca `DI -b`), diagnóstico por marcadores. |
| 90 | [`mcu-config-replication`](implementation/90-mcu-config-replication.md) | **GUÍA DEFINITIVA** de configuración del MCU (registro a registro), reglas de memoria, BSP, transporte rpmsg, patrón de periféricos y driver compartido, **RadioService** (§7bis) y **SensorService** (§7ter), ruteo de endpoints, build/flash y checklist de replicación. |

**Ruta de lectura sugerida:** empieza por el 90 (guía de replicación) si vas a
tocar el MCU; el 80 para entender el arranque dual; 10/20 como referencia del
SoC; 40/50 para el diseño.

## Resumen ejecutivo — restricciones que condicionaron el diseño

Cuatro hechos verificados en el código, base de la migración (detalle en 10/20):

1. **El RISC-V es un Syntacore SCR1, `rv32imc`/`ilp32`.** Toolchain
   `riscv-none-embed-gcc 10.2.0`. El artefacto es `rtthread.bin`, cargado en
   `0x40000`.
2. **Memoria muy ajustada.** La RAM del MCU son **240 KB** (`link.lds`
   `ORIGIN=0x40000, LENGTH=0x3c000`); la SRAM compartida del HPMCU son **8 KB**
   (`hpmcu_sram@0xff6fe000`). Todo driver migrado cabe en este presupuesto.
3. **SPI/I²C en el MCU son solo HAL, no cableados por el BSP.** Se manejan con
   **HAL directo** (PIO/POLL), sin el framework de drivers de RT-Thread, y se
   cede el bus desde Linux en el device tree. *(Hecho: buses cedidos, drivers
   MCU-native.)*
4. **El stack de IPC existía pero NO estaba cableado para el RV1106** (match del
   driver rpmsg, porting, device tree). *(Hecho: transporte rpmsg funcionando
   por poll de vrings + ACK ciego del mailbox.)*
