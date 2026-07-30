/*
 * CubeSat — CommandService: uplink RX (radio0) + TC dispatch.
 *
 * Same pattern as radio_service / sensor_service:
 *   - rpmsg endpoint 0x4008 "rpmsg-command" for control from Linux
 *   - Continuous RX on radio0 to receive TC commands over LoRa
 *   - Processes TC commands on the MCU and replies over radio1 (downlink)
 *   - Sends EVT_TC_RX events to Linux when a TC is received
 *   - Poll function in ping_echo.c
 */

#ifndef COMMAND_SERVICE_H
#define COMMAND_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#include "rpmsg_lite.h"

int  command_service_attach(struct rpmsg_lite_instance *inst);
int  command_service_init_default(void);
void command_service_init(void);
void command_service_poll(void);
void command_service_poll_flush(void);

/* Thruster state (leido por telemetry_service) */
uint8_t command_get_thruster0(void);
uint8_t command_get_thruster1(void);
uint32_t command_get_beacon_interval_ms(void);

#endif /* COMMAND_SERVICE_H */
