/*
 * SPP — Space Packet Protocol (CCSDS 133.0-B-2)
 *
 * CCSDS 133.0-B SPP implementation:
 * - 6-byte big-endian primary header, NO secondary header by default, NO CRC
 * - TM and TC independent sequence counters (wrap at 16383)
 * - Idle packets use sequence count = 0
 */

#ifndef SPP_H
#define SPP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- */
/*  Constants                                                         */
/* ----------------------------------------------------------------- */

#define CCSDS_SPP_VERSION          0U

#define SPP_PTYPE_TM               0U
#define SPP_PTYPE_TC               1U

#define SPP_GROUP_FLAG_CONT        0U
#define SPP_GROUP_FLAG_START       1U
#define SPP_GROUP_FLAG_END         2U
#define SPP_GROUP_FLAG_UNSEGMENTED 3U

#define SPP_SECHEAD_FLAG_NOPRESENT 0U
#define SPP_SECHEAD_FLAG_PRESENT   1U

#define SPP_PRIMARY_HEADER_LEN     6U
#define SPP_MAX_PAYLOAD_CHUNK      256U

#define SPP_APID_IDLE              0x7FFU

/* Error codes */
#define SPP_ERROR_NONE             0
#define SPP_ERROR_PACKET_LEN      -1
#define SPP_ERROR_INVALID_BUFFER  -2
#define SPP_ERROR_VERSION         -3
#define SPP_ERROR_PAYLOAD_LEN     -4
#define SPP_ERROR_UNKNOWN         -5

/* ----------------------------------------------------------------- */
/*  Types                                                             */
/* ----------------------------------------------------------------- */

typedef struct {
    uint16_t identification;
    uint16_t sequence;
    uint16_t length;
} __attribute__((packed)) spp_primary_header_t;

typedef struct {
    spp_primary_header_t header;
    uint8_t              data[SPP_MAX_PAYLOAD_CHUNK];
} space_packet_t;

/* ----------------------------------------------------------------- */
/*  Sequence counters                                                 */
/* ----------------------------------------------------------------- */

typedef struct {
    uint16_t tc;
    uint16_t tm;
} packet_counter_t;

/* ----------------------------------------------------------------- */
/*  API                                                               */
/* ----------------------------------------------------------------- */

void spp_counters_init(packet_counter_t *cnt);

int  spp_tm_build_packet(space_packet_t *pkt, packet_counter_t *cnt,
                         uint8_t flag, uint8_t sec_header,
                         uint16_t sec_header_len, uint16_t apid,
                         const uint8_t *data, uint16_t data_len);

int  spp_tc_build_packet(space_packet_t *pkt, packet_counter_t *cnt,
                         uint8_t flag, uint8_t sec_header,
                         uint16_t sec_header_len, uint16_t apid,
                         const uint8_t *data, uint16_t data_len);

int  spp_idle_build_packet(space_packet_t *pkt);

int  spp_unpack_packet(space_packet_t *pkt, const uint8_t *buffer,
                       uint16_t buffer_len);

/* Helpers */
uint16_t spp_host_to_be16(uint16_t x);
uint16_t spp_be16_to_host(uint16_t x);
uint16_t spp_total_length(const space_packet_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* SPP_H */
