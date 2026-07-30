# IPC — rpmsg entre Linux (A7) y el MCU (RISC-V)

Linux (Cortex-A7) y el MCU (SCR1, RT-Thread) se comunican por **rpmsg-lite** (vrings virtio +
mailbox por hardware). Linux es el maestro; el MCU anuncia **endpoints con nombre**. Todos los
servicios comparten una instancia rpmsg-lite, y un único hilo de poll en el MCU
(`ping_echo.c`) atiende cada servicio por turno.

Ver también: [`overview.md`](overview.md), [`peripheral-ownership.md`](peripheral-ownership.md).

## Servicios y endpoints

| Servicio | Nombre de canal | Endpoint | Responsabilidad | Cliente Linux |
|---|---|---|---|---|
| PingEcho | `rpmsg-ap3-ch0` | 0x4004 | Bring-up del enlace + echo; aloja el hilo de poll que atiende todos los servicios | — |
| **RadioService** | `rpmsg-radio` | 0x4005 | Ambos SX1262 (el byte `instance` elige radio 0/1): init, freq, power, TX, RX, modulación, packet, sync. Empuja `EVT_RX` / `EVT_RX_TIMEOUT`. | `radio_test` |
| **SensorService** | `rpmsg-sensor` | 0x4006 | BME280 (@0x76) + ICM-42670 (@0x68) en I²C0: `PING`, `WHOAMI`, `BME280_READ`, `IMU_READ` | `sensor_test` |
| **TelemetryService** | `rpmsg-telemetry` | 0x4007 | Control del downlink (start/stop/status/config/monitor); el worker corre en CommandService | `radio_test tlm` |
| **CommandService** | `rpmsg-command` | 0x4008 | RX de uplink en radio 0 + despacho de telecomandos; TX de downlink en radio 1. Empuja `EVT_TC_RX` (0xE3). | `radio_test cmd_* / tcsend` |

## Patrón de mensajes

El byte 0 de la petición es el comando; el byte 1 es el `instance` (radio 0/1) para
RadioService; luego va el payload. Las respuestas son síncronas; los **eventos** (`EVT_RX`,
`EVT_TC_RX` = 0xE3) los empuja el MCU de forma asíncrona cuando llega un paquete, y el cliente
Linux hace `poll()` por ellos (p. ej. `radio_test cmd_listen` / `cmd_watch`).

## Forma del transporte

- El payload rpmsg-lite es **≤ 496 B**; los mensajes mayores se fragmentan.
- Medido en este SoC: **B2A** (MCU→A7) funciona directo. Para **A2B** (A7→MCU) los registros
  de status/payload del mailbox leen 0 en el MCU, pero la **interrupción del mailbox (vector 2)
  sí dispara** — así que el RX es **interrupt-driven**: la ISR hace ack del doorbell y libera un
  semáforo, y el hilo de drenado (único dueño de los vrings) se despierta y los atiende. Un
  timeout de 2 ms queda solo como fallback para el trabajo periódico de servicios, no como la
  ruta de RX. (Confirmado empíricamente: la IRQ del mailbox dispara en cada kick del A7; todo lo
  demás en el MCU —SPI/I²C/GPIO— es polled.) Detalles:
  [`../migration/reference/20-luckfox-ipc.md`](../migration/reference/20-luckfox-ipc.md) y
  [`../migration/implementation/70-mailbox-loopback-test.md`](../migration/implementation/70-mailbox-loopback-test.md).
- En Linux los endpoints aparecen bajo `/dev/rpmsg*`; `radio_test` abre los destinos 0x4005,
  0x4007 y 0x4008.

## Diseño del protocolo y bring-up

- Diseño / mapa de comandos: [`../migration/design/50-ipc-protocol-roadmap.md`](../migration/design/50-ipc-protocol-roadmap.md)
- Historia del bring-up: [`../migration/implementation/60-ipc-bringup.md`](../migration/implementation/60-ipc-bringup.md)
