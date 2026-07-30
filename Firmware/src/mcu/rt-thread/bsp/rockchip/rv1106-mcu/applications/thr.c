#include "thr.h"

static uint8_t thruster0 = 10;
static uint8_t thruster0_state = THRUSTER_STATE_RUNNING;
static uint8_t thruster1 = 10;
static uint8_t thruster1_state = THRUSTER_STATE_RUNNING;

uint8_t thruster_get_t0_power(void)     { return thruster0; }
uint8_t thruster_get_t1_power(void)     { return thruster1; }
uint8_t thruster_get_t0_state(void)     { return thruster0_state; }
uint8_t thruster_get_t1_state(void)     { return thruster1_state; }

void thruster_set_t0_power(uint8_t power)
{
    thruster0_state = (power > 0) ? THRUSTER_STATE_RUNNING : THRUSTER_STATE_IDLE;
    thruster0 = power;
}

void thruster_set_t1_power(uint8_t power)
{
    thruster1_state = (power > 0) ? THRUSTER_STATE_RUNNING : THRUSTER_STATE_IDLE;
    thruster1 = power;
}

void thruster_set_t0_state(uint8_t state) { thruster0_state = state; }
void thruster_set_t1_state(uint8_t state) { thruster1_state = state; }