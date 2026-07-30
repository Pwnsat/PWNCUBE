/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2022 Rockchip Electronics Co., Ltd.
 * RV1106 RISC-V (HPMCU) port — CubeSat. See docs/migration/.
 */
#ifndef RPMSG_CONFIG_H_
#define RPMSG_CONFIG_H_

#define RL_BUFFER_PAYLOAD_SIZE (496U)
#define RL_BUFFER_COUNT (64U)
/* endpoint size is formed by payload and struct rpmsg_std_hdr */
#define RL_EPT_SIZE (RL_BUFFER_PAYLOAD_SIZE + 16UL)

#define RL_MAX_INSTANCE_NUM (12U)
#define RL_PLATFORM_HIGHEST_LINK_ID     (0xFFU)

#define RL_PLATFORM_USING_MBOX

#ifdef RL_PLATFORM_USING_MBOX
/*
 * RV1106 has a SINGLE mailbox (MBOX @0xff5c0000) with only two CPU-facing
 * interrupts: MAILBOX0_AP_IRQn (to the Cortex-A7) and MAILBOX0_BB_IRQn (to
 * this RISC-V "BB" core). Unlike RK3568 there is NOT one IRQ per channel, so
 * RL_PLATFORM_M_IRQ/R_IRQ ignore the channel index and return the single IRQ.
 * (soc.h, MCU view: MAILBOX0_AP_IRQn=1, MAILBOX0_BB_IRQn=2.)
 */
#define RL_PLATFORM_B2A_IRQ_BASE        MAILBOX0_AP_IRQn
#define RL_PLATFORM_A2B_IRQ_BASE        MAILBOX0_BB_IRQn
#define RL_PLATFORM_M_IRQ(n)            MAILBOX0_AP_IRQn
#define RL_PLATFORM_R_IRQ(n)            MAILBOX0_BB_IRQn
#define RL_RPMSG_MAGIC                  (0x524D5347U)
#endif

/*
 * env bm/os isr count: 4bit master + 4bit remote, each link_id has 2 vqueue.
 */
#define ISR_COUNT (0x1E0U)

#endif /* RPMSG_CONFIG_H_ */
