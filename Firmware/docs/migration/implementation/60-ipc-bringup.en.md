# IPC transport bring-up (status and recipe)

> Implementation record of the **RPMsg transport A7↔RISC-V** — now **live on hardware**.
> The transport carries **five rpmsg services in production** (PingEcho, Radio, Sensor,
> Telemetry, Command), each on its own endpoint over one shared rpmsg-lite instance.
> This document keeps the bring-up recipe and the design decisions that got there; for the
> settled picture see the architecture docs
> [../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md) and
> [../../architecture/overview.md](../../architecture/overview.md).
> Decisions taken: cede the buses to the RISC-V; our own `platform/RV1106/` porting;
> MCU without console (verification via IPC).

## 1. Done and verified (running on hardware)

**MCU firmware base**:
- Vendored RT-Thread tree in `src/mcu/`; toolchain `toolchain/riscv/` (`riscv-none-embed-gcc 10.2.0`).
- Minimal board `src/mcu/.../rv1106-mcu/board/pwncube/` (no camera/ISP; **no uart2** — Linux owns the console `ttyFIQ0`).
- `scripts/06-build-mcu.sh` + `./build.sh mcu` → `output/mcu/rtthread.bin`.

**rpmsg-lite RISC-V porting** (the hardest piece — done and running):
- `src/mcu/.../rpmsg-lite/lib/include/platform/RV1106/rpmsg_config.h` — RV1106 mailbox IRQ (a single AP/BB pair, not 8 per channel).
- `.../include/platform/RV1106/rpmsg_platform.h` — agnostic copy of RK3568.
- `.../rpmsg_lite/porting/platform/RV1106/rpmsg_platform.c` — **rewritten for RISC-V**:
  no SMP/affinity/`HAL_CPU_TOPOLOGY`, `MBOX0`→`MBOX`, single IRQ `MAILBOX0_BB_IRQn`,
  `cpsid/cpsie`→`rt_hw_interrupt_disable/enable`, `platform_in_isr`→`rt_interrupt_get_nest`.
- Portability change: `.../rpmsg-lite/lib/include/rpmsg_compiler.h` — `MEM_BARRIER()` uses
  `fence` (RISC-V) instead of `dsb` (ARM) under `#if defined(__riscv)`.
- Build selection: `.../rpmsg-lite/SConscript` and `.../common/drivers/Kconfig`
  (`RT_USING_RPMSG_LITE` `depends on ... || SOC_RV1106`).

> **`RT_USING_RPMSG_LITE` is now ENABLED** in `board/pwncube/defconfig`: the link and the
> on-board behavior are validated and all five rpmsg services run against Linux.

**Board data (read over serial, non-destructive):**
- DDR = **256 MB** at `0x00000000–0x0fffffff` (all System RAM; the shared region is carved out via `reserved-memory`).
- Linux console = `ttyFIQ0` (uart2). `/dev/ttyUSB0` @115200 gives root shell. sudo `1334`.

## 2. Shared memory map (fixed, in use)

**1 MB at the top of DDR** holds the RPMsg vrings, identical on both sides:

| Symbol | Value | Notes |
|---------|-----------------|-------|
| Shared region base | `0x0FF00000` | Top of 256 MB. **Verified: no collision with OP-TEE/trust.** |
| Size | `0x00100000` (1 MB) | vrings `0x0ff00000–0x0ff7ffff` + DMA pool `0x0ff80000–0x0fffffff`. One rpmsg instance carries all five service endpoints. |

- **Linux:** `reserved-memory` `no-map` node at `0x0FF00000` + `reg` of the `rpmsg` node.
- **MCU:** linker symbols `__linux_share_rpmsg_start__/__end__` in `src/mcu/.../rv1106-mcu/link.lds`
  point to the **same** address (NOT the SRAM 0x40000; it is DDR).
- `VRING_ALIGN=0x1000` mandatory (Linux requirement).

## 3. Parameters that match on both sides (resolved)

| Parameter | Value | Notes |
|-----------|-----------|---------------------|
| `link-id` | `0x04` (kernel) ↔ MCU encoding | **Resolved:** reconciled to a valid MCU channel (0–3); the link comes up on both sides. |
| Mailbox channel | rx=ch0 / tx=ch3 (kernel) ↔ `RL_RV1106_MBOX_CHAN=0` (MCU) | **Resolved:** the kernel's per-direction channels map to the MCU's single bidirectional channel (A2B/B2A). |
| A2B/B2A direction | MCU = remote (B2A sends, receives A2B) | **B2A (MCU→A7) works.** **A2B (A7→MCU) is not readable by the MCU by IRQ** → the MCU **polls the vrings**; the mailbox ACK is "blind". |
| NS channel names | see below | Five endpoints announced by the MCU (was one at bring-up). |
| `RL_RPMSG_MAGIC` | `0x524D5347` ("RMSG") | Equal on both. ✔ |

**Live services / endpoints** (rpmsg-lite payload **≤ 496 B**, larger messages chunked):

| Service | NS name | Endpoint | Linux client |
|---------|---------|----------|--------------|
| PingEcho | `rpmsg-ap3-ch0` | `0x4004` | — |
| RadioService | `rpmsg-radio` | `0x4005` | `radio_test` |
| SensorService | `rpmsg-sensor` | `0x4006` | `sensor_test` |
| TelemetryService | `rpmsg-telemetry` | `0x4007` | `radio_test tlm` |
| CommandService | `rpmsg-command` | `0x4008` | `radio_test cmd_* / tcsend` |

## 4. Done — Linux side (applied)

1. **Kernel match** `src/kernel/drivers/rpmsg/rockchip_rpmsg.c`: `RV1106` added to the enum
   (`:29-32`) and `{ .compatible = "rockchip,rv1106-rpmsg", .data = (void *)RV1106 }` in the
   table (`:402-406`). (The `chip` field is cosmetic; no per-chip data.)
2. **Device tree** (in `dts/`, with the **double include** of `rv1106-amp.dtsi`
   via `rv1106-evb.dtsi` *and* `rv1106-sdk-ipc.dtsi` resolved to a single include):
   - `rpmsg@0ff00000` node (`compatible="rockchip,rv1106-rpmsg"`, `mbox-names="rpmsg-rx","rpmsg-tx"`,
     `mboxes=<&mailbox 0 &mailbox 3>`, `rockchip,link-id`, `rockchip,vdev-nums=<1>`,
     `reg=<0x0ff00000 0x20000>`, `memory-region=<&rpmsg_dma_reserved>`).
   - `reserved-memory`: `rpmsg_reserved@0ff00000` (`no-map`) + `rpmsg_dma_reserved` (`shared-dma-pool`).
   - `&mailbox { status = "okay"; };` (was `disabled` in `rv1106.dtsi`).
   - `rv1106-amp.dtsi` (clocks `CLK_CORE_MCU`/`PCLK_MAILBOX`) included a single time.
3. **defconfig** `configs/kernel/rv1106_minimal_defconfig`:
   `CONFIG_MAILBOX=y`, `CONFIG_ROCKCHIP_MBOX=y`, `CONFIG_RPMSG_ROCKCHIP=y`,
   `CONFIG_RPMSG_ROCKCHIP_TEST=y`, `CONFIG_RPMSG_VIRTIO=y` (`ROCKCHIP_AMP` was already there).
   After `make oldconfig`, `VIRTIO`/`RPMSG` stay active.

## 5. Done — MCU side

1. **Linker** `src/mcu/.../rv1106-mcu/link.lds`:
   `__linux_share_rpmsg_start__`/`__linux_share_rpmsg_end__` at `0x0FF00000` (DDR, outside the MCU's SRAM RAM).
2. **Service dispatcher** (Style B, direct rpmsg-lite — avoids the nonexistent `rpmsg_base.h`).
   PingEcho brings the link up and hosts the single poll thread that ticks every service;
   the pattern was then generalized to the five endpoints of §3. Bring-up skeleton
   (`INIT_APP_EXPORT(ping_echo_init)`):
   ```c
   instance = rpmsg_lite_remote_init((void*)RPMSG_LINUX_MEM_BASE,
                  RL_PLATFORM_SET_LINK_ID(0 /*A7*/, R /*MCU*/), RL_NO_FLAGS);
   rpmsg_lite_wait_for_link_up(instance);
   ept = rpmsg_lite_create_ept(instance, EPT_ADDR, echo_cb, instance);
   rpmsg_ns_announce(instance, ept, "rpmsg-ap3-ch0", RL_NS_CREATE);
   /* echo_cb: rpmsg_lite_send(instance, ept, src, payload, len, RL_BLOCK); */
   ```
   Model: `src/mcu/.../hal/project/rk3562-mcu/src/test_demo.c:467-526`.
3. **defconfig** `board/pwncube/defconfig`: `CONFIG_RT_USING_RPMSG_LITE=y`
   (+ the dispatcher symbols). `HAL_MBOX_MODULE_ENABLED` is in `hal_conf.h`.

## 6. Flash and test (done)

1. `./build.sh mcu` → `rtthread.bin`; integrated as `LOADER2=Hpmcu` in the rkbin flow
   (the CubeSat RV1106 INI already supports `Hpmcu`) and `./build.sh` + `./build.sh flash`
   (maskrom + `upgrade_tool`). Definitive boot wiring is in [80-dual-boot.md](80-dual-boot.md).
2. On boot, Linux exposes the endpoints under `/dev/rpmsg*`; the MCU announces the five NS
   channels. The bring-up PING/ECHO on `rpmsg-ap3-ch0` was the first thing verified.
3. The co-design parameters (§3) and the IRQ/mailbox semantics of `rpmsg_platform.c` were
   resolved on hardware; the loopback proof is in [70-mailbox-loopback-test.md](70-mailbox-loopback-test.md).

## 6bis. HPMCU boot — real mechanism (on-board findings)

> **⚠️ SUPERSEDED.** This section is HISTORICAL: the definitive
> boot (bootrom→0x40000 + FIT `mcu0` with direct release, WITHOUT wrap nor
> trampoline) and the final transport are in docs **80** and **90**. The
> findings "the bootrom did not work" and "the A7 cannot write 0x40000" were
> artifacts of the bugs of that time (kernel@0x8000, MCU1 without release, DCACHE).

After an extensive on-board bring-up the exact mechanism and where the blockage is were determined:

- **Firmware load:** the HPMCU firmware runs from the **MCU's IRAM at 0x40000**
  (`link.lds` ORIGIN=0x40000). The A7 **cannot** write that IRAM directly (in the A7's
  map, 0x40000 is DDR/System RAM). That is why Rockchip interposes the **`rv1106_hpmcu_wrap`** in
  `hpmcu_sram` (`0xff6fe000`, 8 KB, accessible by A7).
- **Core release:** U-Boot SPL `spl_fit_standalone_release("mcu0"/"mcu1", 0xff6fe000)` (generated
  from the trust INI `[MCU]` set to `okay`) sets `SGRF_HPMCU_BOOT_ADDR` (`0xff076044`) and, for `mcu0`,
  deasserts the reset. **Verified on-board:** `0xff076044 = 0xff6fe000`. The core boots the wrap.
- **The wrap is a MAILBOX COMMAND SERVER (not a flash loader):** disassembly
  (`rv1106_hpmcu_wrap_v1.70.bin`) — initializes clocks/cache (`0xff6ff004`), and enters a loop
  (`0xff6fe494`) that **polls `A2B_CMD(0)`/`A2B_DAT(0)` of mailbox `0xff5c0000`** and dispatches via an
  18-command table: one receives `(address,length)` and **writes data to memory** (loads the
  firmware to 0x40000), another jumps to the entry. That is, **the A7 must send it the firmware
  command by command over mailbox and order it to jump**.
- **BLOCKAGE:** that A7 driver (which drives the wrap over mailbox) **is not in the open-source U-Boot/kernel**
  of the SDK; Rockchip drives it from a closed component in its camera/AOV path.
  With the correct TB config the wrap is released (`SGRF` set) but stays **waiting for commands that
  nobody sends** → our rtthread never gets loaded (`mailbox A2B_INTEN`=0, heartbeat at 0xff6ff800 not
  written). The Linux rpmsg side, by contrast, **does work** (`rpmsg host is online`).
- **DO NOT:** release the core to a **DDR** address (e.g. `mcu0=rtthread,0x0fe00000`) **hangs
  the SPL and bricks** the boot (recovery: short the CLK pin of the SPI-NAND → bootrom maskrom
  → `upgrade_tool UF` with `[MCU]` disabled). The IRAM/SRAM (0x40000 / 0xff6fe000) is safe.

### Definitive finding (verified on-board)
- **The A7 (U-Boot SPL) CANNOT write the HPMCU's IRAM at 0x40000.** The clean route
  `MCU0=rtthread,0x40000,okay` was tested (FIT standalone mcu0 → `spl_fit_standalone_release` loads to 0x40000 +
  full reset release): **Linux boots without brick** (releasing to IRAM is SAFE, unlike
  DDR), `SGRF` set, **but rtthread `main()` does NOT run** — a heartbeat written to the register
  `B2A_DAT(0)` of the mailbox (`0xff5c0034`, read with certainty by the A7) **and** in `hpmcu_sram`
  (`0xff6ff800`) stays at 0 / garbage, `A2B_INTEN`=0. The SPL load to 0x40000 lands in **A7's DDR**
  (useless for the MCU) and the released core runs an empty IRAM. **The MCU's IRAM can only be
  filled by the MCU itself (the wrap) or the bootrom.**
- **The A7 driver that drives the wrap is CLOSED** (`rockit.ko` / `mcu.S` in Luckfox): `mcu_send_message`
  writes `A2B_CMD0=0xff5c0008`/`A2B_DAT0=0xff5c000c`; deciphered opcodes: `cmd6`=config,
  `cmd1`=(physical DDR address of the firmware), `cmd5`=start, `cmd9`=reset. The wrap **copies the firmware from
  that DDR address to the IRAM 0x40000 and boots it**.
- Packaging note: putting rtthread into the FIT of `uboot.img` requires `CONFIG_SPL_FIT_IMAGE_KB` that
  does not exceed the 256K `uboot` partition (512 pads it to 512K → flashing error "Image larger than
  partition"; 256 does fit).

### Path that shipped: bootrom Hpmcu (not the wrap, not a trampoline)
The trampoline idea (a RISC-V stub linked for `0xff6fe000` that copies `rtthread.bin` to `0x40000`)
was one considered route. The **shipped** solution is simpler: the bootrom's `Hpmcu` loader in the
idblock places `rtthread.bin` at `0x40000`, and U-Boot SPL releases the SCR1 directly there via
`spl_fit_standalone_release("mcu0", 0x40000)` — no wrap, no closed mailbox protocol. See
[80-dual-boot.md](80-dual-boot.md) §2.

## 7. Resolved risks (from the on-board bring-up)
- Collision of `0x0FF00000` with OP-TEE/trust: **resolved** — verified no collision (`trust.img` loads elsewhere).
- Mapping `link-id`↔channel↔direction (§3): **resolved** — the link comes up on both sides.
- IRQ acking semantics in SCR1/PLIC: A2B is **not** deliverable to the MCU by interrupt (the SCR1 port
  has no `rt_hw_interrupt_ack`) → the MCU **polls the vrings** and the mailbox ACK is "blind". This is
  the accepted transport shape, not an open bug.
- Cache coherence in the shared DDR region: **resolved** — the MCU runs with `RT_USING_CACHE`
  disabled, so A7↔MCU shared memory is coherent (see [80-dual-boot.md](80-dual-boot.md) §3 bug 5 and
  [70-mailbox-loopback-test.md](70-mailbox-loopback-test.md)).
