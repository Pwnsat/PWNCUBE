# Real GPS receiver (u-blox NEO-6M) — UART0

## Overview

A real u-blox NEO-6M-0-001 GPS receiver, owned by the **RISC-V MCU** running
RT-Thread, on a dedicated **UART0** link. Like the I2C0 sensor bus, this
peripheral is driven with bare `HAL_UART_*` calls in poll mode — the RV1106
MCU BSP never wires up RT-Thread's serial device framework
(`RT_USING_UART`/`drv_uart.c`), so `HAL_UART_MODULE_ENABLED` is force-enabled
directly in `hal_conf.h` (same treatment already given to
`HAL_SPI_MODULE_ENABLED` for the SX1262 radios) instead.

Driver: `applications/gps_nmea.c`/`.h`. A minimal, hand-rolled NMEA parser
reads `$GPGGA`/`$GNGGA` sentences only — that one sentence already carries
latitude, longitude, altitude, satellite count, and fix quality in a single
line, with no need to cross-reference a second sentence like `$GPRMC`. No
checksum verification (matches this firmware's existing "errors are never
checked" stance elsewhere). 9600 baud, 8N1 — the NEO-6M's factory default.

## Wiring

The board exposes a 6-pin header: `GND / SCL / SDA / TX / RX / 3V3`. The
`SCL`/`RX` and `SDA`/`TX` pairs are the **same two SoC pads**
(`GPIO2_B0`/`GPIO2_B1`), alt-muxed between **I2C1** and **UART0** — a combo
header design. Both controllers are `status = "disabled"` in the Linux
device tree, and neither pin overlaps the SX1262 radios, the BME280/ICM42670
I2C0 bus, or the onboard IPC camera's I2C4.

| NEO-6M pin | Header pin | RV1106 GPIO | Mux |
|------------|-----------|-------------|-----|
| TX (module output) | `RX` | GPIO2_B0 | uart0 m1, func 1 |
| RX (module input, optional) | `TX` | GPIO2_B1 | uart0 m1, func 1 |
| VCC | `3V3` | — | — |
| GND | `GND` | — | — |

Cross the data lines: the module's **TX** goes to the header's **RX**, and
vice versa. The module's RX line is only needed if you ever want to send
configuration commands to it — reading NMEA output only needs TX→RX.

## Driving it from Linux

Two `radio_test` subcommands, both synchronous local rpmsg queries — no
SPP/RF round trip involved:

| Subcommand | Purpose |
|------------|---------|
| `radio_test gps_status` | Current parsed state: debug-override flag, UART health, fix validity, satellite count, lat/lon/alt |
| `radio_test gps_raw` | Field-debug diagnostic: total bytes ever received/transmitted on UART0, and the last raw line seen (parsed or not) — useful for telling "real NMEA text, just no fix yet" apart from "wiring/baud problem" |

```bash
radio_test gps_status
# gps_status: override=0 uart_ok=1 fix=0 sats=0 lat=0.000000 lon=0.000000 alt=0.00
```

The same data also appears in the Linux console's Sensor/Telemetry Dashboard
(`pwnsat-console`, menu option 1, "GPS / NAV" section), and — once a fix is
valid, or the debug override below is active — in the periodic `NAV`
telemetry frame (SPP APID `0x0B`) broadcast over the downlink radio.

## Relationship to the `GPS_OVERRIDE` debug hook

This firmware also ships a **debug injection hook** (`GPS_OVERRIDE`, SPP TC
APID `0x0A`) that lets an operator (or an attacker, since it has zero
authentication) inject an arbitrary position directly into the same shared
state this real driver populates — see
[`../security/exploitation-guide.en.md`](../security/exploitation-guide.en.md)
for the full vulnerability writeup. The two sources are deliberately kept
separate:

- **Precedence:** a real fix only ever overwrites the shared
  latitude/longitude/altitude/satellite state when the debug override is
  **not** currently active. The override always wins while active — this
  keeps demonstrations deterministic regardless of whether the receiver has
  sky view that day.
- **The ground-station authentication gate is keyed only to the debug
  override**, never to a real fix (`gs_gps_usable()` in
  `applications/command_service.c`). A genuine satellite fix cannot, by
  itself, unlock the ground-station handshake — only the deliberately
  unauthenticated `GPS_OVERRIDE` TC can, matching the vulnerability the
  platform is meant to demonstrate.

## Known limitation

Only `$GPGGA`/`$GNGGA` is parsed. A u-blox NEO-6M's factory-default NMEA
output set includes GGA, so this is not expected to matter with an
unmodified module — but if a specific unit was reconfigured to omit GGA from
its output set, this driver will never see a fix even with a perfectly
healthy physical link. `$GPRMC` parsing (fix validity + timestamp,
independent of GGA) is not implemented.
