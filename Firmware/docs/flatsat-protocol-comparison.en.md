# FlatSat protocol comparison — ElectronicCats vs PWNSat

Two different FlatSat firmwares speak two **different, non-interoperable** CCSDS
profiles over LoRa. This document pins down exactly where the two wire formats
diverge, with the code that produces each, and how the pwncube RISC-V port (this
`ccsds-tc-library` branch) relates to both.

> **Naming.** Earlier notes called these "FlatSat Zephyr" and "FlatSat RadioLib".
> This document uses **FlatSat (ElectronicCats)** for the Zephyr firmware and
> **FlatSat (PWNSat)** for the RadioLib firmware pwncube ports.

## TL;DR

- **FlatSat (ElectronicCats)** — Zephyr RTOS firmware (`flat-sat-fw-interno`).
  Secured CCSDS: **4-byte timestamp secondary header + tier-selectable payload
  encryption (none / XOR / AES-128-CTR) + CRC-16-CCITT**. Rejects any TC without
  a secondary header.
- **FlatSat (PWNSat)** — Arduino/RadioLib firmware (`FlatSat_Firmware`), the one
  pwncube ports. **Bare CCSDS: 6-byte primary header + plaintext payload.** No
  secondary header, no CRC, no encryption.
- **They are not wire-compatible.** A frame built by one is misparsed by the
  other. (Already verified on hardware.)
- **This branch implements both styles on the pwncube side:** the plaintext path
  (PWNSat-compatible, still carries the CTF vulns) and a *secured TC* library
  (`applications/ccsds/`) now **aligned byte-for-byte with FlatSat
  (ElectronicCats)** — AES-128-CTR + timestamp secondary header + CRC-over-
  plaintext — so pwncube can build and decrypt native ElectronicCats frames
  (given the same difficulty tier). See "What this branch implements".

## CCSDS standards: what SPP and TC are

Both firmwares claim to speak CCSDS. Two distinct standards are involved, and the
distinction matters for the comparison below.

### SPP — Space Packet Protocol (CCSDS 133.0-B-2)

The **application-layer packet format**. It defines the self-describing "space
packet": a **6-octet Primary Header** followed by an optional **Secondary
Header** and a **Packet Data Field**. The primary header carries:

| Field | Bits | Meaning |
|-------|------|---------|
| Packet Version Number | 3 | `000` |
| Packet Type | 1 | 0 = telemetry (TM), 1 = telecommand (TC) |
| Secondary Header Flag | 1 | 1 = a secondary header is present |
| APID | 11 | Application Process ID — routes the packet to a subsystem |
| Sequence Flags | 2 | 11 = unsegmented, 01/10 = first/last segment |
| Packet Sequence Count | 14 | per-APID counter |
| Packet Data Length | 16 | (data field length in octets) − 1 |

SPP is transport-agnostic: the same packet can ride over an RF link, a USB link,
or an on-board bus. Its optional trailing **Packet Error Control** field is a
CRC-16-CCITT. Both FlatSat variants use this exact primary header — that part is
standards-compliant and identical.

### TC — Telecommand

"TC" in CCSDS is a **family** of standards for the uplink (ground → spacecraft),
layered *under* the space packet:

- **TC Space Data Link Protocol (CCSDS 232.0-B)** — TC Transfer Frames, virtual
  channels, the FARM/COP-1 command-acceptance protocol.
- **TC Synchronization and Channel Coding (CCSDS 231.0-B)** — CLTUs, BCH coding,
  randomization on the physical layer.
- **Space Data Link Security, SDLS (CCSDS 355.0-B)** — the *standard* way to add
  authentication and encryption (a security header + trailer with a MAC) to those
  transfer frames.

Neither FlatSat firmware implements the full 232/231 stack (they put space
packets straight onto a raw LoRa PHY). Where they differ is **security**: FlatSat
(PWNSat) sends the TC space packet in the clear; FlatSat (ElectronicCats) wraps a
home-grown security scheme (timestamp + tiered encryption + CRC) that is
*inspired by* SDLS but is **not** SDLS (no real MAC, fixed key). This document,
and this branch's library, deliberately mirror the ElectronicCats scheme rather
than full SDLS, and flag exactly where it is weaker than the standard.

## Frame formats, side by side

```
FlatSat (PWNSat)  — plaintext SPP
┌───────────────────────┬──────────────────────┐
│  Primary header (6)   │  payload (plaintext)  │
└───────────────────────┴──────────────────────┘

FlatSat (ElectronicCats) — secured SPP
┌───────────────────────┬───────────────┬───────────────────────┬────────┐
│  Primary header (6)    │ sec-hdr (4)   │  payload               │ CRC(2) │
│  type=TC, sec_hdr=1    │ timestamp u32 │  none/XOR/AES-128-CTR  │ CCITT  │
└───────────────────────┴───────────────┴───────────────────────┴────────┘
                                 └── used as the AES-CTR IV
```

Both share the **identical CCSDS 133.0-B primary header** (6 octets, big-endian:
version, type, sec-hdr flag, APID, seq flags, seq count, packet-data-length).
Everything after the primary header is where they diverge.

## Where the code differs

### 1. Primary header — identical

Both build the same 6-byte header. PWNSat/pwncube:

```c
// src/mcu/.../applications/spp.c  (spp_build_packet)
packet_id |= (type & 0x01) << 12;          // TM/TC
packet_id |= (sec_header & 0x01) << 11;     // secondary-header flag
packet_id |= (apid & 0x07FF);
pkt->header.length = spp_host_to_be16(data_len + sec_header_len - 1);
```

ElectronicCats parses the same bit layout:

```c
// flat-sat-fw-interno/flatsat/src/main.c  (process_incoming_telecommand)
uint16_t packet_id  = (data[0] << 8) | data[1];
uint8_t  pkt_type   = (packet_id >> 12) & 0x1;
uint8_t  sec_hdr    = (packet_id >> 11) & 0x1;
uint16_t apid       = packet_id & 0x7FF;
```

### 2. Secondary header — timestamp (ElectronicCats) vs none (PWNSat)

**PWNSat** never emits a secondary header; the payload starts at byte 6.

**ElectronicCats** puts a 4-byte timestamp at bytes 6–9, and **uses it as the
encryption IV**:

```c
// flat-sat-fw-interno/flatsat/src/main.c
// Extract timestamp from secondary header (big-endian)
uint32_t timestamp = (data[6] << 24) | (data[7] << 16) | (data[8] << 8) | data[9];
// Payload is from data[10] to data[total_len - 3]
uint16_t payload_len = total_len - 12;      // 6 primary + 4 sec-hdr + 2 CRC
```

### 3. Payload encryption — tiered (ElectronicCats) vs none (PWNSat)

**PWNSat** has no crypto: `command_apid_handler` reads `pkt->data[0]`,
`pkt->data[1]` directly as command arguments.

**ElectronicCats** encrypts the payload by a runtime *difficulty* level
(`flatsat_difficulty`, `0 = training`), keyed on the timestamp IV:

```c
// flat-sat-fw-interno/flatsat/src/main.c
if (flatsat_difficulty == 2) {                      // XOR cipher
    const char *xor_key = "PWNSAT";
    for (uint16_t i = 0; i < payload_len; i++)
        data[10 + i] ^= xor_key[i % 6];
} else if (flatsat_difficulty >= 3) {               // AES-128-CTR
    uint8_t iv[16] = {0};
    iv[0] = (timestamp >> 24) & 0xFF; /* ...iv[1..3] = timestamp... */
    uint8_t round_keys[176];
    aes_key_expansion((const uint8_t *)"PWNSAT_K3Y_2026!", round_keys);
    // per-block: block_iv[12..15] = block counter; keystream = AES(block_iv);
    //            data[10+i] ^= keystream[...]
}
```

- Level 0/1: plaintext. Level 2: XOR with `"PWNSAT"`. Level ≥3: **AES-128-CTR**,
  key `"PWNSAT_K3Y_2026!"`, IV = `timestamp || block_counter`.
- The AES-128 core is a from-scratch implementation in the same file
  (`aes_key_expansion`, `aes128_encrypt_block`, …).

### 4. CRC — present over plaintext (ElectronicCats) vs absent (PWNSat)

**PWNSat** appends no CRC (and pwncube's uplink runs with LoRa PHY CRC off too).

**ElectronicCats** appends a CRC-16-CCITT and verifies it **after decryption**
(over the plaintext frame):

```c
// flat-sat-fw-interno/flatsat/src/main.c
uint16_t expected_crc = ccsds_crc16(data, total_len - 2);   // over decrypted frame
uint16_t actual_crc   = (data[total_len - 2] << 8) | data[total_len - 1];
if (expected_crc != actual_crc) { flat_sat.error_count++; return; }
```

### 5. Enforcement — required (ElectronicCats) vs accepted-either-way (pwncube)

**ElectronicCats rejects** any TC that is not a secured packet:

```c
// flat-sat-fw-interno/flatsat/src/main.c
// Telecommands must have pkt_type == 1 and sec_hdr == 1
if (pkt_type != 1 || sec_hdr != 1) { return; }
```

**pwncube does not enforce** its secured layer — a plaintext TC (`sec_hdr=0`) is
still dispatched, and a bad CRC is ignored (this is the intentional no-auth CTF
weakness):

```c
// src/mcu/.../applications/command_service.c  (process_rx_packet)
ccsds_tc_sec_header_t sh; int crc_ok = 0;
(void)ccsds_tc_unsecure(&pkt, &sh, &crc_ok);   // return + crc_ok ignored
command_apid_handler(&pkt);
```

## What this branch implements

This branch's secured-TC library (`applications/ccsds/ccsds_tc.c` +
`ccsds_aes.c`) is **aligned byte-for-byte with FlatSat (ElectronicCats)** — same
secondary header, same ciphers, same key, same CRC order:

| Field | FlatSat (ElectronicCats) | FlatSat (PWNSat) | pwncube secured lib (this branch) |
|-------|--------------------------|------------------|-----------------------------------|
| Primary header | 6 B CCSDS | 6 B CCSDS | 6 B CCSDS |
| Secondary header | 4 B **timestamp** (=IV) | none | 4 B **timestamp** (=IV) ✅ |
| Encryption | none / XOR `PWNSAT` / **AES-128-CTR** | none | none / XOR `PWNSAT` / **AES-128-CTR** ✅ |
| Key | `PWNSAT_K3Y_2026!` | — | `PWNSAT_K3Y_2026!` ✅ |
| CRC-16-CCITT | yes, over **plaintext** | no | yes, over **plaintext** ✅ |
| Difficulty tiers | 0/1 plain, 2 XOR, ≥3 AES | n/a | 0/1 plain, 2 XOR, ≥3 AES ✅ |
| Requires sec-hdr? | **yes** (rejects plaintext) | n/a | **no** (accepts plaintext) — CTF vuln |

The AES-128 core is a byte-exact port of the ElectronicCats implementation
(verified against the NIST FIPS-197 vector), the CTR IV is
`timestamp || 0×8 || block_index`, and the CRC-16 is computed over the plaintext
frame — so a frame built by `ccsds_tc_build()` is accepted by the real
ElectronicCats `process_incoming_telecommand()` and vice-versa, **provided both
ends select the same difficulty tier** (`ccsds_tc_set_difficulty()`; the tier is
out-of-band, exactly as on the FlatSat). The one deliberate divergence is the
last row: pwncube does **not** enforce the layer (it still accepts plaintext
PWNSat frames and ignores a bad CRC) — that is the intentional no-auth CTF vuln,
and it is what lets this branch speak **both** profiles at once.

The host test `applications/ccsds/test/tc_test.c` replays the exact
ElectronicCats receiver logic to prove acceptance.

## RF / PHY parameters

| Parameter | FlatSat (ElectronicCats) | FlatSat (PWNSat) / pwncube uplink |
|-----------|--------------------------|-----------------------------------|
| Frequency (default) | 915 MHz | 918 MHz |
| Bandwidth | 125 kHz | 250 kHz |
| Spreading factor | 7 | 7 |
| Coding rate | 4/5 | 4/5 |
| Preamble | 12 | 8 |
| Sync word | private (0x1424) | private (0x1424) |
| LoRa CRC | on | off |

The PHY params differ too, so the two do not even demodulate each other until one
side is reconfigured. ElectronicCats is fully reconfigurable from its `Cat-Shell`
(`lora_freq/sf/bw/cr/preamble/syncword` + `lora_apply`); pwncube's uplink is
918/BW250/SF7/CR4-5 (`mission.h`). Aligning the PHY is necessary but not
sufficient — the frame formats above still differ.

## Interop implications (what "receiving" really means)

- **ElectronicCats ↔ PWNSat: not wire-compatible.** Even at difficulty 0
  (plaintext payload), ElectronicCats still emits a 4-byte timestamp secondary
  header and a 2-byte CRC that the PWNSat/pwncube parser does not expect. Higher
  difficulties add XOR/AES on top.
- **pwncube ↔ ElectronicCats: now wire-compatible** (this branch). With the
  library aligned (AES-128-CTR + timestamp) and the same difficulty tier on both
  ends, a frame built by pwncube's `ccsds_tc_build()` is accepted by
  ElectronicCats' `process_incoming_telecommand()`, and pwncube's crypto decrypts
  native ElectronicCats frames. The PHY must be aligned first (see the RF table).
  pwncube keeps accepting plaintext PWNSat frames too, so it speaks both.
- **Verified on hardware (native encrypted TM).** A FlatSat (ElectronicCats) in
  **SAT** role at difficulty 3 transmitted its AES-128-CTR telemetry (APID 0x01F)
  over RF at 918 MHz; pwncube's radio received it (`crc=ok`, RSSI −87) and the
  aligned library decrypted it to real sensor values (temp 29.8 °C, press
  81854 Pa, accel Z ≈ 1 g, **battery 4200 mV — matching the FlatSat's own reported
  state**). Two frames 5 s apart carried different timestamps → different IVs →
  different ciphertext, both decrypting consistently. Note: the TM frame computes
  its CRC over the **ciphertext** (verify-then-decrypt), whereas the TC frame
  computes it over the **plaintext** (decrypt-then-verify) — a per-direction quirk
  of the ElectronicCats firmware.

See also: `applications/ccsds/README.md` (the secured-TC library),
`docs/vulnerability-comparison.md` (shared/ported vulns), `docs/flatsat-port-changes.md`.
