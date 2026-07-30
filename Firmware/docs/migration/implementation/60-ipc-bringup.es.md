# Bring-up del transporte IPC (estado y receta)

> Registro de implementación del **transporte RPMsg A7↔RISC-V** — ya **operativo en placa**.
> El transporte lleva **cinco servicios rpmsg en producción** (PingEcho, Radio, Sensor,
> Telemetry, Command), cada uno en su endpoint sobre una única instancia rpmsg-lite compartida.
> Este documento conserva la receta de bring-up y las decisiones de diseño que llevaron ahí;
> para la foto asentada ver los docs de arquitectura
> [../../architecture/ipc-rpmsg.md](../../architecture/ipc-rpmsg.md) y
> [../../architecture/overview.md](../../architecture/overview.md).
> Decisiones tomadas: ceder buses al RISC-V; porting `platform/RV1106/` propio;
> MCU sin consola (verificación por IPC).

## 1. Hecho y verificado (corriendo en placa)

**Base de firmware MCU**:
- Árbol RT-Thread vendorizado en `src/mcu/`; toolchain `toolchain/riscv/` (`riscv-none-embed-gcc 10.2.0`).
- Board mínima `src/mcu/.../rv1106-mcu/board/pwncube/` (sin cámara/ISP; **sin uart2** — Linux posee la consola `ttyFIQ0`).
- `scripts/06-build-mcu.sh` + `./build.sh mcu` → `output/mcu/rtthread.bin`.

**Porting rpmsg-lite RISC-V** (la pieza más difícil — hecha y corriendo):
- `src/mcu/.../rpmsg-lite/lib/include/platform/RV1106/rpmsg_config.h` — IRQ de mailbox RV1106 (un solo par AP/BB, no 8 por canal).
- `.../include/platform/RV1106/rpmsg_platform.h` — copia agnóstica de RK3568.
- `.../rpmsg_lite/porting/platform/RV1106/rpmsg_platform.c` — **reescrito para RISC-V**:
  sin SMP/afinidad/`HAL_CPU_TOPOLOGY`, `MBOX0`→`MBOX`, IRQ único `MAILBOX0_BB_IRQn`,
  `cpsid/cpsie`→`rt_hw_interrupt_disable/enable`, `platform_in_isr`→`rt_interrupt_get_nest`.
- Cambio de portabilidad: `.../rpmsg-lite/lib/include/rpmsg_compiler.h` — `MEM_BARRIER()` usa
  `fence` (RISC-V) en vez de `dsb` (ARM) bajo `#if defined(__riscv)`.
- Selección de build: `.../rpmsg-lite/SConscript` y `.../common/drivers/Kconfig`
  (`RT_USING_RPMSG_LITE` `depends on ... || SOC_RV1106`).

> **`RT_USING_RPMSG_LITE` está ahora ACTIVADO** en `board/pwncube/defconfig`: el enlace y el
> comportamiento en placa están validados y los cinco servicios rpmsg corren contra Linux.

**Datos de la placa (leídos por serie, no destructivo):**
- DDR = **256 MB** en `0x00000000–0x0fffffff` (todo es System RAM; la región compartida se recorta vía `reserved-memory`).
- Consola Linux = `ttyFIQ0` (uart2). `/dev/ttyUSB0` @115200 da shell root. sudo `1334`.

## 2. Mapa de memoria compartida (fijado, en uso)

**1 MB en el tope de la DDR** aloja los vrings RPMsg, idéntico en ambos lados:

| Símbolo | Valor | Notas |
|---------|-----------------|-------|
| Base región compartida | `0x0FF00000` | Tope de 256 MB. **Verificado: sin colisión con OP-TEE/trust.** |
| Tamaño | `0x00100000` (1 MB) | vrings `0x0ff00000–0x0ff7ffff` + pool DMA `0x0ff80000–0x0fffffff`. Una instancia rpmsg lleva los cinco endpoints de servicio. |

- **Linux:** nodo `reserved-memory` `no-map` en `0x0FF00000` + `reg` del nodo `rpmsg`.
- **MCU:** símbolos de linker `__linux_share_rpmsg_start__/__end__` en `src/mcu/.../rv1106-mcu/link.lds`
  apuntando a la **misma** dirección (NO la SRAM 0x40000; es DDR).
- `VRING_ALIGN=0x1000` obligatorio (requisito de Linux).

## 3. Parámetros que coinciden en ambos lados (resuelto)

| Parámetro | Valor | Notas |
|-----------|-----------|---------------------|
| `link-id` | `0x04` (kernel) ↔ encoding MCU | **Resuelto:** reconciliado a un canal MCU válido (0–3); el enlace levanta en ambos lados. |
| Canal mailbox | rx=ch0 / tx=ch3 (kernel) ↔ `RL_RV1106_MBOX_CHAN=0` (MCU) | **Resuelto:** los canales por-dirección del kernel mapean al único canal bidireccional del MCU (A2B/B2A). |
| Dirección A2B/B2A | MCU = remoto (B2A envía, recibe A2B) | **B2A (MCU→A7) funciona.** **A2B (A7→MCU) el MCU NO puede leerlo por IRQ** → el MCU **sondea los vrings**; el ACK del mailbox es "ciego". |
| Nombres de canal NS | ver abajo | Cinco endpoints anunciados por el MCU (era uno en el bring-up). |
| `RL_RPMSG_MAGIC` | `0x524D5347` ("RMSG") | Igual en ambos. ✔ |

**Servicios / endpoints vivos** (payload rpmsg-lite **≤ 496 B**, mensajes mayores se fragmentan):

| Servicio | Nombre NS | Endpoint | Cliente Linux |
|---------|---------|----------|--------------|
| PingEcho | `rpmsg-ap3-ch0` | `0x4004` | — |
| RadioService | `rpmsg-radio` | `0x4005` | `radio_test` |
| SensorService | `rpmsg-sensor` | `0x4006` | `sensor_test` |
| TelemetryService | `rpmsg-telemetry` | `0x4007` | `radio_test tlm` |
| CommandService | `rpmsg-command` | `0x4008` | `radio_test cmd_* / tcsend` |

## 4. Hecho — lado Linux (aplicado)

1. **Kernel match** `src/kernel/drivers/rpmsg/rockchip_rpmsg.c`: `RV1106` añadido al enum
   (`:29-32`) y `{ .compatible = "rockchip,rv1106-rpmsg", .data = (void *)RV1106 }` en la
   tabla (`:402-406`). (El campo `chip` es cosmético; sin datos por-chip.)
2. **Device tree** (en `dts/`, con el **doble include** de `rv1106-amp.dtsi`
   vía `rv1106-evb.dtsi` *y* `rv1106-sdk-ipc.dtsi` resuelto a un único include):
   - nodo `rpmsg@0ff00000` (`compatible="rockchip,rv1106-rpmsg"`, `mbox-names="rpmsg-rx","rpmsg-tx"`,
     `mboxes=<&mailbox 0 &mailbox 3>`, `rockchip,link-id`, `rockchip,vdev-nums=<1>`,
     `reg=<0x0ff00000 0x20000>`, `memory-region=<&rpmsg_dma_reserved>`).
   - `reserved-memory`: `rpmsg_reserved@0ff00000` (`no-map`) + `rpmsg_dma_reserved` (`shared-dma-pool`).
   - `&mailbox { status = "okay"; };` (era `disabled` en `rv1106.dtsi`).
   - `rv1106-amp.dtsi` (clocks `CLK_CORE_MCU`/`PCLK_MAILBOX`) incluido una sola vez.
3. **defconfig** `configs/kernel/rv1106_minimal_defconfig`:
   `CONFIG_MAILBOX=y`, `CONFIG_ROCKCHIP_MBOX=y`, `CONFIG_RPMSG_ROCKCHIP=y`,
   `CONFIG_RPMSG_ROCKCHIP_TEST=y`, `CONFIG_RPMSG_VIRTIO=y` (`ROCKCHIP_AMP` ya estaba).
   Tras `make oldconfig`, `VIRTIO`/`RPMSG` quedan activos.

## 5. Hecho — lado MCU

1. **Linker** `src/mcu/.../rv1106-mcu/link.lds`:
   `__linux_share_rpmsg_start__`/`__linux_share_rpmsg_end__` en `0x0FF00000` (DDR, fuera de la RAM SRAM del MCU).
2. **Dispatcher de servicios** (Estilo B, rpmsg-lite directo — evita el inexistente `rpmsg_base.h`).
   PingEcho levanta el enlace y aloja el único hilo de poll que atiende a todos los servicios;
   el patrón se generalizó luego a los cinco endpoints del §3. Esqueleto de bring-up
   (`INIT_APP_EXPORT(ping_echo_init)`):
   ```c
   instance = rpmsg_lite_remote_init((void*)RPMSG_LINUX_MEM_BASE,
                  RL_PLATFORM_SET_LINK_ID(0 /*A7*/, R /*MCU*/), RL_NO_FLAGS);
   rpmsg_lite_wait_for_link_up(instance);
   ept = rpmsg_lite_create_ept(instance, EPT_ADDR, echo_cb, instance);
   rpmsg_ns_announce(instance, ept, "rpmsg-ap3-ch0", RL_NS_CREATE);
   /* echo_cb: rpmsg_lite_send(instance, ept, src, payload, len, RL_BLOCK); */
   ```
   Modelo: `src/mcu/.../hal/project/rk3562-mcu/src/test_demo.c:467-526`.
3. **defconfig** `board/pwncube/defconfig`: `CONFIG_RT_USING_RPMSG_LITE=y`
   (+ los símbolos del dispatcher). `HAL_MBOX_MODULE_ENABLED` está en `hal_conf.h`.

## 6. Flasheo y prueba (hecho)

1. `./build.sh mcu` → `rtthread.bin`; integrado como `LOADER2=Hpmcu` en el flujo rkbin
   (INI RV1106 del CubeSat ya soporta `Hpmcu`) y `./build.sh` + `./build.sh flash`
   (maskrom + `upgrade_tool`). El cableado de arranque definitivo está en [80-dual-boot.md](80-dual-boot.md).
2. Al arrancar, Linux expone los endpoints en `/dev/rpmsg*`; el MCU anuncia los cinco canales
   NS. El PING/ECHO de bring-up en `rpmsg-ap3-ch0` fue lo primero verificado.
3. Los parámetros de co-diseño (§3) y la semántica de IRQ/mailbox de `rpmsg_platform.c` se
   resolvieron en placa; la prueba de loopback está en [70-mailbox-loopback-test.md](70-mailbox-loopback-test.md).

## 6bis. Arranque del HPMCU — mecanismo real (hallazgos en placa)

> **⚠️ SUPERSEDED.** Esta sección es HISTÓRICA: el arranque
> definitivo (bootrom→0x40000 + FIT `mcu0` con release directo, SIN wrap ni
> trampolín) y el transporte final están en los docs **80** y **90**. Los
> hallazgos "el bootrom no funcionó" y "el A7 no puede escribir 0x40000" eran
> artefactos de los bugs de entonces (kernel@0x8000, MCU1 sin release, DCACHE).

Tras un bring-up extenso en placa se determinó el mecanismo exacto y dónde está el bloqueo:

- **Carga del firmware:** el firmware del HPMCU se ejecuta desde la **IRAM del MCU en 0x40000**
  (`link.lds` ORIGIN=0x40000). El A7 **no** puede escribir esa IRAM directamente (en el mapa del
  A7, 0x40000 es DDR/System RAM). Por eso Rockchip interpone el **`rv1106_hpmcu_wrap`** en
  `hpmcu_sram` (`0xff6fe000`, 8 KB, accesible por A7).
- **Release del núcleo:** U-Boot SPL `spl_fit_standalone_release("mcu0"/"mcu1", 0xff6fe000)` (generado
  desde el trust INI `[MCU]` puesto a `okay`) fija `SGRF_HPMCU_BOOT_ADDR` (`0xff076044`) y, para `mcu0`,
  hace deassert del reset. **Verificado en placa:** `0xff076044 = 0xff6fe000`. El núcleo arranca el wrap.
- **El wrap es un SERVIDOR DE COMANDOS POR MAILBOX (no un cargador de flash):** desensamblado
  (`rv1106_hpmcu_wrap_v1.70.bin`) — inicializa clocks/cache (`0xff6ff004`), y entra en un bucle
  (`0xff6fe494`) que **sondea `A2B_CMD(0)`/`A2B_DAT(0)` del mailbox `0xff5c0000`** y despacha por una
  tabla de 18 comandos: uno recibe `(dirección,longitud)` y **escribe datos en memoria** (carga el
  firmware a 0x40000), otro hace el salto al entry. Es decir, **el A7 debe enviarle el firmware
  comando a comando por mailbox y ordenarle saltar**.
- **BLOQUEO:** ese driver del A7 (que pilota el wrap por mailbox) **no está en el U-Boot/kernel
  open-source** del SDK; Rockchip lo conduce desde un componente cerrado en su ruta de cámara/AOV.
  Con la config TB correcta el wrap se libera (`SGRF` fijado) pero queda **esperando comandos que
  nadie envía** → nuestro rtthread nunca se carga (`mailbox A2B_INTEN`=0, heartbeat en 0xff6ff800 sin
  escribir). El lado Linux rpmsg, en cambio, **sí funciona** (`rpmsg host is online`).
- **NO HACER:** liberar el núcleo a una dirección **DDR** (p.ej. `mcu0=rtthread,0x0fe00000`) **cuelga
  el SPL y brickea** el arranque (recuperación: corto del pin CLK de la SPI-NAND → maskrom del bootrom
  → `upgrade_tool UF` con `[MCU]` disabled). La IRAM/SRAM (0x40000 / 0xff6fe000) es segura.

### Hallazgo definitivo (verificado en placa)
- **El A7 (U-Boot SPL) NO puede escribir la IRAM del HPMCU en 0x40000.** Probada la vía limpia
  `MCU0=rtthread,0x40000,okay` (FIT standalone mcu0 → `spl_fit_standalone_release` carga a 0x40000 +
  release completo del reset): **arranca Linux sin brick** (liberar a IRAM es SEGURO, a diferencia de
  DDR), `SGRF` fijado, **pero rtthread `main()` NO corre** — un heartbeat escrito en el registro
  `B2A_DAT(0)` del mailbox (`0xff5c0034`, leído con certeza por el A7) **y** en `hpmcu_sram`
  (`0xff6ff800`) queda en 0 / basura, `A2B_INTEN`=0. La carga del SPL a 0x40000 cae en **DDR del A7**
  (inútil para el MCU) y el núcleo liberado corre una IRAM vacía. **La IRAM del MCU solo la puede
  llenar el propio MCU (el wrap) o el bootrom.**
- **El driver del A7 que pilota el wrap es CERRADO** (`rockit.ko` / `mcu.S` en Luckfox): `mcu_send_message`
  escribe `A2B_CMD0=0xff5c0008`/`A2B_DAT0=0xff5c000c`; opcodes descifrados: `cmd6`=config,
  `cmd1`=(dir. física DDR del firmware), `cmd5`=start, `cmd9`=reset. El wrap **copia el firmware desde
  esa dirección DDR a la IRAM 0x40000 y arranca**.
- Nota de empaquetado: meter rtthread en el FIT de `uboot.img` requiere `CONFIG_SPL_FIT_IMAGE_KB` que
  no exceda la partición `uboot` de 256K (512 lo paddea a 512K → error de flasheo "Image larger than
  partition"; 256 sí cabe).

### El camino que se embarcó: bootrom Hpmcu (ni el wrap, ni un trampolín)
La idea del trampolín (un stub RISC-V enlazado para `0xff6fe000` que copia `rtthread.bin` a `0x40000`)
fue una de las rutas consideradas. La solución **embarcada** es más simple: el cargador `Hpmcu` del
bootrom, en el idblock, coloca `rtthread.bin` en `0x40000`, y U-Boot SPL libera el SCR1 directo ahí vía
`spl_fit_standalone_release("mcu0", 0x40000)` — sin wrap, sin protocolo de mailbox cerrado. Ver
[80-dual-boot.md](80-dual-boot.md) §2.

## 7. Riesgos resueltos (del bring-up en placa)
- Colisión de `0x0FF00000` con OP-TEE/trust: **resuelto** — verificado sin colisión (`trust.img` carga en otro sitio).
- Mapeo `link-id`↔canal↔dirección (§3): **resuelto** — el enlace levanta en ambos lados.
- Semántica de acking de IRQ en SCR1/PLIC: A2B **no** se le entrega al MCU por interrupción (el port
  SCR1 no tiene `rt_hw_interrupt_ack`) → el MCU **sondea los vrings** y el ACK del mailbox es "ciego".
  Esta es la forma aceptada del transporte, no un bug abierto.
- Coherencia de caché en la región DDR compartida: **resuelto** — el MCU corre con `RT_USING_CACHE`
  desactivado, así el shared-memory A7↔MCU es coherente (ver [80-dual-boot.md](80-dual-boot.md) §3 bug 5 y
  [70-mailbox-loopback-test.md](70-mailbox-loopback-test.md)).
