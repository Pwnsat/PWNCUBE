/*
 * SPP — Space Packet Protocol implementation (CCSDS 133.0-B-2)
 *
 * CCSDS 133.0-B SPP: 6-byte big-endian primary header,
 * NO secondary header by default, NO CRC.
 * Sequence counters wrap at 16383; idle uses seq = 0.
 */

#include "spp.h"
#include <string.h>

/* ----------------------------------------------------------------- */
/*  Byte-order helpers                                                */
/* ----------------------------------------------------------------- */

uint16_t spp_host_to_be16(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

uint16_t spp_be16_to_host(uint16_t x)
{
    return spp_host_to_be16(x);
}

/* ----------------------------------------------------------------- */
/*  Counter helpers                                                   */
/* ----------------------------------------------------------------- */

void spp_counters_init(packet_counter_t *cnt)
{
    cnt->tc = 0;
    cnt->tm = 0;
}

static void spp_validate_counters(packet_counter_t *cnt)
{
    if (cnt->tc == 16383)
        cnt->tc = 0;
    if (cnt->tm == 16383)
        cnt->tm = 0;
}

/* ----------------------------------------------------------------- */
/*  Total packet length helper                                        */
/* ----------------------------------------------------------------- */

uint16_t spp_total_length(const space_packet_t *pkt)
{
    return SPP_PRIMARY_HEADER_LEN +
           (spp_be16_to_host(pkt->header.length) + 1);
}

/* ----------------------------------------------------------------- */
/*  Internal build (CCSDS SPP primary-header build)      */
/* ----------------------------------------------------------------- */

static int spp_build_packet(space_packet_t *pkt,
                            uint8_t type, uint8_t flag,
                            uint8_t sec_header, uint16_t sec_header_len,
                            uint16_t apid, uint16_t sequence_count,
                            const uint8_t *data, uint16_t data_len)
{
    uint16_t packet_id = 0;
    packet_id |= (CCSDS_SPP_VERSION & 0x07) << 13;
    packet_id |= (type & 0x01) << 12;
    packet_id |= (sec_header & 0x01) << 11;
    packet_id |= (apid & 0x07FF);

    uint16_t seq_ctrl = 0;
    seq_ctrl |= (flag & 0x03) << 14;
    seq_ctrl |= (sequence_count & 0x3FFF);

    pkt->header.identification = spp_host_to_be16(packet_id);
    pkt->header.sequence       = spp_host_to_be16(seq_ctrl);
    pkt->header.length         = spp_host_to_be16(data_len + sec_header_len - 1);

    if (data_len > 0 && data != NULL)
        memcpy(pkt->data, data, data_len);

    return SPP_ERROR_NONE;
}

/* ----------------------------------------------------------------- */
/*  TM packet builder — increments tm counter                         */
/* ----------------------------------------------------------------- */

int spp_tm_build_packet(space_packet_t *pkt, packet_counter_t *cnt,
                        uint8_t flag, uint8_t sec_header,
                        uint16_t sec_header_len, uint16_t apid,
                        const uint8_t *data, uint16_t data_len)
{
    spp_validate_counters(cnt);
    cnt->tm++;
    return spp_build_packet(pkt, SPP_PTYPE_TM, flag, sec_header,
                            sec_header_len, apid, cnt->tm,
                            data, data_len);
}

/* ----------------------------------------------------------------- */
/*  TC packet builder — increments tc counter                         */
/* ----------------------------------------------------------------- */

int spp_tc_build_packet(space_packet_t *pkt, packet_counter_t *cnt,
                        uint8_t flag, uint8_t sec_header,
                        uint16_t sec_header_len, uint16_t apid,
                        const uint8_t *data, uint16_t data_len)
{
    spp_validate_counters(cnt);
    cnt->tc++;
    return spp_build_packet(pkt, SPP_PTYPE_TC, flag, sec_header,
                            sec_header_len, apid, cnt->tc,
                            data, data_len);
}

/* ----------------------------------------------------------------- */
/*  Idle packet builder — seq count always 0 (per CCSDS)        */
/* ----------------------------------------------------------------- */

int spp_idle_build_packet(space_packet_t *pkt)
{
    const uint8_t buffer_idle[] = {
        0x48, 0x61, 0x63, 0x6b, 0x54, 0x68, 0x65, 0x57,
        0x6f, 0x72, 0x6c, 0x64, 0x21, 0x00
    };
    return spp_build_packet(pkt, SPP_PTYPE_TM,
                            SPP_GROUP_FLAG_UNSEGMENTED,
                            SPP_SECHEAD_FLAG_NOPRESENT, 0,
                            SPP_APID_IDLE, 0,
                            buffer_idle, sizeof(buffer_idle));
}

/* ----------------------------------------------------------------- */
/*  Unpack: parse a raw buffer into space_packet_t                   */
/* ----------------------------------------------------------------- */

int spp_unpack_packet(space_packet_t *pkt, const uint8_t *buffer,
                       uint16_t buffer_len)
{
    if (pkt == NULL)
        return SPP_ERROR_INVALID_BUFFER;
    if (buffer_len < SPP_PRIMARY_HEADER_LEN)
        return SPP_ERROR_PACKET_LEN;
    if (buffer == NULL)
        return SPP_ERROR_INVALID_BUFFER;

    memset(pkt, 0, sizeof(space_packet_t));

    /* Same convention as the build path: header fields hold the BE wire
     * value, so the packed struct IS the raw frame. Convert with
     * spp_be16_to_host() when reading a field. */
    memcpy(&pkt->header, buffer, SPP_PRIMARY_HEADER_LEN);

    uint8_t version = (spp_be16_to_host(pkt->header.identification) >> 13) & 0x07;
    if (version != CCSDS_SPP_VERSION)
        return SPP_ERROR_VERSION;

    /* CCSDS length field = data-field bytes - 1; actual data = field + 1.
     *
     * VULN #9 (inherited from FlatSat — intentional for the CTF):
     *   1) The check uses `field` (not `field + 1`) against MAX, same as
     *      FlatSat: it allows field == SPP_MAX_PAYLOAD_CHUNK and then copies
     *      field + 1 bytes -> off-by-one write 1 byte past data[].
     *   2) `buffer_len` is NOT validated against the declared length: memcpy
     *      copies `field + 1` bytes from the input buffer without checking
     *      how many actually arrived -> OVER-READ of up to ~MAX bytes of
     *      adjacent memory (leak), controlled by the TC's `length` field.
     *
     * The data[] array is used as a raw destination pointer in memcpy(): C does
     * not enforce the array bound, so without these checks the attacker controls
     * how much is read/written. (buffer_len is deliberately left unused.) */
    (void)buffer_len;
    uint16_t field = spp_be16_to_host(pkt->header.length);
    if (field > SPP_MAX_PAYLOAD_CHUNK)
        return SPP_ERROR_PAYLOAD_LEN;

    memcpy(pkt->data, buffer + SPP_PRIMARY_HEADER_LEN, field + 1);
    return SPP_ERROR_NONE;
}
