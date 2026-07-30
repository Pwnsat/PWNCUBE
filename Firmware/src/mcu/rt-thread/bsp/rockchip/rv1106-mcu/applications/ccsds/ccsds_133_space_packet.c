#include "ccsds_133_space_packet.h"
#include <string.h>

void ccsds_133_packet_init(ccsds_133_packet_t *pkt)
{
    if (!pkt) return;
    memset(pkt, 0, sizeof(*pkt));
    pkt->primary.version_number = CCSDS_VERSION_NUMBER_133;
    pkt->primary.sequence_flags = CCSDS_SEQ_STANDALONE;
}

ccsds_status_t ccsds_133_packet_encode(const ccsds_133_packet_t *pkt,
                                        uint8_t *buf, size_t buf_size,
                                        size_t *out_len)
{
    if (!pkt || !buf || !out_len) return CCSDS_ERR_NULL;

    size_t total = CCSDS_133_PRI_HDR_SIZE;
    size_t data_field_len = 0;

    if (pkt->primary.secondary_hdr_flag) {
        if (pkt->sec_hdr_len < CCSDS_133_SEC_HDR_MIN_SIZE)
            return CCSDS_ERR_INVALID;
        total += pkt->sec_hdr_len;
        data_field_len += pkt->sec_hdr_len;
    }

    if (pkt->data && pkt->data_len > 0) {
        total += pkt->data_len;
        data_field_len += pkt->data_len;
    }

    if (pkt->crc_type == CCSDS_133_CRC_FLAG_CRC16) {
        total += CCSDS_CRC_16_SIZE;
        data_field_len += CCSDS_CRC_16_SIZE;
    }

    if (total < CCSDS_133_MIN_PACKET_SIZE)
        return CCSDS_ERR_SIZE;
    if (buf_size < total)
        return CCSDS_ERR_NO_SPACE;
    if (pkt->primary.version_number != CCSDS_VERSION_NUMBER_133)
        return CCSDS_ERR_VERSION;

    ccsds_133_primary_header_t ph = pkt->primary;
    ph.packet_data_length = (uint16_t)(data_field_len - 1);

    uint8_t hdr[CCSDS_133_PRI_HDR_SIZE];
    ccsds_133_build_primary(hdr, &ph);

    if (ph.apid == CCSDS_APID_RESERVED_MIN)
        return CCSDS_ERR_APID;

    size_t offset = 0;
    for (size_t i = 0; i < CCSDS_133_PRI_HDR_SIZE; i++)
        buf[offset++] = hdr[i];

    if (pkt->primary.secondary_hdr_flag && pkt->sec_hdr && pkt->sec_hdr_len > 0) {
        for (size_t i = 0; i < pkt->sec_hdr_len; i++)
            buf[offset++] = pkt->sec_hdr[i];
    }

    if (pkt->data && pkt->data_len > 0) {
        for (size_t i = 0; i < pkt->data_len; i++)
            buf[offset++] = pkt->data[i];
    }

    if (pkt->crc_type == CCSDS_133_CRC_FLAG_CRC16) {
        uint16_t crc = ccsds_crc16_ccitt(buf, offset);
        buf[offset++] = (uint8_t)((crc >> 8) & 0xFF);
        buf[offset++] = (uint8_t)(crc & 0xFF);
    }

    *out_len = offset;
    return CCSDS_OK;
}

ccsds_status_t ccsds_133_packet_decode(const uint8_t *buf, size_t buf_len,
                                        ccsds_133_packet_t *pkt,
                                        bool verify_crc)
{
    if (!buf || !pkt) return CCSDS_ERR_NULL;
    if (buf_len < CCSDS_133_PRI_HDR_SIZE) return CCSDS_ERR_SIZE;

    ccsds_133_parse_primary(buf, &pkt->primary);

    if (pkt->primary.version_number != CCSDS_VERSION_NUMBER_133)
        return CCSDS_ERR_VERSION;
    if (pkt->primary.apid == CCSDS_APID_RESERVED_MIN)
        return CCSDS_ERR_APID;

    uint16_t total_len = ccsds_133_total_length(buf);
    if (total_len > buf_len) return CCSDS_ERR_SIZE;

    size_t pos = CCSDS_133_PRI_HDR_SIZE;
    size_t data_field_len = (size_t)pkt->primary.packet_data_length + 1;

    if (pkt->primary.secondary_hdr_flag) {
        pkt->sec_hdr = (uint8_t *)buf + pos;
        if (pkt->sec_hdr_len == 0)
            pkt->sec_hdr_len = data_field_len;
    }

    if (pkt->primary.secondary_hdr_flag && pkt->sec_hdr_len > 0)
        pos += pkt->sec_hdr_len;

    if (pos < CCSDS_133_PRI_HDR_SIZE + data_field_len) {
        pkt->data = (uint8_t *)buf + pos;
        pkt->data_len = (CCSDS_133_PRI_HDR_SIZE + data_field_len) - pos;
    }

    if (verify_crc || pkt->crc_type == CCSDS_133_CRC_FLAG_CRC16) {
        if (pkt->data_len >= CCSDS_CRC_16_SIZE) {
            pkt->data_len -= CCSDS_CRC_16_SIZE;
            pkt->crc_type = CCSDS_133_CRC_FLAG_CRC16;
        }
    }

    if (verify_crc && pkt->crc_type == CCSDS_133_CRC_FLAG_CRC16) {
        size_t crc_start = total_len - CCSDS_CRC_16_SIZE;
        uint16_t expected = ccsds_crc16_ccitt(buf, crc_start);
        uint16_t actual = ((uint16_t)buf[crc_start] << 8) | buf[crc_start + 1];
        if (expected != actual)
            return CCSDS_ERR_CRC;
    }

    return CCSDS_OK;
}

ccsds_status_t ccsds_133_idle_packet_create(uint8_t *buf, size_t buf_size,
                                             uint16_t seq_count,
                                             size_t *out_len)
{
    if (!buf || !out_len) return CCSDS_ERR_NULL;
    if (buf_size < CCSDS_133_PRI_HDR_SIZE + 1) return CCSDS_ERR_NO_SPACE;

    ccsds_133_packet_t pkt;
    ccsds_133_packet_init(&pkt);

    pkt.primary.apid = CCSDS_APID_IDLE;
    pkt.primary.packet_type = CCSDS_PTYPE_TM;
    pkt.primary.secondary_hdr_flag = 0;
    pkt.primary.sequence_flags = CCSDS_SEQ_STANDALONE;
    pkt.primary.sequence_count = seq_count & 0x3FFFU;
    pkt.data_len = 0;
    pkt.crc_type = CCSDS_133_CRC_FLAG_CRC16;

    return ccsds_133_packet_encode(&pkt, buf, buf_size, out_len);
}
