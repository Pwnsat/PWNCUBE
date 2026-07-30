# Protocolo IPC, Servicios, CCSDS y Roadmap

> **Naturaleza del documento.** Registro de diseño — el protocolo de abajo está
> **implementado y corriendo en placa**. Se apoya en el transporte real de
> Luckfox (doc 20) y en el patrón `rpmsg_cmd` (tabla cmd→handler). Versión:
> **v1**. Para la capa IPC tal como quedó construida ver
> [../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md).

## 1. Protocolo IPC

### 1.1 Principios
- **Versionado:** cada mensaje lleva `version` (empezar en 1). Cambios incompatibles
  → nueva versión.
- **Transporte:** RPMsg, payload útil **≤ 496 B** (doc 20 §3). Mensajes mayores →
  fragmentar (campo `seq`/`frag`).
- **Modelo:** request/response con `req_id` correlacionable; más eventos asíncronos
  no solicitados (telemetría, IRQ de radio).
- **Endianness:** little-endian (ambos núcleos LE).

### 1.2 Cabecera común (propuesta, 8 bytes)

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

Respuesta de error: `flags|=ERROR`, payload = `int32 error_code` (espejo de los
`-Exxx`/códigos del driver actual).

### 1.3 Servicios y comandos (derivados de la interfaz actual)

**RadioService (service=1)** — espejo de `SX1262_IOCTL_*` (`src/sx1262-kmod/sx1262.h:37-54`).
Primer byte de payload = `instance` (0 ó 1, para las dos radios).

| cmd | Nombre | Payload req | Payload resp |
|-----|--------|-------------|--------------|
| 1 | RESET | inst | — |
| 2 | STANDBY / 3 SLEEP | inst | — |
| 4 | SET_FREQ | inst, u32 hz | — |
| 5 | SET_POWER | inst, i8 dBm | — |
| 6 | SET_MODEM | inst, `modem_config` | — |
| 7 | SET_TX / 8 SET_RX | inst, params | — |
| 9 | TX_PAYLOAD | inst, u8[≤255] | — (evento TX_DONE) |
| 10 | RX_PAYLOAD | inst | u8[], rssi, snr |
| 11 | GET_STATUS / 12 GET_RSSI | inst | estado |
| 13 | SET_ANTSW | inst, mode | — |
| 14/15 | READ/WRITE_REG | inst, addr[,val] | val |
| 16 | GET_IRQ_STATUS | inst | u16 irq |

Eventos (flags=EVENT): `RADIO_TX_DONE`, `RADIO_RX_DONE` (payload + rssi/snr),
`RADIO_TIMEOUT` — generados desde la ISR de DIO1 en el RISC-V.

**SensorService (service=2)**

| cmd | Nombre | Resp |
|-----|--------|------|
| 1 | READ_BME280 | i32 temp_m°C, i32 pres_kPa·1000, i32 hum_m%RH |
| 2 | READ_ICM42670 | i32 ax,ay,az,gx,gy,gz (escalados), i32 temp |
| 3 | READ_ALL | concatenación |

**ConfigurationService (service=3):** get/set de parámetros persistentes
(frecuencia/potencia por defecto, ODR del IMU, periodo de muestreo).

**EventService (service=4):** suscripción a eventos asíncronos y *health/heartbeat*
del RISC-V (watchdog A7↔MCU).

### 1.4 Compatibilidad con lo existente
La antigua CLI `sx1262_cli` y los lectores IIO se reemplazaron por **clientes
IPC** (doc 40 §1): Linux maneja ahora radios y sensores mediante `radio_test` /
`sensor_test`. Las rutas legadas `/dev/sx1262-*` (char-dev) e IIO quedan retiradas
ahora que el hardware es propiedad del MCU.

## 2. Roadmap de implementación

El orden de implementación fue **SPI → SX1262 → BME280 → ICM-42670**, precedido
del transporte IPC. **Todos los pasos de abajo están entregados y validados en
hardware** (✔).

| # | Entregable | Aceptación | Estado |
|------|-----------|------------|--------|
| **0. Transporte IPC** | rpmsg cableado RV1106 (kernel match + DT + porting + dispatcher PING) + target `mcu` en `build.sh` | Eco A7↔RISC-V por `/dev/rpmsg*` en hardware. | ✔ hecho |
| **1. SPI** | `RT_USING_SPI` + `drv_spi` configurado; SPI0/SPI1 reasignados al MCU | Loopback / lectura de ID del SX1262 desde el MCU. | ✔ hecho |
| **2. SX1262** | RadioService con `sx1262_cmd.c` reutilizado + capa de portabilidad | TX/RX LoRa end-to-end ordenado desde Linux por IPC. | ✔ hecho |
| **3. BME280** | SensorService I²C | Lectura T/P/H correcta vía IPC. | ✔ hecho |
| **4. ICM-42670** | SensorService I²C + AD0/INT1 | Lectura accel/gyro/temp vía IPC. | ✔ hecho |
| **5. Servicios** | TelemetryService + CommandService (CCSDS sobre LoRa) + heartbeat | Telemetría periódica y telecomando. | ✔ hecho |

El paso 5 salió como **TelemetryService** y **CommandService** (paquetes CCSDS
Space Packet sobre LoRa) en vez del par Config/Event originalmente esbozado; hay
un heartbeat/watchdog entre A7 y MCU.

Cada paso siguió: diseño confirmado → implementación → prueba en hardware (flujo
de acceso por serie 115200 / build / insmod documentado en la memoria del
proyecto) → documentación EN/ES.

## 3. Servicios
Implementados como hilos RT-Thread en el RISC-V, cada uno con su tabla de comandos
(patrón `rpmsg_cmd`, doc 20 §1.3). Linux expone clientes equivalentes. Los
servicios tal como quedaron construidos y sus endpoints rpmsg son:

| Servicio | nombre rpmsg | endpoint |
|----------|-------------|----------|
| PingEcho | `rpmsg-ap3-ch0` | `0x4004` |
| RadioService | `rpmsg-radio` | `0x4005` |
| SensorService | `rpmsg-sensor` | `0x4006` |
| TelemetryService | `rpmsg-telemetry` | `0x4007` |
| CommandService | `rpmsg-command` | `0x4008` |

Los servicios Configuration/Event originalmente esbozados se materializaron como
**TelemetryService** y **CommandService**, que hablan CCSDS sobre LoRa (§4).

## 4. CCSDS — implementado y verificado
El **Space Packet Protocol** está implementado y verificado extremo a extremo:
Primary Header (APID, Sequence Count/Flags, Packet Length), Payload. El
**TelemetryService** y el **CommandService** enmarcan paquetes CCSDS Space Packet
sobre LoRa; el RadioService transporta los bytes ya empaquetados. (El plan
original mantenía CCSDS solo del lado Linux; en el sistema tal como quedó, esos
dos servicios del MCU son los dueños del framing.)

```
CCSDS Space Packet Primary Header (6 bytes):
  [Version(3) Type(1) SecHdrFlag(1) APID(11)]
  [SeqFlags(2) SeqCount(14)]
  [PacketLength(16) = (len_datos - 1)]
```

## 5. Cambios arquitectónicos realizados (con visto bueno)
Estos cambios arquitectónicos se realizaron (cada uno requirió visto bueno):
- Se editó el **device tree del CubeSat** para ceder SPI0/SPI1/I²C0 al RISC-V y
  añadir el nodo rpmsg + reserved-memory.
- Se añadió un **árbol de firmware RISC-V** (BSP `rv1106-mcu` adaptado) al repo y
  un **target `mcu`** al `build.sh`.
- Se modificó el **kernel** (tabla de match rpmsg) y el flujo **rkbin** (`LOADER2=Hpmcu`).
- Se reemplazaron las interfaces de usuario (CLI/IIO) por **clientes IPC**.

Invariantes de boot/flash que se cumplen en la imagen entregada: el firmware del
MCU ejecuta desde `0x40000`, Linux desde `0x208000`; flashear con `UF` (nunca
`DI -b`); nunca soltar el MCU a un busy-wait.

> **Estado actual:** migración completa y corriendo en placa. El transporte IPC
> más los servicios Radio, Sensor, Telemetry y Command están implementados y
> validados; CCSDS sobre LoRa está verificado (ver los docs de implementation/ y
> [../../architecture/overview.md](../../architecture/overview.md)).
