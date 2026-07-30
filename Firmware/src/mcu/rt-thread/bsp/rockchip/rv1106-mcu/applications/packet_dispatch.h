#ifndef PACKET_DISPATCH_H
#define PACKET_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>

#define PD_MAX_HANDLERS  4
#define PD_MAX_PAYLOAD   250

typedef void (*pd_handler_t)(int inst, const uint8_t *data, uint8_t len,
                              int16_t rssi, int8_t snr, bool crc_ok);

int  pd_register(pd_handler_t fn);
void pd_dispatch(int inst, const uint8_t *data, uint8_t len,
                 int16_t rssi, int8_t snr, bool crc_ok);

#endif