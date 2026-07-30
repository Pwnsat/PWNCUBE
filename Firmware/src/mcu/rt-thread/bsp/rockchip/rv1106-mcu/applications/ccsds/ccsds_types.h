#ifndef CCSDS_TYPES_H
#define CCSDS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCSDS_VERSION_NUMBER_133  0U
#define CCSDS_VERSION_NUMBER_132  0U

#define CCSDS_APID_IDLE           0x7FFU

#define CCSDS_PACKET_PRIMARY_HDR_SIZE  6U
#define CCSDS_TM_FRAME_PRIMARY_HDR_SIZE  6U

#define CCSDS_CRC_16_SIZE         2U
#define CCSDS_CRC_32_SIZE         4U

#ifndef CCSDS_MAX_PACKET_SIZE
#define CCSDS_MAX_PACKET_SIZE     256U
#endif

#ifndef CCSDS_MAX_TM_FRAME_SIZE
#define CCSDS_MAX_TM_FRAME_SIZE   256U
#endif

typedef enum {
    CCSDS_OK            = 0,
    CCSDS_ERR_NULL      = -1,
    CCSDS_ERR_SIZE      = -2,
    CCSDS_ERR_VERSION   = -3,
    CCSDS_ERR_APID      = -4,
    CCSDS_ERR_CRC       = -5,
    CCSDS_ERR_INVALID   = -6,
    CCSDS_ERR_BUSY      = -7,
    CCSDS_ERR_NO_SPACE  = -8,
} ccsds_status_t;

static inline uint16_t ccsds_htobe16(uint16_t h)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (uint16_t)((h << 8) | (h >> 8));
#else
    return h;
#endif
}

static inline uint32_t ccsds_htobe32(uint32_t h)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (uint32_t)(((h >> 24) & 0xFFU) | ((h >> 8) & 0xFF00U) |
                      ((h & 0xFF00U) << 8) | ((h & 0xFFU) << 24));
#else
    return h;
#endif
}

static inline uint16_t ccsds_betoh16(uint16_t b)
{
    return ccsds_htobe16(b);
}

static inline uint32_t ccsds_betoh32(uint32_t b)
{
    return ccsds_htobe32(b);
}

#ifdef __cplusplus
}
#endif

#endif /* CCSDS_TYPES_H */
