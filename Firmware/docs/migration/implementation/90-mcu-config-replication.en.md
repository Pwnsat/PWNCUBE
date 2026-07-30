# 90 — MCU Configuration (RISC-V/RT-Thread): definitive replication guide

> **Status: TWO SERVICES VALIDATED ON BOARD.**
> The MCU owns **both SX1262 radios (SPI0/SPI1)** and **both I²C0 sensors
> (BME280 + ICM-42670)**. RadioService: CW 915 MHz seen on the analyzer + LoRa
> loopback on board (radio0↔radio1, CRC ok) with packet TX/RX, continuous RX with
> re-arm and DIO1 event push per instance — §7bis. SensorService: chip-IDs
> (0x60/0x67), compensated BME280 (T/P/H) and ICM-42670 (accel 1 g on Z, gyro, temp)
> read over I²C from the MCU and exposed to Linux via rpmsg — §7ter. Both
> services coexist in the same poll thread without interfering. This document
> captures, in extreme detail, EVERYTHING needed to configure the MCU and replicate
> the pattern with new peripherals/services. It complements doc 80 (dual-boot).

---

## 1. What runs where (mental map)

```
Linux (Cortex-A7)                      RT-Thread (SCR1 "HPMCU", rv32imc)
─────────────────────────             ──────────────────────────────────
radio_test (userspace)                 RadioService  (radio_service.c)
   │ /dev/rpmsg0                          │ endpoint 0x4005 "rpmsg-radio"
virtio_rpmsg / rockchip_rpmsg          rpmsg-lite REMOTE (poll, sin IRQ RX)
   │ vrings @0xFF00000                    │
mailbox 0xff5c0000 (kicks) ─────────── ISR ack-ciego + hilo de poll
                                          │
                                       sx1262_cmd.c (COMPARTIDO con Linux)
                                          │ sx1262_port_rtt.c
                                       HAL_SPI PIO → SPI0 → SX1262 físico
```

Design rule (doc 40): Linux = mission, **never** touches migrated hardware;
RT-Thread = deterministic hardware control, **never** mission logic.

---

## 2. MCU boot chain (exact, register by register)

1. **bootrom** reads the idblock from the SPI-NAND and loads `rtthread.bin` into
   `0x40000` (DDR). Config: `src/rkbin/RKBOOT/RV1106MINIALL_SPI_NAND_TB.ini`:
   ```ini
   LOADER2=Hpmcu
   Hpmcu=bin/rv11/rtthread.bin
   [LOADER2_PARAM]
   LOAD_ADDR=0x40000
   FLAG=0x10007
   ```
2. **SPL** (ours, compiled from source) loads the FIT from `uboot.img`, which
   contains the firmware as a **standalone `mcu0`**. Config:
   `src/rkbin/RKTRUST/RV1106TOS_TB.ini`:
   ```ini
   [MCU]
   MCU0=bin/rv11/rtthread.bin,0x40000,okay
   ```
   ⚠️ **`MCU0`, never `MCU1`**: in `spl_fit_standalone_release()`
   (`src/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c`) only the id `"mcu0"`
   runs the full sequence; `"mcu1"` only sets the address and the SCR1
   stays in reset FOREVER. The mcu0 sequence (verifiable with devmem):
   | # | Register | Value | Verification from Linux |
   |---|---|---|---|
   | Non-cacheable window | `0xff040024/28` (CORE_GRF) | `0xff000`/`0xffc00` | — |
   | Cache misc | `0xff04002c` | `0x00080008` | reads `0x8` |
   | Hold reset | `0xff3b8a04` (CORECRU) | `0x1e001e` | — |
   | Boot addr | `0xff076044` (CORE_SGRF) | `0x40000` | reads `0x40000` |
   | **RELEASE** | `0xff3b8a04` | `0x1e0000` | reads `0x0` |
3. The SCR1 boots at `_start` of `rtthread.bin` → RT-Thread → services.

**Do NOT use `rv1106_hpmcu_wrap`**: it is a Rockchip mailbox command interpreter
(the `rk_meta` protocol of the prebuilt SPL, closed); without that sender the
wrap waits for orders forever. It was removed from the flow.

**Do NOT release the SCR1 towards `0xfe00000`** (old DDR staging): it bricks the
SPL. Releasing towards `0x40000` is proven behavior (Rockchip's WAKEUP flow
does the same).

---

## 3. Memory rules (violating them = the historical bugs)

| Region | Use | Rule |
|---|---|---|
| `0x40000-0x7c000` | MCU firmware (link.lds ORIGIN/LENGTH) | Reservation **WITHOUT `no-map`** in the DT (`rtos@40000`). With no-map, the gap not aligned to 2MB makes `adjust_lowmem_bounds` wipe ALL the RAM (doc 80 §3.3). |
| `0x7c000-0x7f000` | ramoops/MCU log | Same, without no-map. |
| `0x208000+` | Linux kernel | `TEXT_OFFSET=0x208000` (arch/arm/Makefile, outside the THUNDER_BOOT ifeq) + FIT `load/entry=0x208000` + `kernel_addr_r`. |
| `0xff00000-0xff80000` | rpmsg vrings | no-map reservation (2MB-aligned, safe). Linux carves it out of System RAM — correct. |
| `0xff80000-0x10000000` | rpmsg DMA pool (buffers) | Same, `shared-dma-pool`. |
| `0xff6fe000-0xff6fffff` | SRAM `hpmcu_sram` | Diagnostic markers (§8). **Persists across reboots** — clear before measuring. |

**SCR1 cache: OFF** (`# CONFIG_RT_USING_CACHE is not set` in
`board/pwncube/defconfig`). Two reasons: (a) the DCACHE init
(`SystemInit`, block `0xff640000`) hangs in our boot flow;
(b) without cache, the vrings' DDR shared-memory is coherent with the A7 without
maintenance. If it is ever re-enabled, the wrap's init dance must be ported AND
flush/invalidate added in the transport.

---

## 4. BSP configuration (board/pwncube)

Files that define the MCU (all under
`src/mcu/rt-thread/bsp/rockchip/rv1106-mcu/`):

- **`board/pwncube/defconfig`** — keys:
  - `CONFIG_RT_USING_MAILBOX=y`, `CONFIG_RT_USING_PIN=y`, `CONFIG_RT_USING_CRU=y`
  - `CONFIG_RT_USING_RPMSG_LITE=y`
  - `# CONFIG_RT_USING_CACHE is not set` (see §3)
  - **NO console/UART**: Linux owns uart2 (`ttyFIQ0`). MCU verification is
    ALWAYS via SRAM markers + IPC, never via printf.
- **`hal_conf.h`** — HAL modules active via `RT_USING_*` + 
  `HAL_SPI_MODULE_ENABLED` unconditional (the RV1106 BSP has no Kconfig for
  SPI; see §6).
- **`link.lds`** — `RAM ORIGIN=0x40000 LENGTH=0x3c000` (must match the DT
  reservation and the RKBOOT ini!) and
  `__linux_share_rpmsg_start__ = 0x0FF00000` (must match the `reg` of the
  Linux rpmsg node!).
- **`applications/`** — main.c (heartbeat), ping_echo.c (transport+echo),
  radio_service.c, sx1262_port.{h,c→_rtt.c}, and synchronized copies of
  sx1262_cmd.c/sx1262_regs.h (do NOT edit; they are regenerated from the kmod on each build).

---

## 5. rpmsg transport: the design that works (and why)

**Silicon constraint (measured)**: the SCR1 **cannot read** the mailbox's
A2B registers (STATUS/CMD/DATA return 0), but **its writes do land**
(W1C of STATUS works). B2A (MCU→A7) works completely.

Resulting design (`rpmsg_platform.c` RV1106 + `ping_echo.c`):

1. **Mailbox ISR = ONLY blind-ack**: `A2B_STATUS = 0xF` (W1C of the 4
   channels) + `s_kicked = 1`. Nothing more. Sub-µs ack → the Linux mailbox TX
   never sees the channel busy (with ack by poll at 2ms, a pingpong burst
   saturated it: "mbox send failed" after ~22 messages).
2. **ALL virtqueue processing in ONE single thread** (`ping_echo_thread`):
   `remote_init` → link wait **by polling** (`rpmsg_lite_wait_for_link_up`
   self-blocks!: the callback that marks link-up only runs when WE
   drain the vq) → `create_ept` + `ns_announce` → drain loop every 2ms
   (`rpmsg_rv1106_rx_poll`: `env_isr(vq0)` + `env_isr(vq1)`).
3. **First-kick gate**: never touch the vrings before Linux's first kick
   (they contain garbage until the host initializes them → corrupts the
   virtio probe).
4. **Deferred response**: the endpoint handlers do NOT send from the
   callback (a long handler + send in-callback froze the drain). The
   handler enqueues; `radio_service_poll()` sends it outside the drain.
5. **Co-design parameters** (must be identical on both sides):
   | Parameter | Value | Linux | MCU |
   |---|---|---|---|
   | Base shmem | `0xFF00000` | DT `rpmsg reg` | link.lds |
   | VRING_SIZE / ALIGN | `0x8000` / `0x1000` | rockchip_rpmsg.h | rpmsg_platform.h |
   | Buffers | 64 × (496+16) | RPMSG_BUF_* | RL_BUFFER_* |
   | link-id | `0x04` | DT `rockchip,link-id` | `RL_PLATFORM_SET_LINK_ID(0,4)` |
   | vq map | vq0@0xFF00000=TX del remoto, vq1@0xFF08000=RX del remoto | vring0=rvq host | callback[0]=tx |

---

## 6. Enabling a peripheral on the MCU (general pattern)

Rockchip's RV1106 BSP **did not wire up** most peripherals for the MCU
(no Kconfig, no `g_xxxDev` descriptors in RV1106's `hal_bsp.c`). The pattern
that works is **direct HAL**, with no RT-Thread driver framework:

1. **Clocks**: identify the domain's CRU — ⚠️ it is not always the main
   CRU: **SPI0 lives in the VEPUCRU** (`0xff3ba000`). The composite IDs
   are in `CMSIS/Device/RV1106/Include/rv1106.h` (e.g.
   `PCLK_SPI0_GATE=0x6012`, `CLK_SPI0_GATE=0x6013`, `SCLK_IN_SPI0_GATE`).
   Enable with `HAL_CRU_ClkEnable(<GATE>)` BEFORE touching the peripheral's
   registers (register without clock = SCR1 bus-stall = hang with no trace).
2. **IOMUX**: get pins+function from the Linux DT
   (`rv1106-pinctrl.dtsi`, e.g. spi0m0: CS0=GPIO1_C0 f4, CLK=C1 f4, MOSI=C2 f6,
   MISO=C3 f6) and apply them with
   `HAL_PINCTRL_SetIOMUX(GPIO_BANKn, GPIO_PIN_xx|..., PIN_CONFIG_MUX_FUNCk)`.
3. **Control GPIOs**: mux FUNC0 + `HAL_GPIO_SetPinDirection` +
   `HAL_GPIO_SetPinLevel/GetPinLevel`. Respect the DT polarity
   (e.g. the SX1262 reset is ACTIVE_LOW).
4. **The peripheral**: `HAL_xxx_Init` + synchronous operation (PIO/polling), with
   no IRQs until needed.
   ⚠️ **Capital SPI gotcha**: `HAL_SPI_PioTransfer` leaves the controller
   ENABLED; you must call `HAL_SPI_Stop()` after EACH transfer or the
   next `HAL_SPI_Configure` hangs the state machine (the SCR1
   freezes on the 2nd transfer). The CS (`SER`) is a separate register and survives
   the Stop, so a write-then-read multi-phase keeps the frame.
5. **Cede the bus from Linux**: `status = "disabled"` in the DT node
   (`dts/rv1106-sdk-ipc.dtsi`) + remove the corresponding Linux package/driver
   (`pkg/package-config`). Rebuild the kernel (DTB).

---

## 7. Shared Linux↔MCU driver (sx1262 pattern, replicate for sensors)

- The **portable core** (`sx1262_cmd.c` + `sx1262_regs.h`) is ONE only, lives in
  `src/sx1262-kmod/` and compiles identically in both worlds:
  ```c
  #ifdef __KERNEL__
  #include <linux/...>; #include "sx1262.h"
  #else
  #include "sx1262_port.h"       /* el shim RT-Thread */
  #endif
  ```
- `sx1262_port.h` provides the minimal linux-isms: `struct spi_transfer`,
  `msleep/udelay/usleep_range`, `jiffies/msecs_to_jiffies/time_after` (RT-Thread
  ticks), `div_u64`, `dev_info/dev_err` (no-op: no console),
  `reinit_completion/atomic_set` (DIO1 is polled), and the
  `struct sx1262_device` RT-Thread version (HAL_SPI handle + pins).
- `sx1262_port_rtt.c` implements the 7 HAL functions (mirror of
  Linux's `sx1262_hal.c`): `spi_write`, `spi_write_then_read`,
  `spi_transfer`, `wait_busy`, `reset`, `set_antsw`, plus `port_init`.
- **Synchronization**: `scripts/06-build-mcu.sh` copies cmd/regs from the kmod to
  `applications/` on each build (the copies are in .gitignore; do NOT edit them).

---

## 7bis. RadioService: IPC protocol and packet plane (VALIDATED)

Status: control + packets working on **both** radios (SPI0 and
SPI1). LoRa loopback on board verified in both directions
(radio0↔radio1, CRC ok, RSSI ~−24 dBm). The service supports `N_RADIO=2`
instances (`req[1]`=instance 0|1).

**Protocol** (endpoint `rpmsg-radio` 0x4005; request `[cmd][inst][args]`,
response mirrors `cmd` with `[1]`=err — 0 ok, 0x10 not-initialized, 0xED
invalid instance, 0xEE unknown cmd):

| cmd | name | args | extra response |
|---|---|---|---|---|
| 0x01 | PING | — | `'R','D','I','O',ver` |
| 0x02 | RESET_STATUS | — | `status` (physical reset) |
| 0x03/0x04 | READ/WRITE_REG | `a_hi a_lo [v]` | `val` (read) |
| 0x05 | INIT | `f3..f0` (Hz BE) + optional `[sf, bw_hi, bw_lo, cr, pwr]` | Full init with the ground-station reference defaults (20 dBm, BW125, SF7, CR4/5, preamble 12, CRC on, sync 0x1424, OCP 140 mA, Rx boosted gain). Extra bytes override modulation and power. |
| 0x06/0x07 | SET_FREQ/POWER | `f3..f0` / `dbm` | requires init |
| 0x08/0x09 | CW / STANDBY | — | carrier on/off |
| 0x0A/0x0B | GET_STATUS/ERRORS | — | `status` / `e_hi e_lo` |
| 0x0C | SET_ANTSW | `mode` | 0=auto 1=tx 2=rx |
| **0x0D** | **TX** | `len data..` | sends 1 packet, blocks until TX_DONE |
| **0x0E** | **RX_START** | `t_hi t_lo` (ms, 0=cont) | arms listen with software deadline; re-arms after each packet for multi-packet capture within the window |
| **0x0F** | **RX_STOP** | — | returns to standby |
| **0x10** | **SET_MOD_PARAMS** | `sf, bw_hi, bw_lo, cr` | LoRa modulation (SF 5-12, BW in kHz, CR 1-4) |
| **0x11** | **SET_PKT_PARAMS** | `pre_hi, pre_lo, hdr, plen, crc, iq` | Packet format. **`preamble`, `crc` and `iq` are all sticky** (`s_preamble`/`s_crc`/`s_iq[inst]`, defaults 8/on/std): once set they are reapplied by every TX and every RX arm until changed or reboot. Both link ends must share all three. |
| **0x12** | **GET_PKT_STATUS** | — | `rssi_hi rssi_lo snr` (RSSI/SNR of last received packet) |
| **0x13** | **GET_RSSI_INST** | — | `rssi_hi rssi_lo` (instantaneous RSSI) |
| **0x14** | **SET_SYNC** | `sw_hi, sw_lo` | LoRa sync word: 0x1424 private (reset default), 0x3444 public/LoRaWAN. Both link ends must match; lost on chip reset |

**Unsolicited events** (MCU→host, pushed from the poll loop):
- `EVT_RX 0xE0`: `[0xE0,inst,flags,len,rssi_hi,rssi_lo,snr,data..]`
  (flags bit0 = CRC ok).
- `EVT_RX_TIMEOUT 0xE1`: `[0xE1,inst]` (the window expired without a packet).

**Key design of the packet plane** (pattern to replicate for sensor RX
by IRQ):
- **Synchronous, bounded TX**: `set_packet_params(len)` → `write_buffer` →
  `set_tx(0)` → `poll_irq(TX_DONE, 3s)`. Blocks the poll thread ~50 ms (airtime);
  the mailbox ISR keeps doing the blind ACK, so it is safe.
  ⚠️ In LoRa the chip transmits EXACTLY `SetPacketParams.payloadLength` bytes
  → it must be set to the real size before each TX (init leaves it at 0xFF);
  restore 0xFF before RX.
- **Event-driven RX, non-blocking**: `RX_START` arms `set_rx()` in continuous mode
  (chip timeout = 0xFFFFFF); `radio_service_poll()` (every ~2 ms) checks **DIO1 via GPIO**
  (`sx1262_dio1_is_high`, no SPI) and, only if it is high, reads IRQ; on RX_DONE it
  reads buffer+RSSI/SNR and **pushes** `EVT_RX` to the host; then **re-arms RX** with
  remaining time if the software deadline has not expired. This allows **multi-packet
  capture** within a single RX window. On timeout (software `s_rx_deadline` or chip
  `TIMEOUT` IRQ) it pushes `EVT_RX_TIMEOUT`. NEVER do a blocking listen inside the
  handler (it would freeze the transport).

**Client** `radio_test` (`/usr/bin`, permanent): `-r <0|1>` selects the radio;
error codes display as human-readable strings (OK, ERROR, NOT_INITED). Commands:

| Command | Description |
|---------|-------------|
| `init <freq> [key=val ...]` | Robust init: every TX param individually settable by name in any order — `sf= bw= cr= power= pre= crc=on\|off iq=std\|inv sync=pub\|priv\|<hex>`. Unspecified take flat-sat defaults. Orchestrates INIT (0x05) + SET_PKT_PARAMS (0x11 sticky pre/crc/iq) + SET_SYNC (0x14) on one endpoint, prints the full resolved config. Bare positional `init <freq> sf bw cr pwr` still works. |
| `tx <text>` | Raw LoRa TX. Sends `<text>` bytes directly over the air (no SPP wrapper). |
| `ccsds <apid_hex> [text]` | Builds and transmits a CCSDS SPP packet (6-byte big-endian header + payload). Prints hex header for correlation. |
| `rx [ms]` | Receives ALL packets within the window. Output always shows **raw hex dump** plus **SPP parsing** (v, type TM/TC, APID, seq, data) when the first 3 bits indicate CCSDS version 0. |
| `power`, `mod`, `pkt`, `pkts`, `rssi`, `reg`, `wreg`, ... | Direct register/modulation control. |

**RX output examples** (raw hex + SPP auto-detect):
```
# raw LoRa:
rx: radio=1 raw[4]=68 6f 6c 61  rssi=-43 snr=10 crc=ok
# CCSDS SPP packet:
rx: radio=1 raw[10]=10 01 c0 00 00 03 70 69 6e 67  SPP: v=0 TC apid=0x001 seq=0(0x3) pay_len=4 data="ping" rssi=-47 snr=12 crc=ok
```

`loopback <freq> <texto> [txi rxi]` (full test in a single process: initializes
both, arms RX, transmits and demultiplexes response+event on ONE fd — avoids the
contention of two processes over `/dev/rpmsg0`).

---

**SX1262 app improvements:**
- **SPI write idle-wait**: `sx_spi_xfer` (`sx1262_port_rtt.c`) waits for the controller to go
  idle (`HAL_SPI_QueryBusState`) between `HAL_SPI_PioTransfer` and `HAL_SPI_Stop`, the same way
  the reference `drv_spi.c rockchip_spi_wait_idle` does. This is required because `PioTransfer`
  returns as soon as the last byte is in the TX FIFO while `Stop` disables the controller
  immediately — the idle-wait is what lets the whole write reach the wire before the controller
  stops (otherwise the tail byte can be lost). Confirmed on hardware (loopback CRC ok).
- **RSSI/SNR corrected**: `sx1262_get_packet_status` read RSSI/SNR with a one-byte
  offset. Handled in the shared core `sx1262_cmd.c`.
- **Continuous RX with software deadline**: `RX_START` with `t>0` uses a software
  deadline (`s_rx_deadline[inst]`) in the poll loop. After each `RX_DONE` the chip is
  re-armed with the remaining time, enabling **multi-packet capture** within a single
  RX window. When the deadline expires, `EVT_RX_TIMEOUT` is pushed even if the chip
  was still in continuous mode.
- **Per-instance event routing**: as before — events go to `s_rx_host[inst]`.
- **INIT extended (cmd 0x05)**: accepts optional `[sf, bw_khz>>8, bw_khz, cr, power_dbm]`
  after the 4-byte frequency. If present, overrides the `sx1262_init()` defaults.
- **on-air defaults**: bare `sx1262_init()` sets **SF7, BW125, CR4/5, preamble 12, 20 dBm,
  sync 0x1424 (private), explicit header, CRC on, IQ standard** on the chip. The OPERATIONAL
  path overrides two of these: the RadioService sticky defaults (`radio_do_tx`/
  `radio_do_rx_start`) use **preamble 8**, and `mission.h` UPLINK/DOWNLINK run at **BW250 kHz**
  (SF7, CR4/5, 20 dBm, CRC on). sync/IQ stay host-configurable; both link ends must match.
- **Wideband-TX ("invasive, grows with power") — OCP**: the splatter that widened as
  power rose was an over-tight OCP starving the PA. A prior the datasheet port had set **OCP =
  60 mA (0x18)**; the SX1262 high-power PA draws >60 mA at +22 dBm, so it clipped mid-
  burst → spectral regrowth scaling with power. `sx1262_set_output_power()` now sets **OCP
  = 140 mA (0x38)** (SX1262 datasheet value, Table 5-2); regulator stays DC-DC.
  It keeps the per-dBm PA optimal-settings table (identical to fixed 0x04/0x07 at +22, cleaner below)
  and is now called by `sx1262_init`, RADIO_CMD_SET_POWER, the INIT override, AND the
  direct callers in `telemetry_service`/`command_service` (were bypassing PA/OCP/clamp).
- **LDRO auto-calc**: `sx1262_set_modulation_params` now turns LDRO ON when the LoRa
  symbol time exceeds 16.38 ms (SF11/12 @125k, SF12 @250k). Was hardwired off → SF11/12
  links silently failed. No change for the SF7 default.
- **DIO IRQ mask** now includes `CRC_ERR` so the CRC-ok flag pushed to Linux is meaningful.
- **Datasheet audit (SX1262_datasheet.pdf, Rev 1.2) + InvertIQ bug**: full cross-check
  of every command/register write in `sx1262_cmd.c` against the datasheet. All verified
  correct — SetPaConfig (Table 13-20: deviceSel 0x00, paLut 0x01), OCP (Table 5-2: SX1262
  = 0x38/140 mA after SetPaConfig), RampTime (Table 13-41: 0x04 = 200 µs), SetRfFrequency
  (freq·2²⁵/32 MHz), BW/CRC/HeaderType codes, erratas 15.1/15.2/15.4 — EXCEPT one bug:
  `SetPacketParams` wrote **InvertIQ = 0x40** for inverted, but Table 13-70 defines
  **0x00 = standard, 0x01 = inverted** (bit 6 is not the InvertIQ field). Set to 0x01.
  Consequence: the earlier "inverted IQ" path was never actually inverting (0x40 has
  bit0=0 → chip read it as standard), so the "peer uses inverted IQ" conclusion was a
  misdiagnosis — the peer runs standard IQ (the ground-station reference default). IQ polarity
  now behaves per datasheet.
- **Exhaustive datasheet audit — additional corrections**: a full line-by-line pass
  found and corrected:
  - `sx1262_set_rx` had leftover debug — `msleep(110)+msleep(50)` (160 ms poll-thread block
    per RX arm) plus a `clear_irq_status(0xFFFF)` **after** SetRx that wiped an RxDone
    arriving in that window (silent packet loss), plus per-arm `clear_dev_errors` that
    erased real error state. Rewritten to datasheet §14.3 order: clear IRQ → SetRx → wait
    BUSY, no post-arm delay/clear.
  - `ClearDeviceErrors` (Table 13-86) sent only the opcode; needs opcode + 0x00 + 0x00
    (3 bytes) — errors were never actually cleared. Corrected.
  - `CalibrateImage` (Table 13-19) sent a spurious 4th byte; takes opcode + freq1 + freq2
    (3 bytes). Corrected.
  - **Rx Boosted Gain** (reg 0x08AC = 0x96, Table 9-3) added in init — the reset default
    0x94 is power-saving (~few dB less sensitivity).
  - `sx1262_set_frequency` now runs CalibrateImage after SetRfFrequency internally, so any
    runtime cross-band retune stays calibrated (init step 7b removed as redundant).
  - Removed init's bogus reads of undocumented regs 0x01D4/0x01D5/0x01D7; corrected a latent
    `read_registers` length truncation at len≥255 (uint8_t→size_t).
  - Everything else (SetPaConfig, SetTxParams/ramp, SetModulationParams BW/SF/CR/LDRO,
    SetRfFrequency, OCP 0x38, RSSI/SNR offsets, erratas 15.1/15.2/15.4, IRQ bit masks,
    read/write buffer offsets) verified correct against the datasheet.
- **Image-cal order**: `calibrate_image_for_freq` now runs AFTER `set_frequency`
  (step 7b). With the old order it ran before, leaving the image centred on the POR
  default (~915 MHz) so RX only worked near 915 (loopback proved RX dead at 916).
  With this order RX works across 915-920 MHz and RSSI improved from ~-90 to ~-18 dBm.
- **Register defines**: all hardcoded register addresses replaced by `SX1262_REG_*`
  defines from `sx1262_regs.h` (IRQ enables, OCP, syncword, TX clamp config).

**IQ polarity + interop:**
- **Configurable IQ (sticky)**: `SET_PKT_PARAMS` (0x11) stores `s_iq[inst]`, applied by
  both `radio_do_tx` and `radio_do_rx_start` — both were previously hardcoded to standard
  IQ, making inverted-IQ links impossible. Set once via `radio_test pkt <pre> <hdr> <plen>
  <crc> <iq>`; resets to standard on reboot.
- **Errata 15.4 (Inverted-IQ operation)**: `sx1262_set_packet_params` now writes
  RegIqPolaritySetup (0x0736) on every call — clears bit 2 for inverted IQ, sets it for
  standard. Without this workaround inverted-IQ LoRa packets are frequently lost.
- **The "peer uses inverted IQ" hypothesis was wrong.** For a while, failing to receive the
  external peer was blamed on an IQ-polarity mismatch (LoRaWAN gateway convention: TX
  inverted, RX standard). That was a **misdiagnosis**: the InvertIQ register bug (0x40 vs
  0x01, above) meant IQ never actually inverted, and the reference peer runs **standard IQ**.
  The real blockers, both now resolved, were the **image-calibration order** (RX only worked near
  915 MHz until `CalibrateImage` was moved after `SetRfFrequency` — RSSI ~−90 → ~−18 dBm) and
  the InvertIQ register value itself. Frequency, the on-board RX loopback, and SF/BW/sync/antsw
  were all ruled out during the hunt.

---

## 7ter. SensorService: BME280 + ICM-42670 over I²C0 (VALIDATED)

Second migrated service, same form as RadioService (§7bis). The MCU owns the
**I²C0** bus (0xFF310000); Linux cedes it in the DT (`&i2c0 status="disabled"`).

**Peripheral (§6 pattern, direct HAL in POLL, no `drv_i2c`):**
- Clocks in **PERICRU**: `HAL_CRU_ClkEnable(PCLK_I2C0_GATE)` + `CLK_I2C0_GATE`
  before touching registers. Input rate: `HAL_CRU_ClkGetFreq(CLK_I2C0)`
  (default mux = 200 MHz) → `HAL_I2C_Init(..., I2C_400K)`.
- i2c0m0 pins: `HAL_PINCTRL_SetIOMUX(GPIO_BANK1, A3|A4, FUNC2)` (SCL=GPIO1_A3,
  SDA=GPIO1_A4; the board has external pull-ups).
- Register read: `HAL_I2C_ConfigureMode(REG_CON_MOD_REGISTER_TX,
  MRXADDR=slave<<1|VALID(0), MRXRADDR=reg|VALID(0))` + `SetupMsg(..., M_RD)` +
  pumping `HAL_I2C_Transfer(POLL)` / `HAL_I2C_IRQHandler` until ≠BUSY + `Close`.
  Write: `REG_CON_MOD_TX` with buf `[reg,val]` (the HAL prepends `slave<<1`).
  Unlike SPI, the I²C poll has a timeout → a wiring failure gives an
  error, it does not freeze the thread. `applications/sensor_port_rtt.c`.

**Drivers (MCU-native, in `applications/`):** `bme280.c` (calibration 0x88/0xE1 +
fixed-point compensation from the Bosch datasheet: T/H 32-bit, P 64-bit; forced mode)
and `icm42670.c` (reset → WHO_AM_I 0x67 → PWR_MGMT0 accel+gyro low-noise; samples
BE16, ported from the register map of `src/icm42670-kmod/icm42670.c`). They are not
shared with Linux because the Linux drivers are regmap/IIO (not portable).

**Protocol** (endpoint `rpmsg-sensor` 0x4006; `[cmd][args]`, response mirrors
`cmd` with `[1]`=err — 0 ok, 0x11 I²C error, 0xEE unknown):

| cmd | name | args | response |
|---|---|---|---|
| 0x01 | PING | — | `'S','E','N','S',ver` |
| 0x02 | WHOAMI | `chip` (0=bme,1=icm) | `id` (0x60 / 0x67) |
| 0x10 | BME280_READ | — | `t(i32 LE m°C), p(u32 Pa), h(u32 m%RH)` |
| 0x20 | IMU_READ | — | `ax,ay,az,gx,gy,gz,temp` (7×i16 LE) |

Handlers with **deferred response** (same reason as radio: don't send from the
callback) flushed by `sensor_service_poll()`. `applications/sensor_service.c`.

**Client** `sensor_test` (`/usr/bin`): `ping | whoami <bme|icm> | bme | imu | all`.

**rpmsg endpoint routing with TWO services (key lesson):** with two announced
channels there are two `/dev/rpmsg_ctrlN`. The `dst` of `RPMSG_CREATE_EPT_IOCTL`
**is ignored if the ctrl belongs to another channel** (the channel forces its own
address). Therefore the client must (1) choose the ctrl whose
`/sys/class/rpmsg/rpmsg_ctrlN/device` is its channel (`rpmsg-radio`/`rpmsg-sensor`),
and (2) use a unique `src` per process and locate its `/dev/rpmsgN` via
`/sys/class/rpmsg/rpmsgN/src` (not by enumeration order, which is a race). The
clients destroy their endpoint on exit (`RPMSG_DESTROY_EPT_IOCTL`) so as not to leak
nodes. rcS binds BOTH channels (`bind_rpmsg` in a loop).

---

## 8. Diagnostics: SRAM marker map (read with `devmem` from Linux)

**⚠️ The SRAM persists across reboots: zero it before measuring**
(`for a in 858 85c 860 ...; do devmem 0xff6ff$a 32 0; done`).

| Address | Writer | Sane values |
|---|---|---|
| `0xff5c0030` | `_start` (startup.S) | `0xCAFE0005` = the SCR1 executed |
| `0xff6ff810/814` | startup: stack/data OK | `0xCAFE0006/0007` |
| `0xff6ff81c` | SystemInit returned | `0xCAFE0009` |
| `0xff6ff820-830` | stages of rt_hw_board_init | `0xCAFE000A..000E` |
| `0xff6ff840` | rpmsg app stage | `0xCAFE0101→0104` (announced) |
| `0xff6ff844` | poll heartbeat | `0xCAB0xxxx` **advancing** |
| `0xff6ff848` | rx_poll stage | `0xE4` in steady state |
| `0xff6ff850` | echo counter | `0xEC0xxxxx` |
| `0xff6ff858` | RadioService: last cmd | `0xAD00ccnn` (cc=counter, nn=cmd) |
| `0xff6ff85c/860` | handler err / send rc | `0xAD200000`/`0xAD300000` |
| `0xff6ff864/868` | RL_ASSERT fired + address | `0xDEADA55E` = assert (map 868 with rtthread.map) |
| `0xff6ff888` | SensorService: last cmd | `0xAE0000nn` (nn=cmd) / `0xAE000001` = announced |
| `0xff6ff88c` | SensorService: handler err | `0xAE1000ee` (ee=err, 0=ok) |
| `0xff6ff86c-878` | rx_callback bisect (TEMP) | `0xCB000006` at the end of the drain |
| `0xff6ff900` | main heartbeat (if compiled) | `0xB000xxxx` advancing |

Express diagnostics: heartbeat `844` frozen = poll thread dead (see `848`
and `86c` for the exact stage); `858` not updating = the command never arrived
(bind?, vrings?); firmware at `0x40000` intact: `devmem 0x40000` = `0x0000A401`.

---

## 9. Build and flashing (full chain, with ALL the gotchas)

```sh
bash scripts/06-build-mcu.sh          # MCU firmware (+syncs sx1262_cmd)
./scripts/01-build-uboot.sh           # ⚠️ REQUIRED after 06: uboot.img/idblock EMBED rtthread.bin
pkg/pkg.sh clean radio-client && pkg/pkg.sh build radio-client   # ⚠️ build does NOT rebuild if staging already exists
./scripts/03-build-rootfs.sh          # installs the ALREADY-built packages + skeleton (rcS)
./scripts/02-build-kernel.sh          # only if kernel/DT changed
./scripts/04-pack-image.sh            # ALWAYS before flashing
tools/upgrade_tool UF output/images/update.img   # ⚠️ UF ONLY; `DI -b` reports ok but does NOT write
```

- To maskrom without touching the board: `reboot loader` from the Linux shell.
- With the board hung: recovery button on power-up — **release it as soon as
  the flashing starts** or the post-UF reboot stays mute in RKUART.
- The radio channel bind is automatic (`rootfs/skeleton/etc/init.d/rcS`).

---

## 10. Checklist to replicate with a new service (e.g. SensorService/I²C)

> **Already executed for the SensorService (§7ter).** This checklist is the general
> recipe; below, in parentheses, how each point was resolved for I²C.

1. Does the peripheral have Kconfig/descriptors in the RV1106 BSP? I²C does
   (`RT_USING_I2Cn` + drv_i2c); if not, direct-HAL pattern (§6).
2. Clocks: which CRU does it live in? (`rv1106.h`, search `<PERIF>_GATE`). Ungate before
   everything.
3. Pins: copy from the Linux DT (bank/pin/function) → `HAL_PINCTRL_SetIOMUX`.
4. Shared driver core: `#ifdef __KERNEL__` + `<driver>_port.h` +
   `<driver>_port_rtt.c` + sync in script 06 (§7).
5. Service: copy the `radio_service.c` pattern — own endpoint
   (0x4006, "rpmsg-sensor"), `*_service_attach()` from ping_echo after the
   announce, **handlers with deferred response**, protocol [cmd][inst][args],
   error `0x10` for "no init".
6. State that gets lost: if the chip loses config on reset, gate the
   commands with an `inited` flag and return an explanatory error.
7. Linux: cede the bus in the DT + client in `src/<x>-client/` + package in
   `pkg/available/` + (if applicable) bind/insmod in `rcS`.
8. ALWAYS test with clean markers (§8) and one change per flash.
9. When changing any MCU source: full 06→01→04→UF chain (§9).

---

## 11. Temporary instrumentation pending removal (once declared stable)

- Kernel: earlyprintk/earlycon (bootargs), DEBUG_LL block of the defconfig,
  S/P/M sentinels in head.S/head-common.S, printascii in init/main.c, memblock
  dump in mmu.c.
- MCU: startup/board_base CAFE markers, RXCB in rpmsg_lite.c,
  instrumented RL_HANG, AD markers of radio_service (useful — maybe
  keep the service ones).
- U-Boot: MCU_CACHE_MISC write in arch_cpu_init (redundant, harmless).
- Re-enabling RT_USING_CACHE requires resolving §3 (DCACHE init) + IPC coherence.
