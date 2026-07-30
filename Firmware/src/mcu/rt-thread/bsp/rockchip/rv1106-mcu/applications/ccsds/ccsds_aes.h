/*
 * ccsds_aes — AES-128 + CTR mode for the FlatSat (ElectronicCats) TC security
 * layer.
 *
 * This is a byte-exact port of the AES-128 core and the CTR construction used by
 * FlatSat (ElectronicCats) firmware (flat-sat-fw-interno, main.c): standard
 * AES-128 block cipher (SubBytes/ShiftRows/MixColumns/AddRoundKey, 10 rounds),
 * used as a keystream generator in counter (CTR) mode. Matching it exactly is
 * what lets pwncube decrypt/produce native ElectronicCats secured telecommands.
 *
 * INTENTIONAL WEAKNESS (CTF): the mission key CCSDS_TC_AES_KEY is FIXED and
 * compiled into the firmware ("PWNSAT_K3Y_2026!" in ASCII), so it is recoverable
 * from a flash dump (FLASH telecommand, APID 0x07). Real flight software would
 * use CCSDS SDLS (355.0-B) with managed, rotated keys.
 */

#ifndef CCSDS_AES_H
#define CCSDS_AES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCSDS_AES_BLOCK     16U   /* AES block size (bytes)            */
#define CCSDS_AES_KEYLEN    16U   /* AES-128 key length (bytes)        */
#define CCSDS_AES_RKLEN    176U   /* expanded round-key schedule bytes */

/* The fixed mission key ("PWNSAT_K3Y_2026!"). Defined in ccsds_aes.c. */
extern const uint8_t CCSDS_TC_AES_KEY[CCSDS_AES_KEYLEN];

/* Expand a 128-bit key into the 11-round (176-byte) schedule. */
void ccsds_aes128_key_expansion(const uint8_t key[16], uint8_t round_keys[176]);

/* Encrypt one 16-byte block (ECB primitive; used to make the CTR keystream). */
void ccsds_aes128_encrypt_block(const uint8_t in[16],
                                const uint8_t round_keys[176], uint8_t out[16]);

/*
 * AES-128-CTR keystream XOR over `buf` (encrypt == decrypt). The 128-bit counter
 * block is IV = timestamp(4 B, big-endian) || 0x00 * 8 || block_index(4 B,
 * big-endian) — the exact construction used by FlatSat (ElectronicCats). Any
 * payload length is allowed (no block padding).
 */
void ccsds_aes128_ctr_xcrypt(uint8_t *buf, size_t len,
                             const uint8_t key[16], uint32_t timestamp);

#ifdef __cplusplus
}
#endif

#endif /* CCSDS_AES_H */
