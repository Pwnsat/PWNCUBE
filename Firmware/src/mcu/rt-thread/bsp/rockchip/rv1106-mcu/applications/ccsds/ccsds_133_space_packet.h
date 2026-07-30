#ifndef CCSDS_133_SPACE_PACKET_H
#define CCSDS_133_SPACE_PACKET_H

#include "ccsds_types.h"
#include "ccsds_crc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CCSDS_133_PRI_HDR_SIZE          6U
#define CCSDS_133_MIN_PACKET_SIZE       7U

#define CCSDS_PTYPE_TM                  0U
#define CCSDS_PTYPE_TC                  1U

#define CCSDS_SEQ_CONTINUATION          0U
#define CCSDS_SEQ_FIRST                 1U
#define CCSDS_SEQ_LAST                  2U
#define CCSDS_SEQ_STANDALONE            3U

#define CCSDS_APID_RESERVED_MIN         0x000U
#define CCSDS_APID_RESERVED_MAX         0x000U
#define CCSDS_APID_IDLE                 0x7FFU

#define CCSDS_133_SEC_HDR_MIN_SIZE      1U

#define CCSDS_133_CRC_FLAG_NONE         0U
#define CCSDS_133_CRC_FLAG_CRC16        1U

typedef struct {
    uint8_t  version_number;
    uint8_t  packet_type;
    uint8_t  secondary_hdr_flag;
    uint16_t apid;
    uint8_t  sequence_flags;
    uint16_t sequence_count;
    uint16_t packet_data_length;
} ccsds_133_primary_header_t;

typedef struct {
    ccsds_133_primary_header_t primary;
    uint8_t  *sec_hdr;
    size_t    sec_hdr_len;
    uint8_t  *data;
    size_t    data_len;
    uint8_t   crc_type;
} ccsds_133_packet_t;

void ccsds_133_packet_init(ccsds_133_packet_t *pkt);

ccsds_status_t ccsds_133_packet_encode(const ccsds_133_packet_t *pkt,
                                        uint8_t *buf, size_t buf_size,
                                        size_t *out_len);

ccsds_status_t ccsds_133_packet_decode(const uint8_t *buf, size_t buf_len,
                                        ccsds_133_packet_t *pkt,
                                        bool verify_crc);

ccsds_status_t ccsds_133_idle_packet_create(uint8_t *buf, size_t buf_size,
                                             uint16_t seq_count,
                                             size_t *out_len);

/* Safe byte-access versions for RISC-V (no unaligned access) */

static inline void ccsds_133_parse_primary(const uint8_t hdr[6],
                                            ccsds_133_primary_header_t *ph)
{
    uint16_t w0 = ((uint16_t)hdr[0] << 8) | hdr[1];
    uint16_t w1 = ((uint16_t)hdr[2] << 8) | hdr[3];
    uint16_t w2 = ((uint16_t)hdr[4] << 8) | hdr[5];

    ph->version_number      = (uint8_t)((w0 >> 13) & 0x07U);
    ph->packet_type         = (uint8_t)((w0 >> 12) & 0x01U);
    ph->secondary_hdr_flag  = (uint8_t)((w0 >> 11) & 0x01U);
    ph->apid                = w0 & 0x07FFU;
    ph->sequence_flags      = (uint8_t)((w1 >> 14) & 0x03U);
    ph->sequence_count      = w1 & 0x3FFFU;
    ph->packet_data_length  = w2;
}

static inline void ccsds_133_build_primary(uint8_t hdr[6],
                                            const ccsds_133_primary_header_t *ph)
{
    uint16_t w0 = (uint16_t)(((uint16_t)(ph->version_number & 0x07U) << 13) |
                              ((uint16_t)(ph->packet_type & 0x01U) << 12) |
                              ((uint16_t)(ph->secondary_hdr_flag & 0x01U) << 11) |
                              (ph->apid & 0x07FFU));
    uint16_t w1 = (uint16_t)(((uint16_t)(ph->sequence_flags & 0x03U) << 14) |
                              (ph->sequence_count & 0x3FFFU));
    uint16_t w2 = ph->packet_data_length;

    hdr[0] = (uint8_t)((w0 >> 8) & 0xFF);
    hdr[1] = (uint8_t)(w0 & 0xFF);
    hdr[2] = (uint8_t)((w1 >> 8) & 0xFF);
    hdr[3] = (uint8_t)(w1 & 0xFF);
    hdr[4] = (uint8_t)((w2 >> 8) & 0xFF);
    hdr[5] = (uint8_t)(w2 & 0xFF);
}

static inline uint16_t ccsds_133_total_length(const uint8_t hdr[6])
{
    uint16_t data_len = ((uint16_t)hdr[4] << 8) | hdr[5];
    return (uint16_t)(6U + data_len + 1U);
}

#ifdef __cplusplus
}
#endif

#endif /* CCSDS_133_SPACE_PACKET_H */
