#include "packet_dispatch.h"

static pd_handler_t s_handlers[PD_MAX_HANDLERS];
static int           s_n_handlers;

int pd_register(pd_handler_t fn)
{
    if (!fn || s_n_handlers >= PD_MAX_HANDLERS)
        return -1;
    s_handlers[s_n_handlers++] = fn;
    return 0;
}

void pd_dispatch(int inst, const uint8_t *data, uint8_t len,
                 int16_t rssi, int8_t snr, bool crc_ok)
{
    for (int i = 0; i < s_n_handlers; i++)
        s_handlers[i](inst, data, len, rssi, snr, crc_ok);
}