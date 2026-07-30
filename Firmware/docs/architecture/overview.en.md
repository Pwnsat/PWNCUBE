# Architecture overview

PwnCube is a CubeSat target built on the **Rockchip RV1106**, a SoC with two asymmetric cores
running side by side (AMP):

| Core | Runs | Role |
|---|---|---|
| **Cortex-A7** | Linux | Mission control, ground-link tooling, the rpmsg clients (`radio_test`, `sensor_test`) |
| **SCR1 (RISC-V)** | RT-Thread | Owns the radios and sensors; runs the CCSDS command/telemetry services in real time |

The two cores communicate over **rpmsg** — see [`ipc-rpmsg.md`](ipc-rpmsg.md).

## Who does what

- **The MCU owns the hardware that must react in real time**: both SX1262 LoRa radios (SPI0/
  SPI1) and both I²C0 sensors (BME280, ICM-42670). Linux cedes those buses in the device tree.
  The full split is in [`peripheral-ownership.md`](peripheral-ownership.md).
- **The MCU runs the mission services**: `RadioService` (radio control), `SensorService`
  (sensor reads), `CommandService` (CCSDS telecommand uplink + dispatch), `TelemetryService`
  (downlink). Linux drives them all indirectly through rpmsg.
- **Linux is the operator side**: it hosts the userspace clients that inject telecommands and
  watch telemetry, and it is the attack surface documented in
  [`../security/exploitation-guide.md`](../security/exploitation-guide.md).

## Boot and memory

The SPL/U-Boot brings up Linux at `0x208000` and releases the MCU firmware, which executes
from SRAM/DDR and starts a heartbeat. The dual-boot memory map, the core-release sequence, and
the hard constraints (never release the MCU into a busy-wait; flash with `UF`, never `DI -b`)
are documented in
[`../migration/implementation/80-dual-boot.md`](../migration/implementation/80-dual-boot.md)
and
[`../migration/implementation/90-mcu-config-replication.md`](../migration/implementation/90-mcu-config-replication.md).

## The mission link

The satellite listens for **CCSDS Space Packet** telecommands on a LoRa uplink (918 MHz /
BW250 / SF7 / CR4-5 / private sync / preamble 8) and answers with telemetry on the downlink
(916 MHz). The radio driver and PHY are covered in
[`../peripherals/sx1262-radio.md`](../peripherals/sx1262-radio.md); the protocol and its
deliberately-inherited vulnerabilities in
[`../security/exploitation-guide.md`](../security/exploitation-guide.md).

## Where to start

- Building the image: [`../build/toolchain.md`](../build/toolchain.md) → `uboot` → `kernel`
  → `rootfs` → `packaging`.
- The RISC-V migration story (how the MCU came to own the peripherals):
  [`../migration/README.md`](../migration/README.md).
