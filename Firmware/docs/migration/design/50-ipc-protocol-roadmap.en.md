# IPC Protocol, Services, CCSDS and Roadmap

> **Nature of the document.** Design record — the protocol below is **implemented
> and running on hardware**. It relies on the real Luckfox transport (doc 20) and
> on the `rpmsg_cmd` pattern (cmd→handler table). Version: **v1**. For the
> as-built IPC layer see [../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md).

## 1. IPC protocol

### 1.1 Principles
- **Versioning:** each message carries `version` (start at 1). Incompatible changes
  → new version.
- **Transport:** RPMsg, usable payload **≤ 496 B** (doc 20 §3). Larger messages →
  fragment (`seq`/`frag` field).
- **Model:** request/response with a correlatable `req_id`; plus asynchronous
  unsolicited events (telemetry, radio IRQ).
- **Endianness:** little-endian (both cores LE).

### 1.2 Common header (proposal, 8 bytes)

```c
struct ipc_hdr {           /* little-endian */
    uint8_t  version;      /* =1 */
    uint8_t  service;      /* RADIO=1, SENSOR=2, CONFIG=3, EVENT=4 */
    uint8_t  cmd;          /* específico del servicio */
    uint8_t  flags;        /* bit0=RESPONSE, bit1=EVENT, bit2=ERROR, bit3=MORE_FRAGS */
    uint16_t req_id;       /* correlación request/response */
    uint16_t len;          /* bytes de payload que siguen */
    /* payload[len] */
};
```

Error response: `flags|=ERROR`, payload = `int32 error_code` (mirror of the
`-Exxx`/codes of the current driver).

### 1.3 Services and commands (derived from the current interface)

**RadioService (service=1)** — mirror of `SX1262_IOCTL_*` (`src/sx1262-kmod/sx1262.h:37-54`).
First byte of payload = `instance` (0 or 1, for the two radios).

| cmd | Name | Payload req | Payload resp |
|-----|--------|-------------|--------------|
| 1 | RESET | inst | — |
| 2 | STANDBY / 3 SLEEP | inst | — |
| 4 | SET_FREQ | inst, u32 hz | — |
| 5 | SET_POWER | inst, i8 dBm | — |
| 6 | SET_MODEM | inst, `modem_config` | — |
| 7 | SET_TX / 8 SET_RX | inst, params | — |
| 9 | TX_PAYLOAD | inst, u8[≤255] | — (TX_DONE event) |
| 10 | RX_PAYLOAD | inst | u8[], rssi, snr |
| 11 | GET_STATUS / 12 GET_RSSI | inst | status |
| 13 | SET_ANTSW | inst, mode | — |
| 14/15 | READ/WRITE_REG | inst, addr[,val] | val |
| 16 | GET_IRQ_STATUS | inst | u16 irq |

Events (flags=EVENT): `RADIO_TX_DONE`, `RADIO_RX_DONE` (payload + rssi/snr),
`RADIO_TIMEOUT` — generated from the DIO1 ISR on the RISC-V.

**SensorService (service=2)**

| cmd | Name | Resp |
|-----|--------|------|
| 1 | READ_BME280 | i32 temp_m°C, i32 pres_kPa·1000, i32 hum_m%RH |
| 2 | READ_ICM42670 | i32 ax,ay,az,gx,gy,gz (scaled), i32 temp |
| 3 | READ_ALL | concatenation |

**ConfigurationService (service=3):** get/set of persistent parameters
(default frequency/power, IMU ODR, sampling period).

**EventService (service=4):** subscription to asynchronous events and *health/heartbeat*
of the RISC-V (A7↔MCU watchdog).

### 1.4 Compatibility with existing code
The old `sx1262_cli` CLI and the IIO readers were replaced by **IPC clients**
(doc 40 §1): Linux now drives the radios and sensors through `radio_test` /
`sensor_test`. The legacy `/dev/sx1262-*` char-dev and IIO paths are retired now
that the hardware is owned by the MCU.

## 2. Implementation roadmap

Implementation order was **SPI → SX1262 → BME280 → ICM-42670**, preceded by the
IPC transport. **All steps below are delivered and validated on hardware** (✔).

| # | Deliverable | Acceptance | Status |
|------|-----------|------------|--------|
| **0. IPC transport** | rpmsg wired for RV1106 (kernel match + DT + porting + PING dispatcher) + `mcu` target in `build.sh` | Echo A7↔RISC-V over `/dev/rpmsg*` in hardware. | ✔ done |
| **1. SPI** | `RT_USING_SPI` + `drv_spi` configured; SPI0/SPI1 reassigned to the MCU | Loopback / ID read of the SX1262 from the MCU. | ✔ done |
| **2. SX1262** | RadioService with `sx1262_cmd.c` reused + portability layer | End-to-end LoRa TX/RX commanded from Linux via IPC. | ✔ done |
| **3. BME280** | I²C SensorService | Correct T/P/H reading via IPC. | ✔ done |
| **4. ICM-42670** | I²C SensorService + AD0/INT1 | accel/gyro/temp reading via IPC. | ✔ done |
| **5. Services** | TelemetryService + CommandService (CCSDS over LoRa) + heartbeat | Periodic telemetry and commanding. | ✔ done |

Step 5 shipped as **TelemetryService** and **CommandService** (CCSDS Space
Packets over LoRa) rather than the originally sketched Config/Event pair; a
heartbeat/watchdog between A7 and MCU is in place.

Each step followed: confirmed design → implementation → hardware test (access
flow via serial 115200 / build / insmod documented in project memory) → EN/ES
documentation.

## 3. Services
Implemented as RT-Thread threads on the RISC-V, each with its command table
(`rpmsg_cmd` pattern, doc 20 §1.3). Linux exposes equivalent clients. The
as-built services and their rpmsg endpoints are:

| Service | rpmsg name | endpoint |
|---------|-----------|----------|
| PingEcho | `rpmsg-ap3-ch0` | `0x4004` |
| RadioService | `rpmsg-radio` | `0x4005` |
| SensorService | `rpmsg-sensor` | `0x4006` |
| TelemetryService | `rpmsg-telemetry` | `0x4007` |
| CommandService | `rpmsg-command` | `0x4008` |

The originally sketched Configuration/Event services were realized as
**TelemetryService** and **CommandService**, which speak CCSDS over LoRa (§4).

## 4. CCSDS — implemented and verified
The **Space Packet Protocol** is implemented and verified end-to-end:
Primary Header (APID, Sequence Count/Flags, Packet Length), Payload. The
**TelemetryService** and **CommandService** frame CCSDS Space Packets over LoRa;
the RadioService carries the packaged bytes. (The original plan kept CCSDS on the
Linux side only; in the as-built system these two MCU services own the framing.)

```
CCSDS Space Packet Primary Header (6 bytes):
  [Version(3) Type(1) SecHdrFlag(1) APID(11)]
  [SeqFlags(2) SeqCount(14)]
  [PacketLength(16) = (len_datos - 1)]
```

## 5. Architectural changes made (with sign-off)
These architectural changes were made (each required sign-off):
- Edited the **CubeSat device tree** to cede SPI0/SPI1/I²C0 to the RISC-V and add
  the rpmsg node + reserved-memory.
- Added a **RISC-V firmware tree** (adapted `rv1106-mcu` BSP) to the repo and an
  **`mcu` target** to `build.sh`.
- Modified the **kernel** (rpmsg match table) and the **rkbin** flow (`LOADER2=Hpmcu`).
- Replaced the user interfaces (CLI/IIO) with **IPC clients**.

Boot/flash invariants that hold in the shipped image: MCU firmware executes from
`0x40000`, Linux from `0x208000`; flash with `UF` (never `DI -b`); never release
the MCU into a busy-wait.

> **Current status:** migration complete and running on hardware. The IPC
> transport plus the Radio, Sensor, Telemetry and Command services are
> implemented and validated; CCSDS over LoRa is verified (see the implementation/
> docs and [../../architecture/overview.md](../../architecture/overview.md)).
