/* Host test for the CCSDS secured-TC library (ElectronicCats-aligned). */
#include <stdio.h>
#include <string.h>
#include "spp.h"
#include "ccsds_tc.h"
#include "ccsds_aes.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                              else printf("ok:   %s\n", msg); } while (0)

/* Replicate FlatSat (ElectronicCats) process_incoming_telecommand acceptance:
 * decrypt the payload with the tier, then CRC over the recovered plaintext. */
extern uint16_t ccsds_crc16_ccitt(const uint8_t *data, size_t size);

int main(void)
{
    /* 1) AES-128 core vs NIST FIPS-197 test vector (proves it is standard AES). */
    {
        uint8_t key[16], pt[16], out[16], rk[176];
        for (int i = 0; i < 16; i++) { key[i] = i; pt[i] = (uint8_t)(i*0x11); }
        static const uint8_t expect[16] = {
            0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
            0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
        ccsds_aes128_key_expansion(key, rk);
        ccsds_aes128_encrypt_block(pt, rk, out);
        CHECK(memcmp(out, expect, 16) == 0, "AES-128 matches NIST FIPS-197 vector");
    }

    /* 2) Round-trip build -> unsecure for each difficulty tier. */
    uint8_t payload[] = { 0x01, 0xC8, 0x03, 0x94, 0x2A };   /* odd length: 5 */
    uint8_t tiers[] = { CCSDS_TC_DIFF_PLAIN, CCSDS_TC_DIFF_XOR, CCSDS_TC_DIFF_AES, 5 };
    for (unsigned t = 0; t < sizeof(tiers); t++) {
        ccsds_tc_set_difficulty(tiers[t]);
        packet_counter_t cnt; spp_counters_init(&cnt);
        space_packet_t pkt; uint16_t total = 0;
        uint32_t ts = 0x11223344u;
        int r = ccsds_tc_build(&pkt, &cnt, 0x04, ts, payload, sizeof(payload), &total);
        char m[64];
        snprintf(m, sizeof(m), "diff=%u: build OK", tiers[t]);
        CHECK(r == SPP_ERROR_NONE, m);
        /* wire = 6 primary + 4 ts + 5 payload + 2 crc = 17 (no block padding) */
        snprintf(m, sizeof(m), "diff=%u: wire length is 17 (no padding)", tiers[t]);
        CHECK(total == 17, m);

        /* ciphertext differs from plaintext for encrypting tiers */
        int enc = (tiers[t] >= CCSDS_TC_DIFF_XOR);
        int differs = memcmp(pkt.data + 4, payload, sizeof(payload)) != 0;
        snprintf(m, sizeof(m), "diff=%u: payload %s", tiers[t], enc ? "encrypted" : "plaintext");
        CHECK(enc ? differs : !differs, m);

        space_packet_t rx = pkt; uint32_t ts_out = 0; int crc_ok = -1;
        r = ccsds_tc_unsecure(&rx, &ts_out, &crc_ok);
        snprintf(m, sizeof(m), "diff=%u: unsecure OK + CRC valid", tiers[t]);
        CHECK(r == CCSDS_TC_OK && crc_ok == 1, m);
        snprintf(m, sizeof(m), "diff=%u: timestamp round-trips", tiers[t]);
        CHECK(ts_out == ts, m);
        snprintf(m, sizeof(m), "diff=%u: decrypted args match plaintext", tiers[t]);
        CHECK(memcmp(rx.data, payload, sizeof(payload)) == 0, m);
    }

    /* 3) An ElectronicCats receiver would accept our AES frame: replicate its
     *    decrypt (AES-CTR, IV=timestamp) then CRC-over-plaintext check. */
    {
        ccsds_tc_set_difficulty(CCSDS_TC_DIFF_AES);
        packet_counter_t cnt; spp_counters_init(&cnt);
        space_packet_t pkt; uint16_t total = 0;
        uint32_t ts = 0xDEADBEEFu;
        ccsds_tc_build(&pkt, &cnt, 0x04, ts, payload, sizeof(payload), &total);

        uint8_t *d = (uint8_t *)&pkt;
        uint16_t total_len = total;
        uint32_t rx_ts = (d[6]<<24)|(d[7]<<16)|(d[8]<<8)|d[9];
        uint16_t plen = total_len - 12;
        ccsds_aes128_ctr_xcrypt(d + 10, plen, CCSDS_TC_AES_KEY, rx_ts);   /* decrypt */
        uint16_t exp_crc = ccsds_crc16_ccitt(d, total_len - 2);
        uint16_t act_crc = (d[total_len-2] << 8) | d[total_len-1];
        CHECK(exp_crc == act_crc, "ElectronicCats-style decrypt+CRC accepts our frame");
        CHECK(memcmp(d + 10, payload, sizeof(payload)) == 0,
              "ElectronicCats-style decrypt recovers our plaintext");
    }

    /* 4) A plaintext TC (no secondary header) is left untouched. */
    {
        ccsds_tc_set_difficulty(CCSDS_TC_DIFF_AES);
        packet_counter_t cnt; spp_counters_init(&cnt);
        space_packet_t plain; uint8_t a[] = { 0x00, 0x2A };
        spp_tc_build_packet(&plain, &cnt, SPP_GROUP_FLAG_UNSEGMENTED,
                            SPP_SECHEAD_FLAG_NOPRESENT, 0, 0x04, a, 2);
        space_packet_t copy = plain;
        int r = ccsds_tc_unsecure(&plain, NULL, NULL);
        CHECK(r == CCSDS_TC_ERR_NOSEC, "plaintext TC reported as NOSEC");
        CHECK(memcmp(&plain, &copy, sizeof(plain)) == 0,
              "plaintext TC left untouched (PWNSat exploits still work)");
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
