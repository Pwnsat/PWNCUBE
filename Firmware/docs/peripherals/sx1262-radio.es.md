# Radios SX1262 Dual LoRa

## Descripción general

Ambos transceptores Semtech SX1262 los maneja el **MCU RISC-V** corriendo
RT-Thread. El MCU los expone a través de un único **`RadioService`** en el canal
rpmsg **`rpmsg-radio`**, endpoint **`0x4005`**. Un byte `instance` por mensaje
selecciona a qué radio físico apunta un comando:

- **Radio 0**: SPI0 del MCU
- **Radio 1**: SPI1 del MCU

Linux no tiene driver de kernel para los chips en placa; abre el endpoint rpmsg
y habla el protocolo del RadioService desde userspace vía el cliente
**`radio_test`** (`src/radio-client/`). El set de comandos (R/W registros,
frecuencia, modulación, params de paquete, TX/RX, calibración, poll IRQ)
refleja el driver Linux heredado del que se portó.

El uplink/dispatch CCSDS de más alto nivel (`CommandService`, endpoint `0x4008`)
y el downlink de telemetría (`TelemetryService`, endpoint `0x4007`) están por
encima de los radios; ver [`../architecture/ipc-rpmsg.md`](../architecture/ipc-rpmsg.md).

## Manejo desde Linux

`radio_test` abre el endpoint `rpmsg-radio` y envía un comando enmarcado por
invocación. El flag global **`-r 0|1`** selecciona el radio (por defecto 0). El
chip pierde su configuración al apagar o resetear, así que una sesión siempre
empieza con `init`.

Los subcomandos se dividen en **dos familias** (`radio_test help` las marca):

- **Nativos** — control directo de la radio por *nuestra* `RadioService`
  (endpoint rpmsg `0x4005`). Son los comandos que creamos nosotros.
- **Heredados** — protocolo CCSDS de telecomando/telemetría **heredado del
  firmware de referencia ElectronicCats/FlatSat** (que portamos para interop),
  vía `CommandService` (`0x4008`) y `TelemetryService` (`0x4007`).

**Nativos — RadioService (control directo del SX1262):**

| Subcomando | Propósito |
|------------|-----------|
| `ping` | Verifica que el RadioService del MCU responde (id `RDIO`) |
| `reset` | Reset físico del chip (pierde init, vuelve a standby) |
| `init <freq> [sf= bw= cr= power= pre= crc= iq= sync=]` | Init + config de TX; params nombrados en cualquier orden |
| `freq <freq>` | Cambia solo la frecuencia (requiere init previo) |
| `power <dbm>` | Potencia TX en dBm, -9..22 (requiere init previo) |
| `cw` / `stop` | Portadora continua ON / standby (bring-up de espectro) |
| `status` / `errors` | Modo del chip (2=standby,4=FS,5=RX,6=TX) / flags de error internos |
| `reg <addr>` / `wreg <addr> <val>` | Lectura / escritura cruda de registros |
| `sync <pub\|priv\|hex>` | Sync word LoRa (priv=0x1424 default, pub=0x3444) |
| `antsw <0\|1\|2>` | Switch de antena: 0=auto, 1=TX, 2=RX |
| `mod <sf> <bw_khz> <cr>` | Configura la modulación LoRa |
| `pkt <pre> <hdr> <plen> <crc> <iq>` | Configura los params de paquete |
| `pkts` / `rssi` | RSSI+SNR del último paquete / RSSI instantáneo |
| `tx <texto>` | Transmite un paquete LoRa (bloquea hasta TX_DONE) |
| `rx [ms]` | Recibe (ventana ms, por defecto 10000, 0 = continuo) |
| `loopback <freq> <txt> [txi rxi]` | Test radio0→radio1 en placa en un solo comando |

**Heredados — protocolo CCSDS/FlatSat (firmware portado):**

Un telecomando (TC) es un CCSDS Space Packet; el MCU actúa sobre él igual que si
hubiera llegado por RF. Los `tc*` inyectan por rpmsg (saltándose el aire);
`ccsds` envía ese mismo formato **por la radio**.

| Subcomando | Endpoint | Propósito |
|------------|----------|-----------|
| `ccsds <apid> [txt]` | Radio (`0x4005`) | TX de un paquete CCSDS SPP por el aire (formato heredado, transporte nativo) |
| `tcsend <apid_hex> [pay_hex…]` | Command (`0x4008`) | Inyecta un TC en claro por rpmsg (salta el RF). Ej: `tcsend 04 00 0A` = thruster0=10 |
| `tcsecsend <apid_hex> [pay_hex…]` | Command (`0x4008`) | Inyecta un TC **seguro** (sec-hdr timestamp + AES-128-CTR/XOR/plano + CRC-16); dificultad por env `CCSDS_DIFF` (debe coincidir con el receptor) |
| `tcbroad <freq> [txt]` | Command (`0x4008`) | `TC_BROADCAST_MSG` (APID 0x06) en frecuencia arbitraria (430–960 MHz) |
| `cmd_ping` / `cmd_start <freq>` / `cmd_stop` | Command (`0x4008`) | Ping / arranque (config uplink) / parada del servicio |
| `cmd_status` / `cmd_config <freq> [sf bw cr]` | Command (`0x4008`) | Estado (thrusters, beacon, TC count) / reconfig del uplink |
| `cmd_listen` / `cmd_watch` | Command (`0x4008`) | Stream de `EVT_TC_RX` / vista unificada TC+efecto |
| `tlm` | Telemetry (`0x4007`) | Monitor del downlink (TM/beacon/idle/respuestas) |

> El protocolo CCSDS SPP y su superficie de ataque se detallan en
> [`../security/exploitation-guide.md`](../security/exploitation-guide.md); el
> ruteo de endpoints rpmsg en [`../architecture/ipc-rpmsg.md`](../architecture/ipc-rpmsg.md).

**Ejemplos de invocación:**

```bash
# Configura el radio 0 para el uplink operativo y confirma que responde
radio_test ping
radio_test init 918000000        # SF7 BW250 CR4/5 pre8 CRC on 20 dBm sync priv

# Transmite un paquete y luego escucha en el radio 1
radio_test -r 0 tx "hello world"
radio_test -r 1 rx 15000

# Loopback en placa en un solo tiro (inicia ambos radios, el 1 escucha, el 0 envía)
radio_test loopback 918000000 "ping"
```

### Defaults operativos en el aire

Estos son los parámetros que realmente salen por el aire (distintos de los
valores de init del chip pelado):

| Parámetro | Valor |
|-----------|-------|
| Frecuencia de uplink | 918 MHz |
| Frecuencia de downlink | 916 MHz |
| Spreading factor | SF7 |
| Ancho de banda | **250 kHz** |
| Coding rate | 4/5 |
| Potencia TX | **20 dBm** |
| CRC | **on** |
| IQ | estándar |
| Preámbulo | **8** símbolos |
| Sync word | 0x1424 (privada) |

> El `sx1262_init` pelado programa en el chip SF7/BW125/preámbulo 12/20 dBm, pero
> la ruta operativa sobreescribe el ancho de banda a 250 kHz y el preámbulo a 8 —
> los valores de arriba son los que usa el enlace.

## Referencia de hardware (dispositivo físico)

Estas características describen los módulos SX1262 de la placa, independientes de
quién los maneje. Para el mapa completo de propiedad de pines (qué subsistema
reclama cada GPIO) ver
[`../architecture/peripheral-ownership.md`](../architecture/peripheral-ownership.md).

Características del módulo (verificadas en la placa):
- **Cristal de 32 MHz (XTAL), NO TCXO** — DIO3-como-TCXO **no** debe habilitarse
  (habilitarlo hace que el XOSC no arranque).
- **Switch de antena externo por GPIO** (no por el DIO2 del chip):
  **TX = ANT_SW0=1, ANT_SW1=0; RX = ANT_SW0=0, ANT_SW1=1**.
- Registro OCP `0x08E7 = 0x38` (140 mA).
- El registro InvertIQ escribe `0x01` (correcto; `0x40` fue un bug antiguo).
- La calibración de imagen corre **después** de `SetRfFrequency` y se re-ejecuta en cada retune.
- El RX es **continuo** (se re-arma tras cada paquete), no single-shot.

### Asignación de pines

**Radio 0 (SPI0)**
| Pin SX1262 | GPIO RV1106 | Función |
|------------|-------------|---------|
| CS / SCK / MOSI / MISO | GPIO1_C0 / C1 / C2 / C3 | SPI0 |
| RST  | GPIO3_A6 | GPIO (activo bajo) |
| BUSY | GPIO3_A7 | GPIO in |
| DIO1 | GPIO3_A3 | GPIO + IRQ |
| ANT_SW0 | GPIO0_A3 | GPIO out |
| ANT_SW1 | GPIO0_A4 | GPIO out |

**Radio 1 (SPI1)**
| Pin SX1262 | GPIO RV1106 | Función |
|------------|-------------|---------|
| CS / SCK / MISO / MOSI | GPIO4_A5 / A7 / A0 / A1 | SPI1 |
| RST  | GPIO1_C6 | GPIO (activo bajo) |
| BUSY | GPIO1_C7 | GPIO in |
| DIO1 | GPIO1_D1 | GPIO + IRQ |
| ANT_SW0 | GPIO1_C5 | GPIO out |
| ANT_SW1 | GPIO1_D0 | GPIO out |

Número GPIO = `banco*32 + (A=0,B=8,C=16,D=24) + índice`.

---

## Legado: driver de kernel Linux (solo referencia)

> **Este driver NO hace bind en placa.** Ambos buses SPI están `status = "disabled"`
> en el DTS (cedidos al MCU — ver el banner), así que `sx1262.ko` nunca hace probe.
> Se conserva como fuente y como la referencia desde la que se portó el set de
> comandos del MCU. Los detalles ioctl/sysfs de abajo son para quien resucite la
> ruta manejada por Linux.

Históricamente los dos radios los manejaba un driver nativo de kernel Linux
(`sx1262.ko`), cada uno expuesto como un dispositivo de caracteres:

- **SX1262 #0**: SPI0 → `/dev/sx1262-0`
- **SX1262 #1**: SPI1 → `/dev/sx1262-1`

Una CLI de usuario (`sx1262_cli`, empaquetada como **sx1262**) hablaba con el
driver vía ioctls / read() / write().

```
Userspace                  Kernel                     Hardware
sx1262_cli ──ioctl──►  sx1262.ko ──spi_sync──►  SX1262 #0 (SPI0)
  (--dev 0)              │                           │
                        ├── gpiod_set_value()  ──►  RST, ANT_SW0/1
                        ├── gpiod_get_value()  ◄──  BUSY
                        ├── request_threaded_irq() DIO1
sx1262_cli ──ioctl──►  sx1262.ko ──spi_sync──►  SX1262 #1 (SPI1)
  (--dev 1)
```

### Driver de kernel (`src/sx1262-kmod/`)

| Archivo | Propósito |
|---------|-----------|
| `sx1262_core.c`    | Init del módulo, registro del driver SPI, alloc chardev/clase, probe por radio |
| `sx1262_hal.c`     | Transferencias SPI, control GPIO (gpiod), handler IRQ DIO1, switch de antena |
| `sx1262_cmd.c`     | Comandos del SX1262: R/W registros, FIFO, frecuencia, modulación, TX/RX, calibración, poll IRQ |
| `sx1262_chardev.c` | `/dev/sx1262-{0,1}`: ioctl, write (TX), read (RX), poll |
| `sx1262.h`         | Header público (ioctls, structs) |
| `sx1262_regs.h`    | Opcodes, mapa de registros, máscaras IRQ |

**Nodos:** `/dev/sx1262-0`, `/dev/sx1262-1`

**ioctls** (magic `'L'`):

| Comando | Arg | Descripción |
|---------|-----|-------------|
| `SX1262_IOCTL_RESET`         | — | Reset hardware |
| `SX1262_IOCTL_SET_STANDBY`   | — | STDBY_RC |
| `SX1262_IOCTL_SET_SLEEP`     | — | Sleep |
| `SX1262_IOCTL_SET_FREQ`      | `uint32_t` Hz | Frecuencia (recalibra imagen para la banda) |
| `SX1262_IOCTL_SET_POWER`     | `int8_t` dBm | Potencia TX (-9..22) |
| `SX1262_IOCTL_SET_MODEM`     | `struct sx1262_modem_config` | SF / BW / CR / preámbulo |
| `SX1262_IOCTL_SET_PACKET`    | `struct sx1262_packet_config` | header / longitud / CRC / IQ |
| `SX1262_IOCTL_SET_TX`        | `uint32_t` ms | Inicia TX (timeout) |
| `SX1262_IOCTL_SET_RX`        | `uint32_t` ms | Inicia RX (0 = continuo) |
| `SX1262_IOCTL_SET_CW`        | — | Portador continuo sin modular (bring-up / espectro) |
| `SX1262_IOCTL_GET_STATUS`    | `uint8_t` | Byte de estado |
| `SX1262_IOCTL_GET_RSSI`      | `int16_t` | RSSI instantáneo |
| `SX1262_IOCTL_GET_IRQ_STATUS`| `uint16_t` | Registro de estado IRQ |
| `SX1262_IOCTL_SET_ANTSW`     | `uint8_t` | 0=auto, 1=forzar TX, 2=forzar RX |
| `SX1262_IOCTL_READ_REG` / `WRITE_REG` | `struct sx1262_reg_access` | Acceso crudo a registros |

**write()** carga el FIFO, fija la longitud, conmuta la antena a TX, inicia TX y espera TX_DONE.
**read()** bloquea hasta que DIO1 dispara (RX_DONE/timeout) y devuelve el payload exacto (longitud vía GetRxBufferStatus). Errores de CRC devuelven sin datos.
**poll(POLLIN)** espera la IRQ de DIO1.

La radio se auto-inicializa en `sx1262_init()` durante el probe: STDBY_RC → regulador
DC-DC → tipo LoRa → fallback STDBY_RC → calibrar → calibración de imagen por banda →
config PA (+ errata TxClamp) → frecuencia → params TX → modulación (SF7/BW125/CR4-5,
+ errata TxModulation) → buffer base → params de paquete → IRQ DIO1 (TxDone/RxDone/Timeout).

### CLI heredada (`sx1262_cli`)

```
Uso: sx1262_cli --dev <0|1> <comando> [args]

  detect                 Verifica que el chip responde
  reset | standby | sleep
  freq <hz>              Frecuencia (Hz)
  power <dbm>            Potencia TX (-9..22)
  modem <sf> <bwhz> <cr> ej. 7 125000 1  (SF7, BW125k, CR4/5)
  setTX <hz> <ms>        Freq + inicia TX
  setRX <hz> <ms>        Freq + inicia RX (0 = continuo)
  cw <hz>                Portador continuo (detener con 'standby')
  send <hex> [ms]        Carga FIFO + transmite (espera TX_DONE)
  recv [ms]              Recibe un paquete (poll DIO1 + read)
  rssi | status | irq
  regread <hexaddr> | regwrite <hexaddr> <hexval>
```

### Notas de implementación heredadas

- `SetRfFrequency` envía 4 bytes Frf (`0x86, Frf[31:24..7:0]`); si falta el MSB el
  portador cae en la frecuencia equivocada.
- `sx1262_poll_irq()` **no** limpia la IRQ — el llamador (`read()` / `set_tx()`) la
  lee y la limpia, de modo que RX_DONE no se pierde antes de que `read()` lo consuma.
- Erratas aplicadas: 15.1 (TxModulation, reg 0x0889) y 15.2 (TxClampConfig, reg 0x08D8).
- La longitud del payload RX viene de GetRxBufferStatus; la calibración de imagen se
  re-ejecuta para la banda en cada cambio de frecuencia.
