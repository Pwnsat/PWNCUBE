/*
 * ccsds_tc — Secured TC aligned to FlatSat (ElectronicCats). See ccsds_tc.h.
 *
 * Built on the live SPP model (space_packet_t): the packed 6-byte header is
 * immediately followed by data[], so (uint8_t*)pkt is the raw on-wire frame and
 * CRC/cipher operate on it in place with no copies.
 */

#include "ccsds_tc.h"
#include "ccsds_crc.h"
#include "ccsds_aes.h"
#include <string.h>

/* Encryption tier (out-of-band; both ends must agree). Default: AES. */
static uint8_t s_difficulty = CCSDS_TC_DIFF_AES;

void ccsds_tc_set_difficulty(uint8_t level) { s_difficulty = level; }
uint8_t ccsds_tc_get_difficulty(void)       { return s_difficulty; }

/*
 * Apply the current cipher tier to the payload in place. XOR and CTR are both
 * involutions (self-inverse), so this same call both encrypts and decrypts.
 */
static void apply_cipher(uint8_t *payload, uint16_t len, uint32_t timestamp)
{
    if (s_difficulty == CCSDS_TC_DIFF_XOR) {
        static const char xor_key[6] = { 'P', 'W', 'N', 'S', 'A', 'T' };
        for (uint16_t i = 0; i < len; i++)
            payload[i] ^= (uint8_t)xor_key[i % 6];
    } else if (s_difficulty >= CCSDS_TC_DIFF_AES) {
        ccsds_aes128_ctr_xcrypt(payload, len, CCSDS_TC_AES_KEY, timestamp);
    }
    /* tiers 0/1: plaintext, no-op */
}

int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint32_t timestamp, const uint8_t *payload,
                   uint16_t payload_len, uint16_t *out_total)
{
    if (pkt == NULL)
        return SPP_ERROR_INVALID_BUFFER;

    uint16_t data_field = (uint16_t)(CCSDS_TC_SECHDR_LEN + payload_len + CCSDS_TC_CRC_LEN);
    if (data_field > SPP_MAX_PAYLOAD_CHUNK)
        return SPP_ERROR_PAYLOAD_LEN;

    /* Advance the (predictable) TC counter, same wrap rule as spp.c. */
    uint16_t counter = 0;
    if (cnt != NULL) {
        if (cnt->tc == 16383)
            cnt->tc = 0;
        cnt->tc++;
        counter = cnt->tc & 0x3FFFU;
    }

    /* Primary header: version 0, type TC, secondary-header flag set, APID. */
    uint16_t packet_id = 0;
    packet_id |= (CCSDS_SPP_VERSION & 0x07U) << 13;
    packet_id |= (SPP_PTYPE_TC & 0x01U) << 12;
    packet_id |= (SPP_SECHEAD_FLAG_PRESENT & 0x01U) << 11;
    packet_id |= (apid & 0x07FFU);

    uint16_t seq_ctrl = 0;
    seq_ctrl |= (SPP_GROUP_FLAG_UNSEGMENTED & 0x03U) << 14;
    seq_ctrl |= (counter & 0x3FFFU);

    pkt->header.identification = spp_host_to_be16(packet_id);
    pkt->header.sequence       = spp_host_to_be16(seq_ctrl);
    pkt->header.length         = spp_host_to_be16((uint16_t)(data_field - 1U));

    /* Secondary header: 32-bit timestamp (big-endian), also the cipher IV. */
    pkt->data[0] = (uint8_t)(timestamp >> 24);
    pkt->data[1] = (uint8_t)(timestamp >> 16);
    pkt->data[2] = (uint8_t)(timestamp >> 8);
    pkt->data[3] = (uint8_t)(timestamp);

    /* Plaintext payload. */
    if (payload != NULL && payload_len > 0)
        memcpy(pkt->data + CCSDS_TC_SECHDR_LEN, payload, payload_len);

    /* CRC-16-CCITT over the PLAINTEXT frame (primary + sec-hdr + payload), then
     * append it. (uint8_t*)pkt is contiguous [header(6)][data...]. */
    uint16_t crc_region = (uint16_t)(SPP_PRIMARY_HEADER_LEN + CCSDS_TC_SECHDR_LEN + payload_len);
    uint16_t crc = ccsds_crc16_ccitt((const uint8_t *)pkt, crc_region);
    pkt->data[CCSDS_TC_SECHDR_LEN + payload_len]      = (uint8_t)(crc >> 8);
    pkt->data[CCSDS_TC_SECHDR_LEN + payload_len + 1U] = (uint8_t)(crc);

    /* Encrypt ONLY the payload region (CRC stays over plaintext). */
    apply_cipher(pkt->data + CCSDS_TC_SECHDR_LEN, payload_len, timestamp);

    if (out_total != NULL)
        *out_total = (uint16_t)(SPP_PRIMARY_HEADER_LEN + data_field);
    return SPP_ERROR_NONE;
}

int ccsds_tc_unsecure(space_packet_t *pkt, uint32_t *timestamp_out, int *crc_ok)
{
    if (pkt == NULL)
        return SPP_ERROR_INVALID_BUFFER;

    uint16_t id       = spp_be16_to_host(pkt->header.identification);
    uint8_t  sec_flag = (uint8_t)((id >> 11) & 0x01U);
    if (!sec_flag)
        return CCSDS_TC_ERR_NOSEC;     /* plaintext TC — leave pkt untouched */

    uint16_t data_field = (uint16_t)(spp_be16_to_host(pkt->header.length) + 1U);
    if (data_field < (CCSDS_TC_SECHDR_LEN + CCSDS_TC_CRC_LEN))
        return SPP_ERROR_PAYLOAD_LEN;  /* too short for sec-hdr + CRC */

    uint16_t payload_len = (uint16_t)(data_field - CCSDS_TC_SECHDR_LEN - CCSDS_TC_CRC_LEN);

    uint32_t timestamp = ((uint32_t)pkt->data[0] << 24) | ((uint32_t)pkt->data[1] << 16) |
                         ((uint32_t)pkt->data[2] << 8)  |  (uint32_t)pkt->data[3];
    if (timestamp_out != NULL)
        *timestamp_out = timestamp;

    /* Decrypt the payload region (IV = timestamp), then verify CRC over the
     * recovered plaintext. */
    apply_cipher(pkt->data + CCSDS_TC_SECHDR_LEN, payload_len, timestamp);

    uint16_t crc_region = (uint16_t)(SPP_PRIMARY_HEADER_LEN + CCSDS_TC_SECHDR_LEN + payload_len);
    uint16_t want = ccsds_crc16_ccitt((const uint8_t *)pkt, crc_region);
    uint16_t got  = (uint16_t)(((uint16_t)pkt->data[CCSDS_TC_SECHDR_LEN + payload_len] << 8) |
                                pkt->data[CCSDS_TC_SECHDR_LEN + payload_len + 1U]);
    if (crc_ok != NULL)
        *crc_ok = (want == got) ? 1 : 0;

    /* Collapse the frame so pkt->data holds only the plaintext args, and fix the
     * length field, so the APID handler reads it like an unsecured TC. */
    memmove(pkt->data, pkt->data + CCSDS_TC_SECHDR_LEN, payload_len);
    pkt->header.length = spp_host_to_be16(payload_len ? (uint16_t)(payload_len - 1U) : 0U);
    return CCSDS_TC_OK;
}
