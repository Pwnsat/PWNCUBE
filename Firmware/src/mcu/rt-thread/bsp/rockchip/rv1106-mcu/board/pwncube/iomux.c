/*
 * CubeSat MCU pin mux.
 * Paso 0 (IPC bring-up): the MCU stays inert at the pin level — it does NOT
 * drive uart2 (Linux owns it as the ttyFIQ0 console). Peripheral iomux (SPI
 * for SX1262, I2C for sensors) is added in later migration steps. The MCU is
 * verified over RPMsg from Linux, not via a serial console.
 */
#include "rtdef.h"
#include "iomux.h"
#include "hal_base.h"

void rt_hw_iomux_config(void)
{
}
