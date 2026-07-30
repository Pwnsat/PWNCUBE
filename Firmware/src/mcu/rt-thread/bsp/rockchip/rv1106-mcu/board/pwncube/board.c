/*
 * CubeSat flight-computer MCU board init (RV1106 RISC-V / RT-Thread).
 * Minimal variant: no camera / ISP. Hardware services (radio, sensors)
 * are added incrementally per docs/migration/.
 */
#include <rthw.h>
#include <rtthread.h>
#include "board.h"
