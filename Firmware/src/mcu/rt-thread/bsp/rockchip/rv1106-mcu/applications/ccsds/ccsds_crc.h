#ifndef CCSDS_CRC_H
#define CCSDS_CRC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t ccsds_crc16_ccitt(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CCSDS_CRC_H */
