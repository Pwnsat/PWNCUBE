# 80 — Arranque dual A7 (Linux) + HPMCU (RT-Thread): arquitectura y depuración

> **Estado: FUNCIONANDO.** Linux arranca hasta shell con el kernel
> en `0x208000` y el MCU ejecuta RT-Thread con heartbeat avanzando
> (`devmem 0xff6ff900` → `0xB000xxxx` creciente) de forma indefinida. Este
> documento fija la arquitectura de arranque que lo hace posible, la cadena de
> cinco bugs que lo impedía, el procedimiento de flasheo correcto y los
> instrumentos de diagnóstico que quedaron en el árbol.
> Para la foto asentada del sistema ver
> [../../architecture/overview.md](../../architecture/overview.md).

## 1. Mapa de memoria de arranque (definitivo)

| Región física | Uso | Quién la define |
|---|---|---|
| `0x00000000-0x0003ffff` | RAM libre de Linux (bajo el MCU) | — |
| `0x00040000-0x0007bfff` | **Firmware MCU (rtthread.bin)**, ejecuta el SCR1 | `link.lds` ORIGIN, RKBOOT `LOAD_ADDR`, reserva `rtos@40000` |
| `0x0007c000-0x0007efff` | ramoops / log del MCU | reserva `ramoops@7c000` |
| `0x00200000` | U-Boot proper (lo carga el SPL; luego se reubica a RAM alta) | SPL |
| `0x00204000-0x00207fff` | Tablas de página iniciales del kernel (`swapper_pg_dir`) | TEXT_OFFSET−0x4000 |
| `0x00208000` | **Kernel Linux (Image descomprimida)**; `_text=0xb0208000`, PHYS_OFFSET=0 | `boot.its` load/entry, `kernel_addr_r`, textofs |
| `0x00c00000` | DTB (`fdt_addr_r`) | `rv1106_common.h` |
| `0x0fe00000-0x0fefffff` | mcu@fe00000 (staging legado, reservado no-map) | dtsi |
| `0x0ff00000-0x0ff7ffff` | vrings rpmsg | reserva `rpmsg@ff00000` (no-map) |
| `0x0ff80000-0x0fffffff` | pool DMA rpmsg | reserva `rpmsg-dma@ff80000` (no-map) |
| `0xff5c0000` | MAILBOX A7↔MCU | — |
| `0xff640000` | Controlador DCACHE del SCR1 (¡bus error desde el A7!) | — |
| `0xff6fe000-0xff6ffxxx` | SRAM: wrap (sin uso ya) + marcadores de diagnóstico | — |

Claves del diseño:

- El kernel se enlaza **y** se carga en `0x208000` → `PHYS_OFFSET = 0` → RAM
  desde `0x0` sin pérdida, y el MCU en `0x40000` queda protegido por una
  **reserva simple (sin `no-map`)**.
- `System RAM` termina en `0x0fdfffff`: el branch "Ignoring RAM" (sin HIGHMEM)
  recorta la cola no-map `0xfe00000-0x10000000`, lo cual es correcto (esas
  regiones se acceden por `memremap`, no son RAM del sistema).

## 2. Flujo de arranque

```
bootrom ─ carga idblock: ddrbin → Hpmcu(rtthread.bin→0x40000, FLAG=0x10007) → SPL
SPL     ─ carga uboot.img (FIT):
          · mcu0 = rtthread.bin → spl_fit_standalone_release("mcu0", 0x40000):
              cache GRF (0xff04002c=0x00080008, ventana peri) →
              hold reset SCR1 (CORECRU 0xa04=0x1e001e) →
              SGRF HPMCU_BOOT_ADDR (0xff076044)=0x40000 →
              RELEASE (0xa04=0x1e0000)            ← el MCU arranca AQUÍ
          · uboot → 0x200000, fdt
U-Boot  ─ boot_fit: descomprime kernel gzip → 0x208000, fdt → 0xc00000, bootm
Kernel  ─ head.S (PHYS_OFFSET=0) → paging_init → ... → shell
```

- **`mcu0`, no `mcu1`**: en `src/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c`
  `spl_fit_standalone_release()` solo el id `"mcu0"` ejecuta la liberación;
  `"mcu1"` únicamente fija la dirección (flujos tipo WAKEUP donde libera el
  kernel después). Se configura en `src/rkbin/RKTRUST/RV1106TOS_TB.ini`.
- **El wrap de Rockchip (`rv1106_hpmcu_wrap`) quedó eliminado del flujo**: es un
  intérprete de comandos por mailbox (espera órdenes de `rk_meta_process()` del
  SPL prebuilt de rkbin); con nuestro SPL de fuente jamás salta a `0x40000`.
  Liberar el SCR1 directo a `0x40000` es comportamiento probado del silicio (el
  flujo WAKEUP de Rockchip hace exactamente eso).

## 3. La cadena de cinco bugs (histórico y por qué de cada uno)

1. **Kernel en `0x8000` pisaba al MCU** (`zImage` con AUTO_ZRELADDR se
   auto-descomprime a `0x8000` fijo). *Solución:* FIT con `Image` cruda comprimida
   gzip y `load/entry=0x208000` (`src/kernel/boot.its`); `mkimg` comprime el
   Image él mismo (el target `Image.gz` de make **corre en paralelo** con
   `zImage` bajo `-j` y puede salir vacío); `kernel_addr_r=0x208000`
   (`rv1106_common.h`); `CONFIG_GZIP=y` en U-Boot.
2. **TEXT_OFFSET**: `textofs-$(CONFIG_CPU_RV1106) := 0x208000` estaba dentro de
   `ifeq (CONFIG_ROCKCHIP_THUNDER_BOOT,y)` en `arch/arm/Makefile` → kernel
   enlazado para `0x8000` pero cargado en `0x208000` → PHYS_OFFSET=0x200000 no
   alineado a 16MiB → `__fixup_pv_table` → `bne __error` **antes de earlycon**.
   *Solución:* la línea del RV1106 quedó fuera del ifeq.
3. **`no-map` en `rtos@40000` borraba toda la RAM**: el hueco NOMAP hace que el
   primer bloque usable termine en `0x40000` (no alineado a PMD/2MB) →
   `adjust_lowmem_bounds()` fija `memblock_limit=0x40000` → `round_down` → 0 →
   sin HIGHMEM entra "Ignoring RAM" → `memblock_remove(0, 256MB)` → panic de
   `early_alloc` en `paging_init`. *Solución:* `rtos@40000` y `ramoops@7c000` son
   **reservas simples** (estilo Luckfox `rv1106-thunder-boot.dtsi`), y el DTS
   lleva nodo `/memory` explícito. *Nota:* este bug estaba latente — con el
   kernel en `0x8000` la reserva chocaba con la del kernel (`-EBUSY`) y nunca
   aplicaba (que es exactamente por lo que Linux podía pisar al MCU).
4. **El SCR1 nunca se liberaba** (wrap como `mcu1`; ver §2).
5. **SystemInit del MCU se cuelga inicializando el DCACHE** (`0xff640000`,
   sondeo infinito de `CACHE_INIT_FINISH`; el A7 recibe bus error al leer ese
   bloque). *Solución provisional:* `# CONFIG_RT_USING_CACHE is not set` en
   `board/pwncube/defconfig` — el MCU corre sin caché, lo que además garantiza
   coherencia del shared-memory de IPC (ver
  [70-mailbox-loopback-test.md](70-mailbox-loopback-test.md)). *Pendiente:* portar la
   secuencia de init del wrap (está desensamblada: `CTRL|=3`, `&=~8`, poll
   `+0x30` bit0, `&=~0x40`, `|=0x781`, `+0x0c=1`, poll, `&=~0x40`) o encontrar
   el clock/GRF que habilita el bloque.

## 4. Flasheo (procedimiento correcto)

```sh
# SIEMPRE la imagen completa; DI -b dice "ok" pero NO escribe en esta SPI-NAND
./scripts/02-build-kernel.sh          # si cambió kernel/dts
bash scripts/06-build-mcu.sh          # si cambió el firmware MCU
cp -f output/mcu/rtthread.bin src/rkbin/bin/rv11/rtthread.bin   # tras 06
./scripts/01-build-uboot.sh           # si cambió rtthread/uboot/inis (re-empaca idblock+uboot.img)
./scripts/04-pack-image.sh            # SIEMPRE antes de flashear
tools/upgrade_tool UF output/images/update.img
```

Para entrar a maskrom sin tocar la placa: `reboot loader` desde el shell de
Linux. Con la placa colgada: botón de recovery mantenido al energizar (o corto
CLK-GND del SPI-NAND) — **soltar el botón en cuanto empiece el flasheo**, o el
reboot post-UF se queda mudo en modo RKUART del bootrom.

## 5. Diagnóstico en runtime (desde Linux)

| Lectura | Valor sano | Significado |
|---|---|---|
| `devmem 0xff6ff900` | `0xB000xxxx` **creciente** | Heartbeat del MCU (main loop vivo) |
| `devmem 0xff6ff800` | `0xCAFE0001` | `main()` del MCU alcanzado |
| `devmem 0xff076044` | `0x00040000` | Boot addr del SCR1 |
| `devmem 0xff3b8a04` | `0x00000000` | Resets del SCR1 liberados |
| `devmem 0x40000` | `0x0000A401` | rtthread.bin intacto en su sitio |
| `head -3 /proc/iomem` | `Kernel code 0x208000-...` | Kernel donde debe |

Marcadores de etapa del arranque del MCU (TEMP, ver §6): `0xff5c0030=0xCAFE0005`
(_start), `0xff6ff810/814` (stack/data), `0xff6ff81c=0xCAFE0009` (SystemInit
OK), `0xff6ff820..830=0xCAFE000A..000E` (etapas de `rt_hw_board_init`). El
primer marcador con basura indica la etapa exacta del fallo.

## 6. Instrumentación temporal a retirar cuando el sistema se declare estable

- **Kernel**: `earlyprintk`+`earlycon` en bootargs (`dts/rv1106g-sdk.dts`);
  bloque `CONFIG_DEBUG_LL*`/`CONFIG_EARLY_PRINTK` del defconfig; centinelas
  `'S'`/`'P'` en `arch/arm/kernel/head.S`, `'M'` en `head-common.S`,
  `printascii` en `init/main.c`; volcado de memblock en
  `arch/arm/mm/mmu.c:early_alloc()`.
- **MCU**: marcadores CAFE en `start_rv1106_mcu.S` y
  `board/common/board_base.c`; los de `main.c`/`ping_echo.c`.
- **U-Boot**: el write de `MCU_CACHE_MISC` en `arch_cpu_init()` (redundante con
  el del branch mcu0; inofensivo).
- Reactivar `RT_USING_CACHE` cuando se resuelva el punto 5 de §3 (evaluando el
  impacto en la coherencia del IPC por shared-memory).
