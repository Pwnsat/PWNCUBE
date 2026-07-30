# Propiedad de periféricos — MCU RISC-V vs Linux

El RV1106 corre **dos núcleos**: un Cortex-A7 (Linux) y un SCR1 (RISC-V, RT-Thread). Varios
periféricos los **posee el MCU**, no Linux. Este documento es la referencia única de *qué*
periféricos, *cómo* se aplica el reparto y el mapa exacto de pines/buses.

Ver también: [`overview.md`](overview.md) (roles de los núcleos), [`ipc-rpmsg.md`](ipc-rpmsg.md)
(los servicios), [`../build/kernel.md`](../build/kernel.md) (cambios de DTS).

## Quién posee qué

| Periférico | Bus / dirección | Dueño | Servicio rpmsg (endpoint) | Device tree de Linux |
|---|---|---|---|---|
| SX1262 radio 0 | SPI0 | **MCU** | RadioService (`rpmsg-radio`, 0x4005) | `&spi0 { status = "disabled"; }` |
| SX1262 radio 1 | SPI1 | **MCU** | RadioService (`rpmsg-radio`, 0x4005) | `&spi1 { status = "disabled"; }` |
| BME280 | I²C0 @ 0x76 | **MCU** | SensorService (`rpmsg-sensor`, 0x4006) | `&i2c0 { status = "disabled"; }` |
| ICM-42670-P | I²C0 @ 0x68 | **MCU** | SensorService (`rpmsg-sensor`, 0x4006) | `&i2c0 { status = "disabled"; }` |
| Uplink / downlink CCSDS | vía los radios | **MCU** | CommandService (0x4008) / TelemetryService (0x4007) | — |

Linux maneja todo esto indirectamente, por rpmsg, mediante los clientes de Linux
`src/radio-client/` (`radio_test`) y `src/sensor-client/` (`sensor_test`).

## El mecanismo — cómo se mantiene a Linux fuera del bus

El reparto tiene **dos mitades complementarias** que deben coincidir en los pines exactos:

1. **Linux cede el controlador.** En el device tree (`dts/rv1106-sdk-ipc.dtsi`) el nodo del
   controlador se pone en `status = "disabled"` y se deja fuera el driver/paquete de Linux
   (`pkg/package-config`). Recompilar el DTB libera los pines. Los periféricos que colisionan
   en esos pines también se deshabilitan (`&uart3/4/5`, varios `&pwm*`).
2. **El MCU reclama el periférico en runtime** (el MCU *no* tiene device tree): habilita los
   clocks (`HAL_CRU_ClkEnable`) → programa el IOMUX (`HAL_PINCTRL_SetIOMUX`, con el mismo
   pin/función que habría usado el DT de Linux) → maneja los GPIOs de control.

> **⚠️ Convención, no aislamiento por hardware.** Este reparto se aplica *solo* por el nodo
> DT deshabilitado más el MCU agarrando el IOMUX primero. **No** hay `reserved-memory`, bus
> firewall ni reserva de periférico por remoteproc para SPI/I²C/GPIO. Nada impide físicamente
> que Linux reactive un nodo de controlador; la corrección depende de (a) mantener el nodo DT
> deshabilitado y (b) que ambos lados coincidan en los pines/funciones exactos. Los nodos hijo
> que quedan bajo el `&i2c0`/`&spi0` deshabilitado en el DTS (`bme280@76`, `icm42670@68`,
> `sx1262@0`) se conservan **solo como documentación de cableado** — no hacen bind mientras el
> padre esté deshabilitado.

## Mapa de pines y buses

### SPI0 — SX1262 radio 0 (RadioService)
Clocks: `PCLK_SPI0_GATE`, `CLK_SPI0_GATE`, `SCLK_IN_SPI0_GATE` (VEPUCRU). Maestro, modo 0,
8-bit MSB primero, 8 MHz.

| Señal | GPIO RV1106 | IOMUX |
|---|---|---|
| CS / CLK | GPIO1_C0 / C1 | spi0m0, func 4 |
| MOSI / MISO | GPIO1_C2 / C3 | spi0m0, func 6 |
| RST | GPIO3_A6 | GPIO, activo-bajo |
| BUSY | GPIO3_A7 | GPIO in |
| DIO1 | GPIO3_A3 | GPIO + IRQ |
| ANT_SW0 / ANT_SW1 | GPIO0_A3 / A4 | GPIO out |

### SPI1 — SX1262 radio 1 (RadioService)
Clocks: `PCLK_SPI1_GATE`, `CLK_SPI1_GATE`, `SCLK_IN_SPI1_GATE` (PERICRU).

| Señal | GPIO RV1106 | IOMUX |
|---|---|---|
| CS / CLK / MISO / MOSI | GPIO4_A5 / A7 / A0 / A1 | spi1m0, func 2 |
| RST | GPIO1_C6 | GPIO, activo-bajo |
| BUSY | GPIO1_C7 | GPIO in |
| DIO1 | GPIO1_D1 | GPIO + IRQ |
| ANT_SW0 / ANT_SW1 | GPIO1_C5 / D0 | GPIO out |

Conmutador RF de antena (ambos radios, por GPIO, **no** por el DIO2 del chip):
**TX = ANT_SW0=1, ANT_SW1=0; RX = ANT_SW0=0, ANT_SW1=1.**

### I²C0 — BME280 + ICM-42670-P (SensorService)
Clocks: `PCLK_I2C0_GATE`, `CLK_I2C0_GATE` (PERICRU), habilitados antes de cualquier acceso a
registros. Bus a 400 kHz.

| Señal | GPIO RV1106 | IOMUX |
|---|---|---|
| SCL | GPIO1_A3 | i2c0m0, func 2 |
| SDA | GPIO1_A4 | i2c0m0, func 2 |

Esclavos: **BME280 @ 0x76**, **ICM-42670-P @ 0x68**. La línea AD0 del ICM se fija con un
`gpio-hog` en `&gpio0` (`GPIO0_A5` a nivel bajo → dirección 0x68).

## Añadir o mover un periférico

1. **Ceder en Linux:** poner el controlador en `status = "disabled"` en el DT, quitar el
   paquete de Linux, recompilar el DTB. Deshabilitar lo que colisione en los pines liberados.
2. **Reclamar en el MCU:** habilitar clocks → poner IOMUX (mismos pines/función que el DT de
   Linux) → manejar los GPIOs de control → escribir el driver como un handler rpmsg estilo
   `SensorService`/`RadioService`.

Paso a paso completo: [`../migration/implementation/90-mcu-config-replication.md`](../migration/implementation/90-mcu-config-replication.md) §6.
