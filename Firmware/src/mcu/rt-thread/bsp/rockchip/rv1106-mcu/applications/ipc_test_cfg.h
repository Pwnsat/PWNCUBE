/*
 * CubeSat IPC test build switch.
 *
 * IPC_RAW_MBOX_TEST == 1  -> run the raw-mailbox loopback diagnostic
 *                            (applications/ipc_mbox_test.c) and DISABLE the
 *                            rpmsg-lite ping/echo app. Use this to validate the
 *                            A7<->MCU transport with the proven Rockchip pattern
 *                            (raw mailbox + shared scratch, NO vrings) and to
 *                            discover which mailbox registers the SCR1 can read.
 *                            See docs/migration/implementation/70-mailbox-loopback-test.md
 *                            and riscv2arm.md.
 *
 * IPC_RAW_MBOX_TEST == 0  -> normal build: rpmsg-lite ping/echo (ping_echo.c).
 */
#ifndef IPC_TEST_CFG_H
#define IPC_TEST_CFG_H

#define IPC_RAW_MBOX_TEST 0

#endif /* IPC_TEST_CFG_H */
