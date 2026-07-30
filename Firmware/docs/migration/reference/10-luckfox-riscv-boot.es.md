# Referencia Luckfox — Soporte RISC-V (HPMCU / RT-Thread)

> **Naturaleza del documento.** Estudio del **SDK Luckfox Pico** (referencia externa)
> como referencia técnica. Todas las rutas son relativas a ese SDK. No describe
> este repo CubeSat. Referencia técnica del SoC.

El RV1106 integra un Cortex-A7 (Linux) y un coprocesador **RISC-V** que Rockchip
denomina **HPMCU** ("High-Performance MCU"). El HPMCU ejecuta **RT-Thread**. En el
SDK oficial su uso típico es el pipeline de imagen/ISP y el *wake-on-motion*; en
el CubeSat se reutiliza como controlador determinista de hardware (esto ya corre
en hardware — ver [`../../architecture/overview.md`](../../architecture/overview.md)).

## 1. CPU, toolchain y flags de compilación

- **Arquitectura / núcleo:** `ARCH = 'risc-v'`, `CPU = 'scr1'` (Syntacore SCR1, RISC-V de 32 bits).
  → `sysdrv/source/mcu/rt-thread/bsp/rockchip/rv1106-mcu/rtconfig.py:3`
- **Cross-prefix:** `PREFIX = 'riscv-none-embed-'`.
  → `rtconfig.py:28`
- **Toolchain:** xPack `riscv-none-embed-gcc` **10.2.0-1.2**, descargado por el build a
  `.../prebuilts/gcc/linux-x86/riscv64/xpack-riscv-none-embed-gcc-10.2.0-1.2/`.
  → `rtconfig.py:13`; descarga en `project/build.sh:883-894`.
- **Flags de máquina:**
  `-march=rv32imc -mabi=ilp32 -DUSE_PLIC -DUSE_M_TIME -DNO_INIT -mcmodel=medany -msmall-data-limit=8 -nostartfiles -lc`
  → `rtconfig.py:38`. (El proyecto de test de la HAL usa `-march=rv32imac`, añadiendo
  el extension atómica `a`: `.../common/hal/project/rv1106-mcu/GCC/Makefile:7`.)
- **Optimización:** `-Os` en *release*. → `rtconfig.py:57`

**Implicación para CubeSat:** el código migrado debe compilar con `rv32imc/ilp32`
(sin FPU hardware — usar enteros/punto fijo; los drivers actuales ya usan punto
fijo, ver doc 30). Es el mismo toolchain RISC-V que ahora está integrado en el
`build.sh` del CubeSat.

## 2. Estructura RT-Thread del BSP `rv1106-mcu`

Raíz: `sysdrv/source/mcu/rt-thread/bsp/rockchip/rv1106-mcu/`

| Ruta | Contenido |
|------|-----------|
| `applications/main.c` | Arranque del RTOS y `main()`. |
| `board/<variante>/` | 25+ variantes (por sensor de cámara): `board.c`, `iomux.c`, `defconfig`. |
| `cpu/` | Específico del núcleo SCR1 (`riscv_csr_encoding.h`, `scr1_specific.h`). |
| `drivers/` | `timer.c`, `int_mux.c` (multiplexado de interrupciones). |
| `link.lds` | Linker script (mapa de memoria SRAM). |
| `rtconfig.h` / `rtconfig.py` | Config generada / config de build. |
| `SConstruct` / `SConscript` | Build basado en **SCons**. |

La HAL común (compartida con rk3562/rk3568/rv1126) está en
`.../bsp/rockchip/common/hal/` y los drivers RT-Thread comunes en
`.../bsp/rockchip/common/drivers/`.

## 3. Arranque del firmware (entry point)

- **Startup en ensamblador:** `_start` en
  `.../common/hal/lib/CMSIS/Device/RV1106/Source/Templates/GCC/start_rv1106_mcu.S:16,59-70`.
  Secuencia: inicializa `gp`, fija `sp` en `__C_STACK_TOP__`, `data_section_fixup`,
  `SystemInit` (weak), `entry` (weak), `main`.
- **Trap/IRQ machine-mode:** vector en `start_rv1106_mcu.S:24-55`; el handler real
  `scr1_trap_entry` (líneas 98-175) salva x1-x31, conmuta a stack de interrupción y
  llama a `HAL_RISCVIC_IRQHandler(cause, pc, frame)`. Retorna con `mret`.
- **Arranque del RTOS:** `rtthread_startup()` en `applications/main.c:20-62`
  (board init → tick → objetos → timers → scheduler → idle → `rt_system_scheduler_start()`).
  Hay un retorno temprano opcional según *wakeup reason* (`main.c:22-29`) usado por
  el modo AOV/wake-on-motion.

## 4. Mapa de memoria del MCU

**Linker script** `.../rv1106-mcu/link.lds:1-6`:

```
OUTPUT_ARCH( "riscv" )
ENTRY(_start)
MEMORY {
    RAM (rwx) : ORIGIN = 0x40000, LENGTH = 0x3c000   /* 240 KB: 0x40000–0x7c000 */
}
```

Secciones (`link.lds`): `.text` (13-44) · `.data` (46-52) · `.sdata` (54-59,
optimización por `gp`) · `.tdata/.tbss` TLS (61-68) · `.sbss/.bss` (78-88) ·
`.stack` (90-97) · `.save_data` NOLOAD (99-108) · `._user_heap` (111-122, hasta el
fin de RAM).

> El linker script CMSIS alternativo (proyectos de test de la HAL) usa
> `ORIGIN=0x40000, LENGTH=0x40000` (256 KB):
> `.../common/hal/lib/CMSIS/Device/RV1106/Source/Templates/GCC/gcc_riscv.ld:4-6`.

**SRAM compartida vista por Linux** (device tree del SDK)
`sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi`:

```
system_sram: sram@ff6c0000 {           // 256 KB de system SRAM
    reg = <0xff6c0000 0x40000>;
    rkisp_sram:  rkisp-sram@0     { reg = <0x0     0x3e000>; };  // 248 KB ISP
    hpmcu_sram:  hpmcu-sram@3e000 { reg = <0x3e000 0x2000>;  };  // 8 KB HPMCU
};
```

→ La ventana del HPMCU en el espacio de Linux es `0xff6fe000–0xff700000` (**8 KB**),
pensada para intercambio puntual de datos. (El transporte RPMsg de mayor volumen usa
vrings en DDR reservada; ver doc 20.)

**Presupuesto de memoria (restricción de diseño):** 240 KB para RT-Thread + drivers
migrados. El SX1262 tiene una capa de comandos grande (`sx1262_cmd.c`, 769 líneas;
doc 30); cabe, pero obliga a vigilar el *footprint* (sin `printf` de coma flotante,
*heap* acotado).

## 5. Carga y arranque del firmware (U-Boot SPL → RISC-V)

El HPMCU se carga **antes que Linux**, desde el SPL de U-Boot. No está bajo el
*remoteproc* de Linux (Linux no lo arranca ni lo apaga en runtime).

**Empaquetado en la imagen de arranque (rkbin):**
`sysdrv/source/uboot/rkbin/RKBOOT/RV1106MINIALL_EMMC_TB.ini`:

```
NUM=3
LOADER1=FlashData
LOADER2=Hpmcu                 # <- el firmware RISC-V (rtthread.bin)
LOADER3=FlashBoot
...
Hpmcu=bin/rv11/rv1106_hpmcu_tb_v1.01.bin
[LOADER2_PARAM]
LOAD_ADDR=0x40000             # coincide con ORIGIN del link.lds
FLAG=0x10007
```

→ `.ini` líneas ~14-23. El build sustituye la ruta del binario por el `rtthread.bin`
compilado: `project/build.sh:699-728` (`__modify_file ... "Hpmcu=" "$RK_PROJECT_FILE_SYSDRV_MCU_BIN"`),
con `RK_PROJECT_FILE_SYSDRV_MCU_BIN = $RK_PROJECT_PATH_MCU/rtthread.bin`
(`project/build.sh:70`).

**Liberación del MCU desde el SPL:**
`sysdrv/source/uboot/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c:548-566`
`spl_fit_standalone_release(id, entry_point)`:

```c
// para "mcu0":
writel(0xff000,  CORE_GRF_BASE + CORE_GRF_CACHE_PERI_ADDR_START); // región no cacheable
writel(0xffc00,  CORE_GRF_BASE + CORE_GRF_CACHE_PERI_ADDR_END);
writel(0x1e001e, CORECRU_BASE + CORECRU_CORESOFTRST_CON01);       // assert reset
writel(entry_point, CORE_SGRF_BASE + CORE_SGRF_HPMCU_BOOT_ADDR);  // dir. de arranque (0x40000)
writel(0x1e0000, CORECRU_BASE + CORECRU_CORESOFTRST_CON01);       // deassert reset -> arranca
```

Registros: `CORE_SGRF_HPMCU_BOOT_ADDR=0x0044` sobre `CORE_SGRF_BASE=0xff076000`;
`CORECRU_BASE=0xff3b8000`, `CORECRU_CORESOFTRST_CON01=0xa04`
(`rv1106.c:39,46,109,110`). `rk_meta_process()` (`rv1106.c:568-571`) ajusta
`CORE_GRF_MCU_CACHE_MISC`.

## 6. Build: puntos de entrada

- **Orquestación de alto nivel:** `project/build.sh:876-920` → `build_mcu()`
  (descarga toolchain, selecciona placa, invoca el build del MCU).
- **Build del MCU:** `sysdrv/source/mcu/project/build.sh` → invoca SCons:
  `scons -C .../rv1106-mcu -j${RK_JOBS}` (`:279`) y copia `rtthread.bin` a la salida
  (`:321-323`).
- **Selección de placa (defconfig):** `sysdrv/source/mcu/build.sh lunch [placa]`,
  luego `... all`. Las placas viven en `.../rv1106-mcu/board/`.

## 7. Qué se reutiliza para el CubeSat

| Elemento Luckfox | Reutilización en CubeSat (hecho) |
|------------------|--------------------------|
| BSP `rv1106-mcu` + HAL común | **Base** del firmware RISC-V. Se partió de una variante de `board/` reducida (sin sensores de cámara). |
| Toolchain `riscv-none-embed-gcc 10.2.0` | Integrado en el `build.sh` del CubeSat (target `mcu`). |
| `link.lds` (0x40000/240 KB) | Reutilizado tal cual; footprint dentro de presupuesto. |
| Flujo SPL `Hpmcu` / `spl_fit_standalone_release` | Mecanismo de carga reutilizado; el CubeSat usa rkbin (ver `src/rkbin`, `docs/uboot.md`). |
| `start_rv1106_mcu.S`, `HAL_RISCVIC_IRQHandler` | Sin cambios (arranque e IRQs del núcleo). |

**Resuelto (eran riesgos durante la migración):** (a) qué *board variant* mínima
usar como punto de partida; (b) que el flujo rkbin del CubeSat acepte el
empaquetado `LOADER2=Hpmcu` del firmware de MCU; (c) presupuesto real de RAM tras
enlazar RT-Thread + drivers. Todo confirmado en hardware durante el bring-up —
ver los docs de implementación (60, 80,
[`90`](../implementation/90-mcu-config-replication.md)) y
[`../../architecture/overview.md`](../../architecture/overview.md).
