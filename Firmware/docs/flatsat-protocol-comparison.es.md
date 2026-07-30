# Comparación de protocolo FlatSat — ElectronicCats vs PWNSat

Dos firmwares de FlatSat distintos hablan dos perfiles CCSDS **diferentes y no
interoperables** sobre LoRa. Este documento fija exactamente dónde divergen los
dos formatos de wire, con el código que produce cada uno, y cómo se relaciona el
port RISC-V de pwncube (esta rama `ccsds-tc-library`) con ambos.

> **Nomenclatura.** Notas previas los llamaban "FlatSat Zephyr" y "FlatSat
> RadioLib". Este documento usa **FlatSat (ElectronicCats)** para el firmware
> Zephyr y **FlatSat (PWNSat)** para el firmware RadioLib que portea pwncube.

## Resumen

- **FlatSat (ElectronicCats)** — firmware Zephyr RTOS (`flat-sat-fw-interno`).
  CCSDS seguro: **secondary header de 4 B (timestamp) + cifrado de payload por
  niveles (ninguno / XOR / AES-128-CTR) + CRC-16-CCITT**. Rechaza cualquier TC
  sin secondary header.
- **FlatSat (PWNSat)** — firmware Arduino/RadioLib (`FlatSat_Firmware`), el que
  portea pwncube. **CCSDS pelado: primary header de 6 B + payload en claro.** Sin
  secondary header, sin CRC, sin cifrado.
- **No son compatibles a nivel de wire.** Un frame construido por uno es
  malinterpretado por el otro. (Ya verificado en hardware.)
- **Esta rama implementa ambos estilos del lado pwncube:** el camino en claro
  (compatible PWNSat, conserva las vulns del CTF) y una librería de *TC seguro*
  (`applications/ccsds/`) ahora **alineada byte-a-byte con FlatSat
  (ElectronicCats)** — AES-128-CTR + secondary header timestamp + CRC-sobre-
  plaintext — así que pwncube puede construir y descifrar frames nativos de
  ElectronicCats (con el mismo nivel de dificultad). Ver "Qué implementa esta rama".

## Estándares CCSDS: qué son SPP y TC

Ambos firmwares dicen hablar CCSDS. Hay dos estándares distintos en juego, y la
distinción importa para la comparación de abajo.

### SPP — Space Packet Protocol (CCSDS 133.0-B-2)

El **formato de paquete de capa de aplicación**. Define el "space packet"
autodescriptivo: un **Primary Header de 6 octetos** seguido de un **Secondary
Header** opcional y un **Packet Data Field**. El primary header lleva:

| Campo | Bits | Significado |
|-------|------|-------------|
| Packet Version Number | 3 | `000` |
| Packet Type | 1 | 0 = telemetría (TM), 1 = telecomando (TC) |
| Secondary Header Flag | 1 | 1 = hay secondary header presente |
| APID | 11 | Application Process ID — rutea el paquete a un subsistema |
| Sequence Flags | 2 | 11 = unsegmentado, 01/10 = primer/último segmento |
| Packet Sequence Count | 14 | contador por APID |
| Packet Data Length | 16 | (longitud del data field en octetos) − 1 |

SPP es agnóstico del transporte: el mismo paquete puede ir sobre un enlace RF, un
enlace USB o un bus a bordo. Su campo opcional al final, **Packet Error Control**,
es un CRC-16-CCITT. Ambas variantes de FlatSat usan este mismo primary header —
esa parte es estándar e idéntica.

### TC — Telecommand

"TC" en CCSDS es una **familia** de estándares para el uplink (tierra →
satélite), por debajo del space packet:

- **TC Space Data Link Protocol (CCSDS 232.0-B)** — TC Transfer Frames, canales
  virtuales, el protocolo FARM/COP-1 de aceptación de comandos.
- **TC Synchronization and Channel Coding (CCSDS 231.0-B)** — CLTUs, código BCH,
  randomización en la capa física.
- **Space Data Link Security, SDLS (CCSDS 355.0-B)** — la forma *estándar* de
  añadir autenticación y cifrado (un security header + trailer con un MAC) a esos
  transfer frames.

Ningún firmware de FlatSat implementa el stack 232/231 completo (ponen space
packets directo sobre un PHY LoRa crudo). Donde difieren es en **seguridad**:
FlatSat (PWNSat) manda el TC space packet en claro; FlatSat (ElectronicCats)
envuelve un esquema de seguridad casero (timestamp + cifrado por niveles + CRC)
*inspirado* en SDLS pero que **no** es SDLS (sin MAC real, clave fija). Este
documento, y la librería de esta rama, replican a propósito el esquema de
ElectronicCats en vez del SDLS completo, y marcan exactamente dónde es más débil
que el estándar.

## Formatos de frame, lado a lado

```
FlatSat (PWNSat)  — SPP en claro
┌───────────────────────┬──────────────────────┐
│  Primary header (6)   │  payload (en claro)   │
└───────────────────────┴──────────────────────┘

FlatSat (ElectronicCats) — SPP seguro
┌───────────────────────┬───────────────┬───────────────────────┬────────┐
│  Primary header (6)    │ sec-hdr (4)   │  payload               │ CRC(2) │
│  type=TC, sec_hdr=1    │ timestamp u32 │  ninguno/XOR/AES-128   │ CCITT  │
└───────────────────────┴───────────────┴───────────────────────┴────────┘
                                 └── se usa como IV de AES-CTR
```

Ambos comparten el **primary header CCSDS 133.0-B idéntico** (6 octetos,
big-endian: version, type, flag de sec-hdr, APID, seq flags, seq count,
packet-data-length). Todo lo que va **después** del primary header es donde
divergen.

## Dónde cambia el código

### 1. Primary header — idéntico

Ambos arman el mismo header de 6 B. PWNSat/pwncube:

```c
// src/mcu/.../applications/spp.c  (spp_build_packet)
packet_id |= (type & 0x01) << 12;          // TM/TC
packet_id |= (sec_header & 0x01) << 11;     // flag de secondary header
packet_id |= (apid & 0x07FF);
pkt->header.length = spp_host_to_be16(data_len + sec_header_len - 1);
```

ElectronicCats parsea el mismo layout de bits:

```c
// flat-sat-fw-interno/flatsat/src/main.c  (process_incoming_telecommand)
uint16_t packet_id  = (data[0] << 8) | data[1];
uint8_t  pkt_type   = (packet_id >> 12) & 0x1;
uint8_t  sec_hdr    = (packet_id >> 11) & 0x1;
uint16_t apid       = packet_id & 0x7FF;
```

### 2. Secondary header — timestamp (ElectronicCats) vs ninguno (PWNSat)

**PWNSat** nunca emite secondary header; el payload empieza en el byte 6.

**ElectronicCats** pone un timestamp de 4 B en los bytes 6–9, y **lo usa como IV
del cifrado**:

```c
// flat-sat-fw-interno/flatsat/src/main.c
// Extract timestamp from secondary header (big-endian)
uint32_t timestamp = (data[6] << 24) | (data[7] << 16) | (data[8] << 8) | data[9];
// Payload is from data[10] to data[total_len - 3]
uint16_t payload_len = total_len - 12;      // 6 primary + 4 sec-hdr + 2 CRC
```

### 3. Cifrado del payload — por niveles (ElectronicCats) vs ninguno (PWNSat)

**PWNSat** no tiene crypto: `command_apid_handler` lee `pkt->data[0]`,
`pkt->data[1]` directo como argumentos del comando.

**ElectronicCats** cifra el payload según un nivel de *dificultad* en runtime
(`flatsat_difficulty`, `0 = training`), con clave sobre el IV del timestamp:

```c
// flat-sat-fw-interno/flatsat/src/main.c
if (flatsat_difficulty == 2) {                      // cifrado XOR
    const char *xor_key = "PWNSAT";
    for (uint16_t i = 0; i < payload_len; i++)
        data[10 + i] ^= xor_key[i % 6];
} else if (flatsat_difficulty >= 3) {               // AES-128-CTR
    uint8_t iv[16] = {0};
    iv[0] = (timestamp >> 24) & 0xFF; /* ...iv[1..3] = timestamp... */
    uint8_t round_keys[176];
    aes_key_expansion((const uint8_t *)"PWNSAT_K3Y_2026!", round_keys);
    // por bloque: block_iv[12..15] = contador; keystream = AES(block_iv);
    //             data[10+i] ^= keystream[...]
}
```

- Nivel 0/1: en claro. Nivel 2: XOR con `"PWNSAT"`. Nivel ≥3: **AES-128-CTR**,
  clave `"PWNSAT_K3Y_2026!"`, IV = `timestamp || contador_de_bloque`.
- El núcleo AES-128 es una implementación desde cero en el mismo archivo
  (`aes_key_expansion`, `aes128_encrypt_block`, …).

### 4. CRC — presente sobre plaintext (ElectronicCats) vs ausente (PWNSat)

**PWNSat** no añade CRC (y el uplink de pwncube corre con el CRC del PHY LoRa
apagado también).

**ElectronicCats** añade un CRC-16-CCITT y lo verifica **después de descifrar**
(sobre el frame en claro):

```c
// flat-sat-fw-interno/flatsat/src/main.c
uint16_t expected_crc = ccsds_crc16(data, total_len - 2);   // sobre frame descifrado
uint16_t actual_crc   = (data[total_len - 2] << 8) | data[total_len - 1];
if (expected_crc != actual_crc) { flat_sat.error_count++; return; }
```

### 5. Enforcement — obligatorio (ElectronicCats) vs se acepta cualquiera (pwncube)

**ElectronicCats rechaza** cualquier TC que no sea un paquete seguro:

```c
// flat-sat-fw-interno/flatsat/src/main.c
// Telecommands must have pkt_type == 1 and sec_hdr == 1
if (pkt_type != 1 || sec_hdr != 1) { return; }
```

**pwncube NO exige** su capa segura — un TC en claro (`sec_hdr=0`) igual se
despacha, y un CRC malo se ignora (esta es la debilidad intencional de sin-auth
del CTF):

```c
// src/mcu/.../applications/command_service.c  (process_rx_packet)
ccsds_tc_sec_header_t sh; int crc_ok = 0;
(void)ccsds_tc_unsecure(&pkt, &sh, &crc_ok);   // return + crc_ok ignorados
command_apid_handler(&pkt);
```

## Qué implementa esta rama

La librería de TC seguro de esta rama (`applications/ccsds/ccsds_tc.c` +
`ccsds_aes.c`) está **alineada byte-a-byte con FlatSat (ElectronicCats)** — mismo
secondary header, mismos cifrados, misma clave, mismo orden de CRC:

| Campo | FlatSat (ElectronicCats) | FlatSat (PWNSat) | lib segura pwncube (esta rama) |
|-------|--------------------------|------------------|--------------------------------|
| Primary header | 6 B CCSDS | 6 B CCSDS | 6 B CCSDS |
| Secondary header | 4 B **timestamp** (=IV) | ninguno | 4 B **timestamp** (=IV) ✅ |
| Cifrado | ninguno / XOR `PWNSAT` / **AES-128-CTR** | ninguno | ninguno / XOR `PWNSAT` / **AES-128-CTR** ✅ |
| Clave | `PWNSAT_K3Y_2026!` | — | `PWNSAT_K3Y_2026!` ✅ |
| CRC-16-CCITT | sí, sobre **plaintext** | no | sí, sobre **plaintext** ✅ |
| Niveles de dificultad | 0/1 plano, 2 XOR, ≥3 AES | n/a | 0/1 plano, 2 XOR, ≥3 AES ✅ |
| ¿Exige sec-hdr? | **sí** (rechaza planos) | n/a | **no** (acepta planos) — vuln CTF |

El núcleo AES-128 es un port byte-a-byte del de ElectronicCats (verificado contra
el vector NIST FIPS-197), el IV de CTR es `timestamp || 0×8 || índice_de_bloque`,
y el CRC-16 se calcula sobre el frame en claro — así que un frame construido por
`ccsds_tc_build()` lo acepta el `process_incoming_telecommand()` real de
ElectronicCats y viceversa, **siempre que ambos extremos elijan el mismo nivel de
dificultad** (`ccsds_tc_set_difficulty()`; el nivel es out-of-band, igual que en
el FlatSat). La única divergencia deliberada es la última fila: pwncube **no**
exige la capa (sigue aceptando frames planos PWNSat e ignora un CRC malo) — esa es
la vuln intencional de sin-auth, y es lo que deja a esta rama hablar **ambos**
perfiles a la vez.

El test host `applications/ccsds/test/tc_test.c` replica la lógica exacta del
receptor ElectronicCats para probar la aceptación.

## Parámetros de RF / PHY

| Parámetro | FlatSat (ElectronicCats) | FlatSat (PWNSat) / uplink pwncube |
|-----------|--------------------------|-----------------------------------|
| Frecuencia (default) | 915 MHz | 918 MHz |
| Ancho de banda | 125 kHz | 250 kHz |
| Spreading factor | 7 | 7 |
| Coding rate | 4/5 | 4/5 |
| Preamble | 12 | 8 |
| Sync word | private (0x1424) | private (0x1424) |
| CRC LoRa | on | off |

Los params de PHY también difieren, así que los dos ni siquiera se demodulan
hasta que un lado se reconfigura. ElectronicCats es totalmente reconfigurable
desde su `Cat-Shell` (`lora_freq/sf/bw/cr/preamble/syncword` + `lora_apply`); el
uplink de pwncube es 918/BW250/SF7/CR4-5 (`mission.h`). Alinear el PHY es
necesario pero no suficiente — los formatos de frame de arriba siguen difiriendo.

## Implicaciones de interop (qué significa "recibir" de verdad)

- **ElectronicCats ↔ PWNSat: no compatibles a nivel de wire.** Incluso en
  dificultad 0 (payload en claro), ElectronicCats igual emite un secondary header
  de 4 B (timestamp) y un CRC de 2 B que el parser PWNSat/pwncube no espera. Las
  dificultades más altas añaden XOR/AES encima.
- **pwncube ↔ ElectronicCats: ahora compatibles a nivel wire** (esta rama). Con la
  librería alineada (AES-128-CTR + timestamp) y el mismo nivel de dificultad en
  ambos extremos, un frame construido por `ccsds_tc_build()` de pwncube lo acepta
  el `process_incoming_telecommand()` de ElectronicCats, y la crypto de pwncube
  descifra frames nativos de ElectronicCats. Primero hay que alinear el PHY (ver
  la tabla RF). pwncube sigue aceptando frames planos PWNSat también, así que habla
  ambos.
- **Verificado en hardware (TM cifrado nativo).** Un FlatSat (ElectronicCats) en
  rol **SAT** con difficulty 3 transmitió su telemetría AES-128-CTR (APID 0x01F)
  por RF a 918 MHz; la radio de pwncube la recibió (`crc=ok`, RSSI −87) y la
  librería alineada la descifró a valores reales de sensores (temp 29.8 °C, press
  81854 Pa, accel Z ≈ 1 g, **batería 4200 mV — coincide con el estado que el
  propio FlatSat reportó**). Dos frames con 5 s de diferencia llevaban timestamps
  distintos → IVs distintos → ciphertext distinto, ambos descifrando consistente.
  Nota: el frame TM calcula su CRC sobre el **ciphertext** (verifica-luego-
  descifra), mientras el TC lo calcula sobre el **plaintext** (descifra-luego-
  verifica) — un matiz por-dirección del firmware ElectronicCats.

Ver también: `applications/ccsds/README.md` (la librería de TC seguro),
`docs/vulnerability-comparison.md` (vulns compartidas/porteadas),
`docs/flatsat-port-changes.md`.
