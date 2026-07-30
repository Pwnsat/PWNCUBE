# Luckfox Reference — RISC-V Support (HPMCU / RT-Thread)

> **Nature of the document.** Study of the **Luckfox Pico SDK** (external reference)
> as a technical reference. All paths are relative to that SDK. It does not describe
> this CubeSat repo. Technical reference for the SoC.

The RV1106 integrates a Cortex-A7 (Linux) and a **RISC-V** coprocessor that Rockchip
calls **HPMCU** ("High-Performance MCU"). The HPMCU runs **RT-Thread**. In the
official SDK its typical use is the image/ISP pipeline and *wake-on-motion*; on
the CubeSat it is reused as a deterministic hardware controller (this is now
running on hardware — see [`../../architecture/overview.md`](../../architecture/overview.md)).

## 1. CPU, toolchain and compilation flags

- **Architecture / core:** `ARCH = 'risc-v'`, `CPU = 'scr1'` (Syntacore SCR1, 32-bit RISC-V).
  → `sysdrv/source/mcu/rt-thread/bsp/rockchip/rv1106-mcu/rtconfig.py:3`
- **Cross-prefix:** `PREFIX = 'riscv-none-embed-'`.
  → `rtconfig.py:28`
- **Toolchain:** xPack `riscv-none-embed-gcc` **10.2.0-1.2**, downloaded by the build to
  `.../prebuilts/gcc/linux-x86/riscv64/xpack-riscv-none-embed-gcc-10.2.0-1.2/`.
  → `rtconfig.py:13`; download in `project/build.sh:883-894`.
- **Machine flags:**
  `-march=rv32imc -mabi=ilp32 -DUSE_PLIC -DUSE_M_TIME -DNO_INIT -mcmodel=medany -msmall-data-limit=8 -nostartfiles -lc`
  → `rtconfig.py:38`. (The HAL test project uses `-march=rv32imac`, adding
  the atomic extension `a`: `.../common/hal/project/rv1106-mcu/GCC/Makefile:7`.)
- **Optimization:** `-Os` in *release*. → `rtconfig.py:57`

**Implication for CubeSat:** the migrated code must compile with `rv32imc/ilp32`
(no hardware FPU — use integers/fixed point; the current drivers already use fixed
point, see doc 30). It is the same RISC-V toolchain that is now integrated into
the CubeSat's `build.sh`.

## 2. RT-Thread structure of the `rv1106-mcu` BSP

Root: `sysdrv/source/mcu/rt-thread/bsp/rockchip/rv1106-mcu/`

| Path | Contents |
|------|-----------|
| `applications/main.c` | RTOS boot and `main()`. |
| `board/<variant>/` | 25+ variants (by camera sensor): `board.c`, `iomux.c`, `defconfig`. |
| `cpu/` | Specific to the SCR1 core (`riscv_csr_encoding.h`, `scr1_specific.h`). |
| `drivers/` | `timer.c`, `int_mux.c` (interrupt multiplexing). |
| `link.lds` | Linker script (SRAM memory map). |
| `rtconfig.h` / `rtconfig.py` | Generated config / build config. |
| `SConstruct` / `SConscript` | Build based on **SCons**. |

The common HAL (shared with rk3562/rk3568/rv1126) is in
`.../bsp/rockchip/common/hal/` and the common RT-Thread drivers in
`.../bsp/rockchip/common/drivers/`.

## 3. Firmware boot (entry point)

- **Startup in assembly:** `_start` in
  `.../common/hal/lib/CMSIS/Device/RV1106/Source/Templates/GCC/start_rv1106_mcu.S:16,59-70`.
  Sequence: initializes `gp`, sets `sp` to `__C_STACK_TOP__`, `data_section_fixup`,
  `SystemInit` (weak), `entry` (weak), `main`.
- **Machine-mode trap/IRQ:** vector in `start_rv1106_mcu.S:24-55`; the real handler
  `scr1_trap_entry` (lines 98-175) saves x1-x31, switches to the interrupt stack and
  calls `HAL_RISCVIC_IRQHandler(cause, pc, frame)`. Returns with `mret`.
- **RTOS boot:** `rtthread_startup()` in `applications/main.c:20-62`
  (board init → tick → objects → timers → scheduler → idle → `rt_system_scheduler_start()`).
  There is an optional early return depending on the *wakeup reason* (`main.c:22-29`) used by
  the AOV/wake-on-motion mode.

## 4. MCU memory map

**Linker script** `.../rv1106-mcu/link.lds:1-6`:

```
OUTPUT_ARCH( "riscv" )
ENTRY(_start)
MEMORY {
    RAM (rwx) : ORIGIN = 0x40000, LENGTH = 0x3c000   /* 240 KB: 0x40000–0x7c000 */
}
```

Sections (`link.lds`): `.text` (13-44) · `.data` (46-52) · `.sdata` (54-59,
optimization via `gp`) · `.tdata/.tbss` TLS (61-68) · `.sbss/.bss` (78-88) ·
`.stack` (90-97) · `.save_data` NOLOAD (99-108) · `._user_heap` (111-122, up to the
end of RAM).

> The alternative CMSIS linker script (HAL test projects) uses
> `ORIGIN=0x40000, LENGTH=0x40000` (256 KB):
> `.../common/hal/lib/CMSIS/Device/RV1106/Source/Templates/GCC/gcc_riscv.ld:4-6`.

**Shared SRAM seen by Linux** (SDK device tree)
`sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi`:

```
system_sram: sram@ff6c0000 {           // 256 KB of system SRAM
    reg = <0xff6c0000 0x40000>;
    rkisp_sram:  rkisp-sram@0     { reg = <0x0     0x3e000>; };  // 248 KB ISP
    hpmcu_sram:  hpmcu-sram@3e000 { reg = <0x3e000 0x2000>;  };  // 8 KB HPMCU
};
```

→ The HPMCU window in the Linux address space is `0xff6fe000–0xff700000` (**8 KB**),
intended for occasional data exchange. (The higher-volume RPMsg transport uses
vrings in reserved DDR; see doc 20.)

**Memory budget (design constraint):** 240 KB for RT-Thread + migrated drivers.
The SX1262 has a large command layer (`sx1262_cmd.c`, 769 lines;
doc 30); it fits, but forces us to watch the *footprint* (no floating-point
`printf`, bounded *heap*).

## 5. Firmware loading and boot (U-Boot SPL → RISC-V)

The HPMCU is loaded **before Linux**, from U-Boot's SPL. It is not under Linux's
*remoteproc* (Linux neither boots it nor powers it down at runtime).

**Packaging in the boot image (rkbin):**
`sysdrv/source/uboot/rkbin/RKBOOT/RV1106MINIALL_EMMC_TB.ini`:

```
NUM=3
LOADER1=FlashData
LOADER2=Hpmcu                 # <- the RISC-V firmware (rtthread.bin)
LOADER3=FlashBoot
...
Hpmcu=bin/rv11/rv1106_hpmcu_tb_v1.01.bin
[LOADER2_PARAM]
LOAD_ADDR=0x40000             # matches ORIGIN of link.lds
FLAG=0x10007
```

→ `.ini` lines ~14-23. The build replaces the binary path with the compiled
`rtthread.bin`: `project/build.sh:699-728` (`__modify_file ... "Hpmcu=" "$RK_PROJECT_FILE_SYSDRV_MCU_BIN"`),
with `RK_PROJECT_FILE_SYSDRV_MCU_BIN = $RK_PROJECT_PATH_MCU/rtthread.bin`
(`project/build.sh:70`).

**Releasing the MCU from the SPL:**
`sysdrv/source/uboot/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c:548-566`
`spl_fit_standalone_release(id, entry_point)`:

```c
// for "mcu0":
writel(0xff000,  CORE_GRF_BASE + CORE_GRF_CACHE_PERI_ADDR_START); // non-cacheable region
writel(0xffc00,  CORE_GRF_BASE + CORE_GRF_CACHE_PERI_ADDR_END);
writel(0x1e001e, CORECRU_BASE + CORECRU_CORESOFTRST_CON01);       // assert reset
writel(entry_point, CORE_SGRF_BASE + CORE_SGRF_HPMCU_BOOT_ADDR);  // boot addr. (0x40000)
writel(0x1e0000, CORECRU_BASE + CORECRU_CORESOFTRST_CON01);       // deassert reset -> boots
```

Registers: `CORE_SGRF_HPMCU_BOOT_ADDR=0x0044` over `CORE_SGRF_BASE=0xff076000`;
`CORECRU_BASE=0xff3b8000`, `CORECRU_CORESOFTRST_CON01=0xa04`
(`rv1106.c:39,46,109,110`). `rk_meta_process()` (`rv1106.c:568-571`) adjusts
`CORE_GRF_MCU_CACHE_MISC`.

## 6. Build: entry points

- **High-level orchestration:** `project/build.sh:876-920` → `build_mcu()`
  (downloads toolchain, selects board, invokes the MCU build).
- **MCU build:** `sysdrv/source/mcu/project/build.sh` → invokes SCons:
  `scons -C .../rv1106-mcu -j${RK_JOBS}` (`:279`) and copies `rtthread.bin` to the output
  (`:321-323`).
- **Board selection (defconfig):** `sysdrv/source/mcu/build.sh lunch [board]`,
  then `... all`. The boards live in `.../rv1106-mcu/board/`.

## 7. What is reused for the CubeSat

| Luckfox element | Reuse in CubeSat (done) |
|------------------|--------------------------|
| BSP `rv1106-mcu` + common HAL | **Base** of the RISC-V firmware. Started from a reduced `board/` variant (without camera sensors). |
| Toolchain `riscv-none-embed-gcc 10.2.0` | Integrated into the CubeSat's `build.sh` (`mcu` target). |
| `link.lds` (0x40000/240 KB) | Reused as-is; footprint under budget. |
| SPL `Hpmcu` flow / `spl_fit_standalone_release` | Loading mechanism reused; the CubeSat uses rkbin (see `src/rkbin`, `docs/uboot.md`). |
| `start_rv1106_mcu.S`, `HAL_RISCVIC_IRQHandler` | No changes (core boot and IRQs). |

**Resolved (were risks during the migration):** (a) which minimal *board variant*
to start from; (b) that the CubeSat's rkbin flow accepts the `LOADER2=Hpmcu`
packaging of MCU firmware; (c) real RAM budget after linking RT-Thread + drivers.
All confirmed on hardware during bring-up — see the implementation docs (60, 80,
[`90`](../implementation/90-mcu-config-replication.md)) and
[`../../architecture/overview.md`](../../architecture/overview.md).
