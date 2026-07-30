# Raw mailbox loopback test (without rpmsg)

A7 ↔ MCU transport diagnostic based on the **pattern proven by Rockchip**
(raw mailbox + shared memory, **without vrings**), documented in
`riscv2arm.md` (codemap of the Luckfox SDK).

## Why

The rpmsg-lite path **hangs**: when Linux's first kick arrives, the poll
thread enters `env_isr → virtqueue_notification` and freezes (poller heartbeat
stuck). It is also already confirmed that the SCR1 **reads `A2B_STATUS` as 0**
even though the A7 sees the pending bit (that is why `HAL_MBOX_IrqHandler` never finds
the channel). The Luckfox reference **never** uses rpmsg nor receives A2B on the MCU
(see [`luckfox-amp-is-b2a-only`]): it only does raw B2A MCU→ARM.

This test isolates the problem into two questions:

1. **Does MCU→ARM (B2A) work?** — the direction that Rockchip does exercise.
2. **Which RX (A2B) registers can the SCR1 READ?** — `A2B_STATUS` no; what about
   `A2B_CMD(3)` / `A2B_DAT(3)`? If the MCU can read `A2B_DAT(3)`, the clean approach
   is to poll the magic `0x524D5347` there, without int_mux or virtqueue.

## Pieces of the package

| Piece | Path |
|-------|------|
| Build switch | `src/mcu/.../rv1106-mcu/applications/ipc_test_cfg.h` (`IPC_RAW_MBOX_TEST 1`) |
| MCU test app | `src/mcu/.../rv1106-mcu/applications/ipc_mbox_test.c` |
| rpmsg ping/echo | `ping_echo.c` — compiled out when the switch is at 1 |
| Reference | `riscv2arm.md` |

**Safety:** the test touches ONLY the mailbox registers (`0xff5c0000`) and the
`hpmcu_sram` scratch (`0xff6ff900+`). It **never** touches the vrings in DDR
(`0x0ff00000`) — which is what corrupted Linux's virtio probe. That is why it is
safe to boot regardless of Linux's state.

## How to build and flash

```sh
# 1) With IPC_RAW_MBOX_TEST = 1 in ipc_test_cfg.h
./build.sh mcu                                   # -> output/mcu/rtthread.bin
cp output/mcu/rtthread.bin src/rkbin/bin/rv11/rtthread.bin
./build.sh                                       # repack -> output/images/update.img
# 2) Board in maskrom (short SPI-NAND CLK<->GND while powering)
sudo ./tools/upgrade_tool UF output/images/update.img
```

## Observation from Linux (serial, `busybox devmem`)

```sh
# Heartbeat MCU->ARM (B2A ch2 DATA): should advance 0xC0DExxxx  -> tests MCU TX
devmem 0xff5c0044 32

# What the MCU READS (scratch):
devmem 0xff6ff900 32   # thread heartbeat        (0xB000xxxx advancing)
devmem 0xff6ff904 32   # A2B_STATUS seen by MCU  (A7 sees 0x08 with kick pending)
devmem 0xff6ff908 32   # A2B_CMD(3)  seen by MCU  (A7 wrote 0x00000004)   <- KEY
devmem 0xff6ff90c 32   # A2B_DAT(3)  seen by MCU  (A7 wrote 0x524D5347)   <- KEY
devmem 0xff6ff910 32   # int_mux status0 seen by MCU
devmem 0xff6ff914 32   # A2B_INTEN seen by MCU    (sanity: 0x08)
devmem 0xff6ff918 32   # detected kick latch      (0x600Dxxxx if DAT(3)==magic)
devmem 0xff6ff91c 32   # test start marker        (0x1EE70001)
```

### Send a controlled A2B kick to the MCU (mimics the kernel)

```sh
devmem 0xff5c0024 32 0x524D5347   # A2B_DAT(3) = magic
devmem 0xff5c0020 32 0x00000004   # A2B_CMD(3) = link_id (raises the doorbell)
# luego re-leer 0xff6ff908 / 0xff6ff90c / 0xff6ff918
```

## Interpretation

- `0xff5c0044` advances → **MCU→ARM (B2A) OK** (base transport alive).
- `0xff6ff908` = `0x04` and `0xff6ff90c` = `0x524D5347` → **the MCU CAN read
  A2B_CMD/DAT** → clean approach: poll `A2B_DAT(3)==magic` as kick detector,
  discard `A2B_STATUS`/int_mux/virtqueue-by-IRQ.
- `0xff6ff908`/`0x90c` = 0 → the MCU cannot read CMD/DAT either → only the
  int_mux path (`0xff6ff910` bit2) remains as detector.
- `0xff6ff900` **does not advance** → the MCU's RTOS died (exception) — review.
