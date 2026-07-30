# Visión general de la arquitectura

PwnCube es un target CubeSat construido sobre el **Rockchip RV1106**, un SoC con dos núcleos
asimétricos corriendo en paralelo (AMP):

| Núcleo | Ejecuta | Rol |
|---|---|---|
| **Cortex-A7** | Linux | Control de misión, tooling de enlace de tierra, los clientes rpmsg (`radio_test`, `sensor_test`) |
| **SCR1 (RISC-V)** | RT-Thread | Posee los radios y sensores; corre los servicios CCSDS de comando/telemetría en tiempo real |

Los dos núcleos se comunican por **rpmsg** — ver [`ipc-rpmsg.md`](ipc-rpmsg.md).

## Quién hace qué

- **El MCU posee el hardware que debe reaccionar en tiempo real**: ambos radios LoRa SX1262
  (SPI0/SPI1) y ambos sensores I²C0 (BME280, ICM-42670). Linux cede esos buses en el device
  tree. El reparto completo está en [`peripheral-ownership.md`](peripheral-ownership.md).
- **El MCU corre los servicios de misión**: `RadioService` (control de radio), `SensorService`
  (lectura de sensores), `CommandService` (uplink de telecomandos CCSDS + despacho),
  `TelemetryService` (downlink). Linux los maneja todos indirectamente por rpmsg.
- **Linux es el lado del operador**: aloja los clientes de usuario que inyectan telecomandos y
  observan telemetría, y es la superficie de ataque documentada en
  [`../security/exploitation-guide.md`](../security/exploitation-guide.md).

## Boot y memoria

El SPL/U-Boot arranca Linux en `0x208000` y libera el firmware del MCU, que ejecuta desde
SRAM/DDR y arranca un heartbeat. El mapa de memoria de arranque dual, la secuencia de
liberación del núcleo y las restricciones duras (nunca liberar el MCU a un busy-wait; flashear
con `UF`, nunca `DI -b`) están documentados en
[`../migration/implementation/80-dual-boot.md`](../migration/implementation/80-dual-boot.md)
y
[`../migration/implementation/90-mcu-config-replication.md`](../migration/implementation/90-mcu-config-replication.md).

## El enlace de misión

El satélite escucha telecomandos **CCSDS Space Packet** en un uplink LoRa (918 MHz / BW250 /
SF7 / CR4-5 / sync privada / preámbulo 8) y responde con telemetría en el downlink (916 MHz).
El driver de radio y la PHY se cubren en
[`../peripherals/sx1262-radio.md`](../peripherals/sx1262-radio.md); el protocolo y sus
vulnerabilidades heredadas a propósito en
[`../security/exploitation-guide.md`](../security/exploitation-guide.md).

## Por dónde empezar

- Compilar la imagen: [`../build/toolchain.md`](../build/toolchain.md) → `uboot` → `kernel`
  → `rootfs` → `packaging`.
- La historia de la migración RISC-V (cómo el MCU llegó a poseer los periféricos):
  [`../migration/README.md`](../migration/README.md).
