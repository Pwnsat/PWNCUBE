/**
  * Copyright (c) 2022 Rockchip Electronic Co.,Ltd
  *
  * SPDX-License-Identifier: Apache-2.0
  ******************************************************************************
  * @file    main.c
  * @author  Jason Zhu
  * @version V0.1
  * @date    11-Jan-2022
  * @brief
  *
  ******************************************************************************
  */

#include <stdio.h>
#include <string.h>
#include <rtthread.h>
#include "board_base.h"

void rtthread_startup(void)
{
#ifdef RT_USING_RK_AOV
    rt_kprintf("RT_USING_RK_AOV=y\n");
    uint32_t resume_reason = readl(0xff3001c0);
    rt_kprintf("resume_reason is 0x%x\n", resume_reason);
    if (resume_reason == 0) {
        return;
    }
#endif
    /* disable interrupt first */
    rt_hw_interrupt_disable();

    /* init board */
    rt_hw_board_init();

    /* init tick */
    rt_system_tick_init();

    /* init kernel object */
    rt_system_object_init();

    /* init timer system */
    rt_system_timer_init();

    /* init scheduler system */
    rt_system_scheduler_init();

    /* init application */
    rt_application_init();

    /* init timer thread */
    rt_system_timer_thread_init();

    /* init idle thread */
    rt_thread_idle_init();

    /* start scheduler */
    rt_system_scheduler_start();

    /* never reach here */
    return;
}

int main(void)
{
    /* CubeSat Paso 0 bring-up heartbeat — prove the RISC-V core executes rtthread.
     * Write a marker into the MAILBOX B2A data/cmd regs (0xff5c0030/34), which the
     * A7 reads reliably via `busybox devmem` regardless of hpmcu_sram mapping/cache.
     * Also keep the hpmcu_sram marker. Remove once IPC is validated. */
    *(volatile unsigned int *)0xff5c0034 = 0xCAFE0001;   /* B2A_DAT(0) */
    *(volatile unsigned int *)0xff6ff800 = 0xCAFE0001;   /* hpmcu_sram */

    /* startup RT-Thread RTOS */
    rtthread_startup();

    return 0;
}
