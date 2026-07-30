/*
 * ccsds_tc — Secured Telecommand layer, aligned to FlatSat (ElectronicCats).
 *
 * This is the exact secured-TC frame that FlatSat (ElectronicCats) accepts
 * (flat-sat-fw-interno, process_incoming_telecommand), layered on the CCSDS
 * 133.0-B Space Packet primary header (spp.h):
 *
 *   +------------------+-----------------+---------------------+--------+
 *   | SPP primary (6)  | sec-hdr: TS (4) | payload             | CRC(2) |
 *   +------------------+-----------------+---------------------+--------+
 *      type=TC, sec_hdr=1   timestamp u32    encrypted per        CRC-16
 *                           (big-endian)     difficulty tier      CCITT
 *                                └── used as the AES-128-CTR IV
 *
 * CCSDS mapping:
 *   - Primary header: CCSDS 133.0-B-2 Packet Primary Header (6 octets).
 *   - Secondary header: a 32-bit time code (seconds) — CCSDS 133.0-B permits a
 *     user-defined secondary header; here it doubles as the cipher IV.
 *   - CRC: CRC-16-CCITT (poly 0x1021, init 0xFFFF) — the CCSDS Packet Error
 *     Control field, computed over the PLAINTEXT frame (primary+sec-hdr+payload).
 *   - Encryption: NOT standard CCSDS; a stand-in for CCSDS SDLS (355.0-B). Tier
 *     is selected by ccsds_tc_set_difficulty():
 *        0,1 = plaintext, 2 = XOR "PWNSAT", >=3 = AES-128-CTR (key in ccsds_aes.c).
 *
 * Build order (so the receiver's decrypt->CRC check passes): assemble the
 * PLAINTEXT frame, compute the CRC over it, append the CRC, THEN encrypt only
 * the payload region. Decrypt reverses it: decrypt the payload, then verify the
 * CRC over the recovered plaintext.
 *
 * INTENTIONAL WEAKNESSES (CTF) — the crypto is real but broken by design:
 *   - Fixed AES key in .rodata (recoverable via the FLASH leak); ECB-derived
 *     CTR keystream keyed only on a low-entropy timestamp.
 *   - The receiver (command_service.c) does NOT enforce the layer: a plaintext
 *     TC (sec_hdr=0) is still dispatched and a bad CRC is ignored, so every
 *     plaintext PWNSat exploit still works. This is the no-auth vuln.
 */

#ifndef CCSDS_TC_H
#define CCSDS_TC_H

#include <stdint.h>
#include "spp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CCSDS_TC_SECHDR_LEN   4U   /* 32-bit timestamp / IV */
#define CCSDS_TC_CRC_LEN      2U   /* CRC-16-CCITT trailer  */

/* Difficulty tiers — mirror FlatSat (ElectronicCats) flatsat_difficulty. */
#define CCSDS_TC_DIFF_TRAINING  0U   /* plaintext payload            */
#define CCSDS_TC_DIFF_PLAIN     1U   /* plaintext payload            */
#define CCSDS_TC_DIFF_XOR       2U   /* XOR with "PWNSAT"            */
#define CCSDS_TC_DIFF_AES       3U   /* AES-128-CTR (>=3)           */

/* Return codes (in addition to the SPP_ERROR_* negatives from spp.h). */
#define CCSDS_TC_OK           0    /* secured TC parsed/decrypted   */
#define CCSDS_TC_ERR_NOSEC    1    /* plaintext TC (no sec header)  */

/* Select the encryption tier used by build/unsecure. Both ends must agree
 * (the tier is out-of-band, exactly as on the real FlatSat). Default: AES. */
void    ccsds_tc_set_difficulty(uint8_t level);
uint8_t ccsds_tc_get_difficulty(void);

/*
 * Build a secured TC into `pkt`: SPP primary (type=TC, sec_hdr=1), 4-byte
 * timestamp secondary header, CRC-16 over the plaintext frame, then the payload
 * encrypted per the current difficulty tier (IV = timestamp). The TC counter in
 * `cnt` is advanced. Full on-wire length is returned via *out_total.
 * Returns SPP_ERROR_NONE, or a negative SPP_ERROR_* on bad args / overflow.
 */
int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint32_t timestamp, const uint8_t *payload,
                   uint16_t payload_len, uint16_t *out_total);

/*
 * In-place "unsecure" of a received, already-SPP-unpacked TC.
 *
 * sec_hdr=1: reads the timestamp (into *timestamp_out if non-NULL), decrypts the
 * payload per the current difficulty tier (IV = timestamp), verifies the CRC over
 * the recovered plaintext (reported via *crc_ok if non-NULL; NOT enforced), and
 * rewrites pkt->data to hold only the plaintext command args (fixing
 * pkt->header.length). Returns CCSDS_TC_OK.
 *
 * sec_hdr=0: leaves `pkt` untouched, returns CCSDS_TC_ERR_NOSEC.
 * Malformed (too short): negative SPP_ERROR_*.
 */
int ccsds_tc_unsecure(space_packet_t *pkt, uint32_t *timestamp_out, int *crc_ok);

#ifdef __cplusplus
}
#endif

#endif /* CCSDS_TC_H */
