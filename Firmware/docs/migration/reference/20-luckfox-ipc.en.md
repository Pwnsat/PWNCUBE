# Luckfox Reference — IPC ARM (A7) ↔ RISC-V (HPMCU)

> **Nature of the document.** Study of the **Luckfox Pico SDK** (external reference) as a
> technical reference. Paths relative to the SDK. It locates and documents the real
> A7↔RISC-V communication, citing files.

## 0. Summary and status for the RV1106

The SDK implements IPC over three layers: **RPMsg-Lite** (virtio-style messaging) →
**Mailbox** (HW signaling) → **Shared Memory / vrings** (transport in DDR).
The real wiring shipped in the SDK is for RK3568; the concrete integration for the
RV1106 **is now done and running on hardware** (see §8). One RV1106-specific quirk
was found while bringing it up: **B2A (MCU→A7) works over IRQ, but A2B (A7→MCU) is not
readable by the MCU via IRQ, so the MCU polls the vrings** (see
[`../../architecture/ipc-rpmsg.md`](../../architecture/ipc-rpmsg.md)).

```
 Linux (A7)                                  RT-Thread (RISC-V/HPMCU)
 ┌───────────────────────┐                   ┌────────────────────────────┐
 │ app  rpmsg_send/sendto │                   │ app  rpmsg_lite_send       │
 ├───────────────────────┤                   ├────────────────────────────┤
 │ rockchip_rpmsg.c       │   vrings (DDR)    │ rpmsg-lite + rpmsg_cmd     │
 │ (virtio + endpoints)   │ <===============> │ (endpoints + cmd table)    │
 ├───────────────────────┤  buffers 496 B    ├────────────────────────────┤
 │ rockchip-mailbox.c     │   A2B / B2A IRQ   │ rpmsg_platform.c + HAL_MBOX│
 └─────────┬─────────────┘ <===============> └─────────────┬──────────────┘
           └──────────── MAILBOX0 @ 0xff5c0000 ────────────┘
```

## 1. RPMsg layer

### 1.1 Linux side (A7) — `rockchip_rpmsg.c`
`sysdrv/source/kernel/drivers/rpmsg/rockchip_rpmsg.c`

- virtio device + mailbox channels: `struct rk_rpmsg_dev` (`:29-57`).
- `virtio_config_ops` (find/del vqueues): `:252-260`.
- RX from mailbox B2A → `rk_rpmsg_rx_callback` → `vring_interrupt(0, vq[0])` (`:64-84`).
- Notify (A2B): `rk_rpmsg_notify()` builds `{cmd = link_id&0xFF, data = RPMSG_MBOX_MAGIC}` (`:86-120`,`:101-102`).
- Vrings: 2 per instance, `0x8000` each (`:141-148`).
- Probe: mailbox RX/TX channels, `link_id` from properties (`:331`),
  `of_reserved_mem_device_init()` (`:353`); see `:298-390`.
- **Compatibles table:** only `rockchip,rk3562-rpmsg` and `rockchip,rk3568-rpmsg`
  (`:402-406`). **`rv1106` is NOT there** → it has to be added (§8).

### 1.2 RT-Thread side (RISC-V) — RPMsg-Lite
`sysdrv/source/mcu/rt-thread/bsp/rockchip/common/drivers/rpmsg-lite/`

- API (`lib/include/rpmsg_lite.h`): `rpmsg_lite_create_ept(inst, addr, rx_cb, data)`,
  `rpmsg_lite_send(inst, ept, dst, data, len, timeout)`.
- Endpoint (`rpmsg_lite.h:100-114`): `{ uint32_t addr; rl_ept_rx_cb_t rx_cb; void *rx_cb_data; }`.
- Message header (`rpmsg_lite.h:55-62`): `{ src, dst, reserved, uint16_t len, uint16_t flags }`.
- Constants: `RL_VERSION="4.0.0"`; payload 496 B; 64 buffers/address; vring `0x8000`.

### 1.3 Command layer — `rpmsg_cmd`
`sysdrv/source/mcu/rt-thread/components/rpmsg_cmd/rpmsg_cmd.h`

- Endpoint handle (`:27-41`): instance + ept + `cmd_table` + thread + `rt_messagequeue`.
- Command header (`:43-58`): `{ uint32_t type; uint32_t cmd; void *priv; void *addr; }`.
- Types (`:64-66`): `RPMSG_TYPE_DIRECT=1` (callback in ISR), `URGENT=2`, `NORMAL=3` (queue).
- Endpoint conversion (`:69-70`): `EPT_M2R_ADDR()`, `EPT_R2M_ADDR()`.
- Init (`rpmsg_cmd_remote.c:47-55`): `rpmsg_cmd_ept_init(handle, MASTER_ID, REMOTE_ID, EPT, table, n, stack=2048, prio)`.

> **`rpmsg_cmd` is the pattern to copy for the CubeSat:** a *command table*
> `(cmd → handler)` per endpoint, dispatched by an RT-Thread thread. It maps
> directly to the IPC protocol proposed in doc 50.

## 2. Mailbox (HW signaling)

**RV1106 base:** `MAILBOX0 @ 0xff5c0000` (`#mbox-cells=<1>`, `status="okay"`);
there is also `pmu_mailbox@0xff378000` (`disabled`). Device tree:
`sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi` (`mailbox` nodes).

### 2.1 Linux driver — `rockchip-mailbox.c`
`sysdrv/source/kernel/drivers/mailbox/rockchip-mailbox.c`

Registers (`:17-25`): `A2B_INTEN=0x00`, `A2B_STATUS=0x04`, `A2B_CMD(x)=0x08+8x`,
`A2B_DAT(x)=0x0c+8x`; `B2A_INTEN=0x28`, `B2A_STATUS=0x2C`, `B2A_CMD(x)=0x30+8x`,
`B2A_DAT(x)=0x34+8x`. Message `{u32 cmd; u32 data}`
(`include/soc/rockchip/rockchip-mailbox.h:14-17`). Send `:46-70`; IRQ `:127-156`
(reads B2A, `mbox_chan_received_data()`, clears status).

### 2.2 RT-Thread platform — `rpmsg_platform.c`
`.../rpmsg-lite/lib/rpmsg_lite/porting/platform/RK3568/rpmsg_platform.c`

- Mailbox ISR: `rpmsg_mbox_isr()` → `HAL_MBOX_IrqHandler()` (`:110-114`).
- Master callback: validates `RL_RPMSG_MAGIC`, `env_isr(RL_GET_VQ_ID(link_id,0))` (`:116-125`).
- Notify: `platform_notify()` builds `{CMD=link_id&0xFF, DATA=RL_RPMSG_MAGIC}` and
  `HAL_MBOX_SendMsg()` (`:284-310`).
- Mailbox M/R clients per channel (`:165-179`); IRQ mapping M=B2A, R=A2B (`:147-163`).

## 3. Shared Memory / vrings

`sysdrv/source/kernel/include/linux/rpmsg/rockchip_rpmsg.h:19-41`:

```c
#define RPMSG_BUF_PAYLOAD_SIZE  (496UL)        // useful data per message
#define RPMSG_BUF_SIZE          (512UL)        // 496 + 16 header
#define RPMSG_BUF_COUNT         (64UL)         // buffers per address
#define RPMSG_VRING_ALIGN       (0x1000UL)     // 4 KB
#define RPMSG_VRING_SIZE        (0x8000UL)     // 32 KB per vring
#define RPMSG_VRING_OVERHEAD    (0x10000UL)    // 2 vrings/instance (64 KB)
#define RPMSG_MAX_INSTANCE_NUM  (12U)
#define RPMSG_MBOX_MAGIC        (0x524D5347U)  // "RMSG"
#define RPMSG_GET_M_CPU_ID(id)  (((id)&0xF0)>>4)
#define RPMSG_GET_R_CPU_ID(id)  ((id)&0xF)
```

Each instance = 2 vrings × 32 KB = 64 KB in DDR. `vring_new_virtqueue(index, 64,
0x1000, vdev, true, ctx, addr, rk_rpmsg_notify, cb, name)` (`rockchip_rpmsg.c:150-152`).
The physical region is provided by `platform_get_resource()` / `reserved-memory` of the DT
(`:262-296`). **Note:** the 64 KB of vrings go in **reserved DDR**, distinct from the
8 KB `hpmcu_sram` window (doc 10 §4).

## 4. Interrupts

`.../common/hal/lib/CMSIS/Device/RV1106/Include/soc.h`: on the MCU
`MAILBOX0_AP_IRQn=1`, `MAILBOX0_BB_IRQn=2` (`:53-101`); on the A7
`MAILBOX0_AP_IRQn=33`, `MAILBOX0_BB_IRQn=34` (`:102-172`). The Linux node declares
`interrupts = <GIC_SPI 1 IRQ_TYPE_LEVEL_HIGH>` for `mailbox@ff5c0000`.

Platform config (RK3568 template):
`.../rpmsg-lite/lib/include/platform/RK3568/rpmsg_config.h:24-30`
(`RL_PLATFORM_USING_MBOX`, B2A/A2B bases, `RL_RPMSG_MAGIC=0x524D5347`).
In the SDK porting files the RV1106-specific IRQ numbers are not defined (they inherit
the RK3568 ones). The CubeSat firmware now sets the real numbers from the RV1106 `soc.h`
(§8). Measured on hardware: the **B2A** IRQ reaches the MCU, but the **A2B** IRQ does
**not**, so the MCU polls the vrings instead of relying on the A2B interrupt.

## 5. Link/queue encoding

`.../rpmsg-lite/.../platform/RK3568/rpmsg_platform.h:29-30`:
`RL_GET_VQ_ID(link_id, q) = (q&1) | ((link_id<<1)&~1)`,
`RL_GET_LINK_ID(id) = (id&~1)>>1`, `RL_GET_Q_ID(id) = id&1`. `link_id` packs
M-CPU (high nibble) and R-CPU (low nibble): `0x01` ⇒ master 0 ↔ remote 1.

## 6. End-to-end flow

**A7 → RISC-V:** app `rpmsg_sendto()` → `rk_rpmsg_notify()` enqueues in the TX vring and
`mbox_send_message()` writes A2B → IRQ on RISC-V → `rpmsg_mbox_isr` →
`env_isr(vq)` → endpoint callback.
**RISC-V → A7:** `rpmsg_lite_send()` takes a buffer from the pool, builds `rpmsg_std_hdr`,
enqueues in the vring → `platform_notify()` writes B2A → GIC IRQ on Linux →
`rockchip_mbox_irq` → `rk_rpmsg_rx_callback` → `vring_interrupt` → ept callback.

> **RV1106 deviation (measured).** The A7→RISC-V direction above assumes the A2B IRQ
> reaches the MCU; on the RV1106 it does not. The CubeSat firmware therefore **polls the
> vrings** on the MCU side to pick up A7→MCU messages, while B2A (MCU→A7) uses the IRQ as
> described. See [`../../architecture/ipc-rpmsg.md`](../../architecture/ipc-rpmsg.md).

## 7. Examples in the SDK

- Kernel test: `sysdrv/source/kernel/drivers/rpmsg/rockchip_rpmsg_test.c`
  (probe `:47-79` sends `rpmsg_send/sendto`; callback `:23-45` replies to the `src`).
- Media app: `project/app/rkipc/rkipc/src/rv1106_wakeup_ipc/` (uses IPC under the
  ISP pipeline; does not show direct use of endpoints in `main.c`).
- MCU shmem test: `.../common/tests/shmem_ipc_test/task_ipc_test.c` (critical section
  with `HAL_SPINLOCK_Lock/Unlock`).

## 8. What was added for the RV1106 (wiring work — DONE)

A functional A7↔RISC-V channel in the CubeSat required **adding**, not just recompiling.
All five points below are **implemented and running on hardware**:

1. **Kernel:** `"rockchip,rv1106-rpmsg"` added to the match table of
   `rockchip_rpmsg.c` (the `rk3568` code path is binary-compatible).
2. **Device tree (Linux):** `rpmsg` node linked to `mailbox@ff5c0000` + a `reserved-memory`
   region for the vrings (64 KB/instance) — added to the RV1106 DTS.
3. **RPMsg-Lite porting (RISC-V):** RV1106 porting derived from `RK3568`, with the correct
   IRQ bases and vring addresses of the RV1106.
4. **RV1106 IRQ:** the real `MBOX0_CHn_{A2B,B2A}_IRQn` are set from the RV1106 `soc.h`.
   Note the measured deviation (§4, §6): the **B2A** IRQ reaches the MCU, but the **A2B**
   IRQ does not, so the MCU **polls the vrings** for A7→MCU traffic.
5. **Physical address of vrings:** defined consistently on both sides (DT
   reserved-memory ↔ MCU porting).

This bring-up is documented step by step in
[`../implementation/60-ipc-bringup.md`](../implementation/60-ipc-bringup.md) and
[`../implementation/70-mailbox-loopback-test.md`](../implementation/70-mailbox-loopback-test.md).
On top of it the CubeSat runs `rpmsg_cmd`-style command tables (§1.3) as these live
services / endpoints: **PingEcho** `rpmsg-ap3-ch0` 0x4004, **RadioService** `rpmsg-radio`
0x4005, **SensorService** `rpmsg-sensor` 0x4006, **TelemetryService** `rpmsg-telemetry`
0x4007 and **CommandService** `rpmsg-command` 0x4008.
