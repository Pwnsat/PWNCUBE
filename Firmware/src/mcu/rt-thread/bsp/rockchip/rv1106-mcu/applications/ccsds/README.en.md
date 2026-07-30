# CCSDS SPP + secured TC library

A small, self-contained CCSDS library for the CubeSat firmware. It provides the
Space Packet Protocol (SPP) primary header codec and a **secured telecommand
(TC)** layer **aligned to FlatSat (ElectronicCats)** — a timestamp secondary
header + tier-selectable encryption (plaintext / XOR / AES-128-CTR) + CRC-16 —
layered on top of the SPP frame.

> The AES-128 core, the CTR IV construction, the difficulty tiers and the
> CRC-over-plaintext order are a **byte-exact match** of FlatSat (ElectronicCats)
> firmware, so frames built here are accepted by that firmware and vice-versa
> (given the same difficulty tier). For the full three-way protocol comparison
> (ElectronicCats vs PWNSat vs this library) see
> `docs/flatsat-protocol-comparison.md` (at the repo root).

> This is a **CTF exercise**. The crypto is real and standard, but it is used with
> **intentional weaknesses** (fixed key, low-entropy IV, no enforcement) so the
> secured link can be broken. See "Intentional weaknesses". Do not reuse as-is.

## Files

| File | Role |
|------|------|
| `../spp.h` / `../spp.c` | SPP primary header (6 B, big-endian), TM/TC/idle builders, `spp_unpack_packet` (parse). Carries the deliberate over-read (vuln #9). |
| `ccsds_crc.h` / `ccsds_crc.c` | CRC-16-CCITT (poly 0x1021, init 0xFFFF), the CCSDS Packet Error Control field. |
| `ccsds_aes.h` / `ccsds_aes.c` | AES-128 (byte-exact port of the ElectronicCats core) + CTR-mode keystream XOR. Holds the fixed mission key `"PWNSAT_K3Y_2026!"`. |
| `ccsds_tc.h` / `ccsds_tc.c` | Secured TC: build (`ccsds_tc_build`), in-place decrypt/verify (`ccsds_tc_unsecure`), difficulty selector. |
| `ccsds_133_space_packet.*`, `ccsds_types.h` | Earlier standalone CCSDS model (unused by the mission; kept as reference). |

## Secured TC wire format

```
+------------------+-----------------+---------------------+--------+
| SPP primary (6)  | sec-hdr: TS (4) | payload             | CRC(2) |
+------------------+-----------------+---------------------+--------+
   type=TC             timestamp u32     encrypted per        CRC-16
   sec_hdr_flag=1      (big-endian)      difficulty tier      CCITT
   APID (11)           = AES-CTR IV      (no padding)
```

- `packet_data_length` = `(4 + payload + 2) − 1`.
- The 32-bit timestamp secondary header is also the **AES-128-CTR IV**
  (`IV = timestamp || 0×8 || block_index`).
- Encryption tier (`ccsds_tc_set_difficulty`): `0/1` plaintext, `2` XOR `"PWNSAT"`,
  `≥3` AES-128-CTR. CTR/XOR are stream ciphers, so **no block padding**.
- **CRC-16-CCITT is computed over the PLAINTEXT frame** (primary + sec-hdr +
  plaintext payload), then the payload is encrypted. The receiver decrypts first,
  then verifies the CRC — this is the ElectronicCats order.

## API

```c
void ccsds_tc_set_difficulty(uint8_t level);   /* 0/1 plain, 2 XOR, >=3 AES */

/* Build a secured TC (timestamp sec-hdr + CRC over plaintext + encrypted payload). */
int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint32_t timestamp, const uint8_t *payload,
                   uint16_t payload_len, uint16_t *out_total);

/* Decrypt + verify in place. sec_hdr=1: reads timestamp (*timestamp_out),
 * decrypts per tier, reports CRC validity (*crc_ok, NOT enforced), rewrites
 * pkt->data to the plaintext args. sec_hdr=0: CCSDS_TC_ERR_NOSEC, pkt untouched. */
int ccsds_tc_unsecure(space_packet_t *pkt, uint32_t *timestamp_out, int *crc_ok);
```

Both ends must select the **same difficulty tier** (`ccsds_tc_set_difficulty`) —
the tier is out-of-band, exactly as on the real FlatSat.

### Mission integration

`command_service.c :: process_rx_packet` calls `ccsds_tc_unsecure` right after
`spp_unpack_packet`, then dispatches by APID. The return value and `crc_ok` are
**ignored** on purpose (see below).

## Intentional weaknesses (CTF)

The AES-128 is textbook-correct (verified against the NIST FIPS-197 vector). The
exploitable weaknesses are in how it is keyed and enforced:

1. **Fixed key, in the clear.** `CCSDS_TC_AES_KEY` (`ccsds_aes.c`) is one 128-bit
   key compiled in, ASCII `"PWNSAT_K3Y_2026!"`, so it appears verbatim in a flash
   image obtained through the FLASH telecommand (APID 0x07). Recover it → decrypt
   or forge any secured TC.
2. **Low-entropy IV, keystream reuse.** The CTR IV is just a 32-bit timestamp; the
   XOR tier is a repeating 6-byte key. Captured keystreams/known-plaintext break it.
3. **CRC is a check, not a MAC.** Tamper + recompute the CRC-16 trivially — no
   authentication.
4. **The receiver does not require the layer.** `process_rx_packet` accepts a
   plaintext TC (`sec_hdr=0`) and never rejects on CRC mismatch, so every
   pre-existing over-the-air exploit still works — the no-authentication vuln.

Preserved from the base implementation: **#9** SPP unpack over-read (`spp.c`),
**#10** broadcast length underflow (`command_service.c`), **#6** uninitialised
secondary header on the FLASH TM path.

## Testing

`test/tc_test.c` is a host test: it checks the AES-128 core against the NIST
FIPS-197 vector, round-trips build→unsecure for every difficulty tier, replays
the exact ElectronicCats receiver logic (AES-CTR decrypt + CRC-over-plaintext) to
prove a real FlatSat would accept our frame, and confirms a plaintext TC passes
through untouched. It lives in `test/` so the mission build (which globs
`ccsds/*.c`, non-recursive) never pulls its `main()` in. Run from this directory:

```sh
gcc -Wall -Wextra -I. -I.. -o /tmp/tc_test \
    test/tc_test.c ../spp.c ccsds_crc.c ccsds_aes.c ccsds_tc.c && /tmp/tc_test
```

## Building a different secured command (example)

```c
ccsds_tc_set_difficulty(CCSDS_TC_DIFF_AES);          /* match the peer's tier */
packet_counter_t cnt; spp_counters_init(&cnt);
uint8_t args[] = { 0x00, 0xC8 };                     /* thruster 0, power 200 */
space_packet_t pkt; uint16_t total;
ccsds_tc_build(&pkt, &cnt, SPP_APID_TC_SET_THRUSTER, /*timestamp=*/0x11223344,
               args, sizeof(args), &total);
/* transmit the first `total` bytes of `pkt` on the uplink */
```
