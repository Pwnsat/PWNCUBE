#ifndef THR_H
#define THR_H

#include <stdint.h>
#include <stdbool.h>

#define THRUSTER_STATE_IDLE     0U
#define THRUSTER_STATE_RUNNING  1U

uint8_t thruster_get_t0_power(void);
uint8_t thruster_get_t1_power(void);
uint8_t thruster_get_t0_state(void);
uint8_t thruster_get_t1_state(void);

void thruster_set_t0_power(uint8_t power);
void thruster_set_t1_power(uint8_t power);
void thruster_set_t0_state(uint8_t state);
void thruster_set_t1_state(uint8_t state);

#endif