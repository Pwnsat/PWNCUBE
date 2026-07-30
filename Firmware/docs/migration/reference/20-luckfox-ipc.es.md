# Referencia Luckfox — IPC ARM (A7) ↔ RISC-V (HPMCU)

> **Naturaleza del documento.** Estudio del **SDK Luckfox Pico** (referencia externa) como
> referencia técnica. Rutas relativas al SDK. Localiza y documenta la comunicación
> A7↔RISC-V real, citando archivos.

## 0. Resumen y estado para el RV1106

El SDK implementa IPC sobre tres capas: **RPMsg-Lite** (mensajería tipo virtio) →
**Mailbox** (señalización HW) → **Shared Memory / vrings** (transporte en DDR).
El cableado real que trae el SDK es para RK3568; la integración concreta para el
RV1106 **ya está hecha y corriendo en hardware** (ver §8). Al levantarla se encontró
una peculiaridad del RV1106: **B2A (MCU→A7) funciona por IRQ, pero A2B (A7→MCU) no es
legible por el MCU vía IRQ, así que el MCU hace poll de los vrings** (ver
[`../../architecture/ipc-rpmsg.md`](../../architecture/ipc-rpmsg.md)).

```
 Linux (A7)                                  RT-Thread (RISC-V/HPMCU)
 ┌───────────────────────┐                   ┌────────────────────────────┐
 │ app  rpmsg_send/sendto │                   │ app  rpmsg_lite_send       │
 ├───────────────────────┤                   ├────────────────────────────┤
 │ rockchip_rpmsg.c       │   vrings (DDR)    │ rpmsg-lite + rpmsg_cmd     │
 │ (virtio + endpoints)   │ <===============> │ (endpoints + tabla de cmd) │
 ├───────────────────────┤  buffers 496 B    ├────────────────────────────┤
 │ rockchip-mailbox.c     │   A2B / B2A IRQ   │ rpmsg_platform.c + HAL_MBOX│
 └─────────┬─────────────┘ <===============> └─────────────┬──────────────┘
           └──────────── MAILBOX0 @ 0xff5c0000 ────────────┘
```

## 1. Capa RPMsg

### 1.1 Lado Linux (A7) — `rockchip_rpmsg.c`
`sysdrv/source/kernel/drivers/rpmsg/rockchip_rpmsg.c`

- Dispositivo virtio + canales mailbox: `struct rk_rpmsg_dev` (`:29-57`).
- `virtio_config_ops` (find/del vqueues): `:252-260`.
- RX desde mailbox B2A → `rk_rpmsg_rx_callback` → `vring_interrupt(0, vq[0])` (`:64-84`).
- Notificar (A2B): `rk_rpmsg_notify()` arma `{cmd = link_id&0xFF, data = RPMSG_MBOX_MAGIC}` (`:86-120`,`:101-102`).
- Vrings: 2 por instancia, `0x8000` cada una (`:141-148`).
- Probe: canales mailbox RX/TX, `link_id` de propiedades (`:331`),
  `of_reserved_mem_device_init()` (`:353`); ver `:298-390`.
- **Tabla de compatibles:** solo `rockchip,rk3562-rpmsg` y `rockchip,rk3568-rpmsg`
  (`:402-406`). **`rv1106` NO está** → hay que añadirlo (§8).

### 1.2 Lado RT-Thread (RISC-V) — RPMsg-Lite
`sysdrv/source/mcu/rt-thread/bsp/rockchip/common/drivers/rpmsg-lite/`

- API (`lib/include/rpmsg_lite.h`): `rpmsg_lite_create_ept(inst, addr, rx_cb, data)`,
  `rpmsg_lite_send(inst, ept, dst, data, len, timeout)`.
- Endpoint (`rpmsg_lite.h:100-114`): `{ uint32_t addr; rl_ept_rx_cb_t rx_cb; void *rx_cb_data; }`.
- Cabecera de mensaje (`rpmsg_lite.h:55-62`): `{ src, dst, reserved, uint16_t len, uint16_t flags }`.
- Constantes: `RL_VERSION="4.0.0"`; payload 496 B; 64 buffers/dirección; vring `0x8000`.

### 1.3 Capa de comandos — `rpmsg_cmd`
`sysdrv/source/mcu/rt-thread/components/rpmsg_cmd/rpmsg_cmd.h`

- Handle de endpoint (`:27-41`): instancia + ept + `cmd_table` + hilo + `rt_messagequeue`.
- Cabecera de comando (`:43-58`): `{ uint32_t type; uint32_t cmd; void *priv; void *addr; }`.
- Tipos (`:64-66`): `RPMSG_TYPE_DIRECT=1` (callback en ISR), `URGENT=2`, `NORMAL=3` (cola).
- Conversión de endpoints (`:69-70`): `EPT_M2R_ADDR()`, `EPT_R2M_ADDR()`.
- Init (`rpmsg_cmd_remote.c:47-55`): `rpmsg_cmd_ept_init(handle, MASTER_ID, REMOTE_ID, EPT, tabla, n, stack=2048, prio)`.

> **`rpmsg_cmd` es el patrón a copiar para el CubeSat:** una *tabla de comandos*
> `(cmd → handler)` por endpoint, despachada por un hilo de RT-Thread. Mapea
> directamente al protocolo IPC propuesto en el doc 50.

## 2. Mailbox (señalización HW)

**Base RV1106:** `MAILBOX0 @ 0xff5c0000` (`#mbox-cells=<1>`, `status="okay"`);
existe además `pmu_mailbox@0xff378000` (`disabled`). Device tree:
`sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi` (nodos `mailbox`).

### 2.1 Driver Linux — `rockchip-mailbox.c`
`sysdrv/source/kernel/drivers/mailbox/rockchip-mailbox.c`

Registros (`:17-25`): `A2B_INTEN=0x00`, `A2B_STATUS=0x04`, `A2B_CMD(x)=0x08+8x`,
`A2B_DAT(x)=0x0c+8x`; `B2A_INTEN=0x28`, `B2A_STATUS=0x2C`, `B2A_CMD(x)=0x30+8x`,
`B2A_DAT(x)=0x34+8x`. Mensaje `{u32 cmd; u32 data}`
(`include/soc/rockchip/rockchip-mailbox.h:14-17`). Envío `:46-70`; IRQ `:127-156`
(lee B2A, `mbox_chan_received_data()`, limpia status).

### 2.2 Plataforma RT-Thread — `rpmsg_platform.c`
`.../rpmsg-lite/lib/rpmsg_lite/porting/platform/RK3568/rpmsg_platform.c`

- ISR mailbox: `rpmsg_mbox_isr()` → `HAL_MBOX_IrqHandler()` (`:110-114`).
- Callback maestro: valida `RL_RPMSG_MAGIC`, `env_isr(RL_GET_VQ_ID(link_id,0))` (`:116-125`).
- Notify: `platform_notify()` arma `{CMD=link_id&0xFF, DATA=RL_RPMSG_MAGIC}` y
  `HAL_MBOX_SendMsg()` (`:284-310`).
- Clientes mailbox M/R por canal (`:165-179`); mapeo IRQ M=B2A, R=A2B (`:147-163`).

## 3. Shared Memory / vrings

`sysdrv/source/kernel/include/linux/rpmsg/rockchip_rpmsg.h:19-41`:

```c
#define RPMSG_BUF_PAYLOAD_SIZE  (496UL)        // datos útiles por mensaje
#define RPMSG_BUF_SIZE          (512UL)        // 496 + 16 de cabecera
#define RPMSG_BUF_COUNT         (64UL)         // buffers por dirección
#define RPMSG_VRING_ALIGN       (0x1000UL)     // 4 KB
#define RPMSG_VRING_SIZE        (0x8000UL)     // 32 KB por vring
#define RPMSG_VRING_OVERHEAD    (0x10000UL)    // 2 vrings/instancia (64 KB)
#define RPMSG_MAX_INSTANCE_NUM  (12U)
#define RPMSG_MBOX_MAGIC        (0x524D5347U)  // "RMSG"
#define RPMSG_GET_M_CPU_ID(id)  (((id)&0xF0)>>4)
#define RPMSG_GET_R_CPU_ID(id)  ((id)&0xF)
```

Cada instancia = 2 vrings × 32 KB = 64 KB en DDR. `vring_new_virtqueue(index, 64,
0x1000, vdev, true, ctx, addr, rk_rpmsg_notify, cb, name)` (`rockchip_rpmsg.c:150-152`).
La región física la entrega `platform_get_resource()` / `reserved-memory` del DT
(`:262-296`). **Nota:** las 64 KB de vrings van en **DDR reservada**, distinta de la
ventana de 8 KB `hpmcu_sram` (doc 10 §4).

## 4. Interrupciones

`.../common/hal/lib/CMSIS/Device/RV1106/Include/soc.h`: en el MCU
`MAILBOX0_AP_IRQn=1`, `MAILBOX0_BB_IRQn=2` (`:53-101`); en el A7
`MAILBOX0_AP_IRQn=33`, `MAILBOX0_BB_IRQn=34` (`:102-172`). El nodo Linux declara
`interrupts = <GIC_SPI 1 IRQ_TYPE_LEVEL_HIGH>` para `mailbox@ff5c0000`.

Config de plataforma (plantilla RK3568):
`.../rpmsg-lite/lib/include/platform/RK3568/rpmsg_config.h:24-30`
(`RL_PLATFORM_USING_MBOX`, bases B2A/A2B, `RL_RPMSG_MAGIC=0x524D5347`).
En los archivos de porting del SDK los números de IRQ específicos del RV1106 no están
definidos (heredan los de RK3568). El firmware del CubeSat ya fija los reales desde el
`soc.h` del RV1106 (§8). Medido en hardware: la IRQ **B2A** llega al MCU, pero la **A2B**
**no**, así que el MCU hace poll de los vrings en vez de depender de la interrupción A2B.

## 5. Codificación de link/queue

`.../rpmsg-lite/.../platform/RK3568/rpmsg_platform.h:29-30`:
`RL_GET_VQ_ID(link_id, q) = (q&1) | ((link_id<<1)&~1)`,
`RL_GET_LINK_ID(id) = (id&~1)>>1`, `RL_GET_Q_ID(id) = id&1`. `link_id` empaqueta
M-CPU (nibble alto) y R-CPU (nibble bajo): `0x01` ⇒ master 0 ↔ remote 1.

## 6. Flujo extremo a extremo

**A7 → RISC-V:** app `rpmsg_sendto()` → `rk_rpmsg_notify()` encola en vring TX y
`mbox_send_message()` escribe A2B → IRQ en RISC-V → `rpmsg_mbox_isr` →
`env_isr(vq)` → callback del endpoint.
**RISC-V → A7:** `rpmsg_lite_send()` toma buffer del pool, arma `rpmsg_std_hdr`,
encola en vring → `platform_notify()` escribe B2A → IRQ GIC en Linux →
`rockchip_mbox_irq` → `rk_rpmsg_rx_callback` → `vring_interrupt` → callback del ept.

> **Desviación del RV1106 (medida).** La dirección A7→RISC-V de arriba asume que la IRQ
> A2B llega al MCU; en el RV1106 no llega. El firmware del CubeSat por tanto **hace poll
> de los vrings** en el lado MCU para recoger los mensajes A7→MCU, mientras que B2A
> (MCU→A7) sí usa la IRQ como se describe. Ver
> [`../../architecture/ipc-rpmsg.md`](../../architecture/ipc-rpmsg.md).

## 7. Ejemplos en el SDK

- Test kernel: `sysdrv/source/kernel/drivers/rpmsg/rockchip_rpmsg_test.c`
  (probe `:47-79` envía `rpmsg_send/sendto`; callback `:23-45` responde al `src`).
- App media: `project/app/rkipc/rkipc/src/rv1106_wakeup_ipc/` (usa IPC bajo el
  pipeline ISP; no muestra uso directo de endpoints en `main.c`).
- Test shmem MCU: `.../common/tests/shmem_ipc_test/task_ipc_test.c` (sección crítica
  con `HAL_SPINLOCK_Lock/Unlock`).

## 8. Lo que se añadió para el RV1106 (trabajo de cableado — HECHO)

Un canal A7↔RISC-V funcional en el CubeSat exigió **añadir**, no solo recompilar. Los
cinco puntos de abajo están **implementados y corriendo en hardware**:

1. **Kernel:** `"rockchip,rv1106-rpmsg"` añadido a la tabla de match de
   `rockchip_rpmsg.c` (la ruta de código `rk3568` es compatible binario).
2. **Device tree (Linux):** nodo `rpmsg` enlazado a `mailbox@ff5c0000` + región
   `reserved-memory` para los vrings (64 KB/instancia) — añadido a los DTS del RV1106.
3. **Porting RPMsg-Lite (RISC-V):** porting del RV1106 derivado de `RK3568`, con las
   bases de IRQ y direcciones de vring correctas del RV1106.
4. **IRQ del RV1106:** los `MBOX0_CHn_{A2B,B2A}_IRQn` reales se fijan desde el `soc.h`
   del RV1106. Nótese la desviación medida (§4, §6): la IRQ **B2A** llega al MCU, pero la
   **A2B** no, así que el MCU **hace poll de los vrings** para el tráfico A7→MCU.
5. **Dirección física de vrings:** definida coherentemente en ambos lados (DT
   reserved-memory ↔ porting del MCU).

Este bring-up está documentado paso a paso en
[`../implementation/60-ipc-bringup.md`](../implementation/60-ipc-bringup.md) y
[`../implementation/70-mailbox-loopback-test.md`](../implementation/70-mailbox-loopback-test.md).
Encima de él, el CubeSat corre tablas de comandos estilo `rpmsg_cmd` (§1.3) como estos
servicios / endpoints vivos: **PingEcho** `rpmsg-ap3-ch0` 0x4004, **RadioService**
`rpmsg-radio` 0x4005, **SensorService** `rpmsg-sensor` 0x4006, **TelemetryService**
`rpmsg-telemetry` 0x4007 y **CommandService** `rpmsg-command` 0x4008.
