# 80 — Dual boot A7 (Linux) + HPMCU (RT-Thread): architecture and debugging

> **Status: WORKING.** Linux boots to shell with the kernel
> at `0x208000` and the MCU runs RT-Thread with a heartbeat advancing
> (`devmem 0xff6ff900` → increasing `0xB000xxxx`) indefinitely. This
> document settles the boot architecture that makes it possible, the chain of
> five bugs that prevented it, the correct flashing procedure and the
> diagnostic instruments that were left in the tree.
> For the settled system picture see
> [../../architecture/overview.md](../../architecture/overview.md).

## 1. Boot memory map (definitive)

| Physical region | Use | Who defines it |
|---|---|---|
| `0x00000000-0x0003ffff` | Free Linux RAM (below the MCU) | — |
| `0x00040000-0x0007bfff` | **MCU firmware (rtthread.bin)**, the SCR1 runs it | `link.lds` ORIGIN, RKBOOT `LOAD_ADDR`, reserve `rtos@40000` |
| `0x0007c000-0x0007efff` | ramoops / MCU log | reserve `ramoops@7c000` |
| `0x00200000` | U-Boot proper (loaded by the SPL; then relocated to high RAM) | SPL |
| `0x00204000-0x00207fff` | Kernel initial page tables (`swapper_pg_dir`) | TEXT_OFFSET−0x4000 |
| `0x00208000` | **Linux kernel (decompressed Image)**; `_text=0xb0208000`, PHYS_OFFSET=0 | `boot.its` load/entry, `kernel_addr_r`, textofs |
| `0x00c00000` | DTB (`fdt_addr_r`) | `rv1106_common.h` |
| `0x0fe00000-0x0fefffff` | mcu@fe00000 (legacy staging, reserved no-map) | dtsi |
| `0x0ff00000-0x0ff7ffff` | rpmsg vrings | reserve `rpmsg@ff00000` (no-map) |
| `0x0ff80000-0x0fffffff` | rpmsg DMA pool | reserve `rpmsg-dma@ff80000` (no-map) |
| `0xff5c0000` | MAILBOX A7↔MCU | — |
| `0xff640000` | SCR1 DCACHE controller (bus error from the A7!) | — |
| `0xff6fe000-0xff6ffxxx` | SRAM: wrap (no longer used) + diagnostic markers | — |

Design keys:

- The kernel is linked **and** loaded at `0x208000` → `PHYS_OFFSET = 0` → RAM
  from `0x0` without loss, and the MCU at `0x40000` is protected by a
  **simple reservation (no `no-map`)**.
- `System RAM` ends at `0x0fdfffff`: the "Ignoring RAM" branch (without HIGHMEM)
  trims the no-map tail `0xfe00000-0x10000000`, which is correct (those
  regions are accessed via `memremap`, they are not system RAM).

## 2. Boot flow

```
bootrom ─ loads idblock: ddrbin → Hpmcu(rtthread.bin→0x40000, FLAG=0x10007) → SPL
SPL     ─ loads uboot.img (FIT):
          · mcu0 = rtthread.bin → spl_fit_standalone_release("mcu0", 0x40000):
              cache GRF (0xff04002c=0x00080008, peri window) →
              hold reset SCR1 (CORECRU 0xa04=0x1e001e) →
              SGRF HPMCU_BOOT_ADDR (0xff076044)=0x40000 →
              RELEASE (0xa04=0x1e0000)            ← the MCU boots HERE
          · uboot → 0x200000, fdt
U-Boot  ─ boot_fit: decompress gzip kernel → 0x208000, fdt → 0xc00000, bootm
Kernel  ─ head.S (PHYS_OFFSET=0) → paging_init → ... → shell
```

- **`mcu0`, not `mcu1`**: in `src/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c`
  `spl_fit_standalone_release()` only the id `"mcu0"` performs the release;
  `"mcu1"` only sets the address (WAKEUP-type flows where the kernel
  releases it later). It is configured in `src/rkbin/RKTRUST/RV1106TOS_TB.ini`.
- **The Rockchip wrap (`rv1106_hpmcu_wrap`) was removed from the flow**: it is a
  command interpreter over mailbox (it waits for orders from `rk_meta_process()` of the
  prebuilt SPL of rkbin); with our source SPL it never jumps to `0x40000`.
  Releasing the SCR1 directly to `0x40000` is proven silicon behavior (the
  Rockchip WAKEUP flow does exactly that).

## 3. The chain of five bugs (history and the why of each)

1. **Kernel at `0x8000` clobbered the MCU** (`zImage` with AUTO_ZRELADDR
   self-decompresses to a fixed `0x8000`). *Solution:* FIT with raw `Image` compressed
   gzip and `load/entry=0x208000` (`src/kernel/boot.its`); `mkimg` compresses the
   Image itself (make's `Image.gz` target **runs in parallel** with
   `zImage` under `-j` and can come out empty); `kernel_addr_r=0x208000`
   (`rv1106_common.h`); `CONFIG_GZIP=y` in U-Boot.
2. **TEXT_OFFSET**: `textofs-$(CONFIG_CPU_RV1106) := 0x208000` was inside
   `ifeq (CONFIG_ROCKCHIP_THUNDER_BOOT,y)` in `arch/arm/Makefile` → kernel
   linked for `0x8000` but loaded at `0x208000` → PHYS_OFFSET=0x200000 not
   aligned to 16MiB → `__fixup_pv_table` → `bne __error` **before earlycon**.
   *Solution:* the RV1106 line was moved out of the ifeq.
3. **`no-map` on `rtos@40000` wiped all RAM**: the NOMAP hole makes the
   first usable block end at `0x40000` (not aligned to PMD/2MB) →
   `adjust_lowmem_bounds()` sets `memblock_limit=0x40000` → `round_down` → 0 →
   without HIGHMEM it enters "Ignoring RAM" → `memblock_remove(0, 256MB)` → panic in
   `early_alloc` in `paging_init`. *Solution:* `rtos@40000` and `ramoops@7c000` are
   **simple reservations** (Luckfox `rv1106-thunder-boot.dtsi` style), and the DTS
   carries an explicit `/memory` node. *Note:* this bug was latent — with the
   kernel at `0x8000` the reservation collided with the kernel's (`-EBUSY`) and never
   applied (which is exactly why Linux could clobber the MCU).
4. **The SCR1 was never released** (wrap as `mcu1`; see §2).
5. **The MCU's SystemInit hangs initializing the DCACHE** (`0xff640000`,
   infinite polling of `CACHE_INIT_FINISH`; the A7 receives a bus error reading that
   block). *Provisional solution:* `# CONFIG_RT_USING_CACHE is not set` in
   `board/pwncube/defconfig` — the MCU runs without cache, which also guarantees
   coherency of the IPC shared-memory (see
  [70-mailbox-loopback-test.md](70-mailbox-loopback-test.md)). *Pending:* port the
   wrap's init sequence (it is disassembled: `CTRL|=3`, `&=~8`, poll
   `+0x30` bit0, `&=~0x40`, `|=0x781`, `+0x0c=1`, poll, `&=~0x40`) or find
   the clock/GRF that enables the block.

## 4. Flashing (correct procedure)

```sh
# ALWAYS the full image; DI -b says "ok" but does NOT write on this SPI-NAND
./scripts/02-build-kernel.sh          # if kernel/dts changed
bash scripts/06-build-mcu.sh          # if the MCU firmware changed
cp -f output/mcu/rtthread.bin src/rkbin/bin/rv11/rtthread.bin   # after 06
./scripts/01-build-uboot.sh           # if rtthread/uboot/inis changed (re-packs idblock+uboot.img)
./scripts/04-pack-image.sh            # ALWAYS before flashing
tools/upgrade_tool UF output/images/update.img
```

To enter maskrom without touching the board: `reboot loader` from the Linux
shell. With the board hung: recovery button held while powering on (or short
CLK-GND of the SPI-NAND) — **release the button as soon as flashing starts**, or
the post-UF reboot stays silent in the bootrom's RKUART mode.

## 5. Runtime diagnostics (from Linux)

| Reading | Healthy value | Meaning |
|---|---|---|
| `devmem 0xff6ff900` | **increasing** `0xB000xxxx` | MCU heartbeat (main loop alive) |
| `devmem 0xff6ff800` | `0xCAFE0001` | MCU `main()` reached |
| `devmem 0xff076044` | `0x00040000` | SCR1 boot addr |
| `devmem 0xff3b8a04` | `0x00000000` | SCR1 resets released |
| `devmem 0x40000` | `0x0000A401` | rtthread.bin intact in place |
| `head -3 /proc/iomem` | `Kernel code 0x208000-...` | Kernel where it should be |

MCU boot stage markers (TEMP, see §6): `0xff5c0030=0xCAFE0005`
(_start), `0xff6ff810/814` (stack/data), `0xff6ff81c=0xCAFE0009` (SystemInit
OK), `0xff6ff820..830=0xCAFE000A..000E` (stages of `rt_hw_board_init`). The
first marker with garbage indicates the exact stage of the failure.

## 6. Temporary instrumentation to remove once the system is declared stable

- **Kernel**: `earlyprintk`+`earlycon` in bootargs (`dts/rv1106g-sdk.dts`);
  `CONFIG_DEBUG_LL*`/`CONFIG_EARLY_PRINTK` block of the defconfig; sentinels
  `'S'`/`'P'` in `arch/arm/kernel/head.S`, `'M'` in `head-common.S`,
  `printascii` in `init/main.c`; memblock dump in
  `arch/arm/mm/mmu.c:early_alloc()`.
- **MCU**: CAFE markers in `start_rv1106_mcu.S` and
  `board/common/board_base.c`; those in `main.c`/`ping_echo.c`.
- **U-Boot**: the `MCU_CACHE_MISC` write in `arch_cpu_init()` (redundant with
  the one in the mcu0 branch; harmless).
- Re-enable `RT_USING_CACHE` once point 5 of §3 is resolved (evaluating the
  impact on the coherency of the shared-memory IPC).
