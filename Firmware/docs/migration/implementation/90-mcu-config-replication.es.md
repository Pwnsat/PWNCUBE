# 90 — Configuración del MCU (RISC-V/RT-Thread): guía definitiva de replicación

> **Estado: DOS SERVICIOS VALIDADOS EN PLACA.**
> El MCU posee **las dos radios SX1262 (SPI0/SPI1)** y **los dos sensores I²C0
> (BME280 + ICM-42670)**. RadioService: CW 915 MHz visto en analizador + loopback
> LoRa en placa (radio0↔radio1, CRC ok) con TX/RX de paquetes, RX continuo con
> re-arme y push de eventos DIO1 por instancia — §7bis. SensorService: chip-IDs
> (0x60/0x67), BME280 compensado (T/P/H) e ICM-42670 (accel 1 g en Z, giro, temp)
> leídos por I²C desde el MCU y expuestos a Linux por rpmsg — §7ter. Los dos
> servicios coexisten en el mismo hilo de poll sin interferirse. Este documento
> captura, a detalle extremo, TODO lo necesario para configurar el MCU y replicar
> el patrón con nuevos periféricos/servicios. Complementa el doc 80 (arranque dual).

---

## 1. Qué corre dónde (mapa mental)

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

Regla del diseño (doc 40): Linux = misión, **nunca** toca hardware migrado;
RT-Thread = control determinista de hardware, **nunca** lógica de misión.

---

## 2. Cadena de arranque del MCU (exacta, registro por registro)

1. **bootrom** lee el idblock de la SPI-NAND y carga `rtthread.bin` en
   `0x40000` (DDR). Config: `src/rkbin/RKBOOT/RV1106MINIALL_SPI_NAND_TB.ini`:
   ```ini
   LOADER2=Hpmcu
   Hpmcu=bin/rv11/rtthread.bin
   [LOADER2_PARAM]
   LOAD_ADDR=0x40000
   FLAG=0x10007
   ```
2. **SPL** (nuestro, compilado de fuente) carga el FIT de `uboot.img`, que
   contiene el firmware como **standalone `mcu0`**. Config:
   `src/rkbin/RKTRUST/RV1106TOS_TB.ini`:
   ```ini
   [MCU]
   MCU0=bin/rv11/rtthread.bin,0x40000,okay
   ```
   ⚠️ **`MCU0`, jamás `MCU1`**: en `spl_fit_standalone_release()`
   (`src/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c`) solo el id `"mcu0"`
   ejecuta la secuencia completa; `"mcu1"` solo fija la dirección y el SCR1
   queda en reset PARA SIEMPRE. La secuencia mcu0 (verificable con devmem):
   | # | Registro | Valor | Verificación desde Linux |
   |---|---|---|---|
   | Ventana no-cacheable | `0xff040024/28` (CORE_GRF) | `0xff000`/`0xffc00` | — |
   | Cache misc | `0xff04002c` | `0x00080008` | lee `0x8` |
   | Hold reset | `0xff3b8a04` (CORECRU) | `0x1e001e` | — |
   | Boot addr | `0xff076044` (CORE_SGRF) | `0x40000` | lee `0x40000` |
   | **RELEASE** | `0xff3b8a04` | `0x1e0000` | lee `0x0` |
3. El SCR1 arranca en `_start` de `rtthread.bin` → RT-Thread → servicios.

**NO usar el `rv1106_hpmcu_wrap`**: es un intérprete de comandos por mailbox de
Rockchip (protocolo `rk_meta` del SPL prebuilt, cerrado); sin ese emisor el
wrap espera órdenes eternamente. Fue eliminado del flujo.

**NO liberar el SCR1 hacia `0xfe00000`** (staging DDR viejo): brickea el SPL.
Liberar hacia `0x40000` es comportamiento probado (el flujo WAKEUP de Rockchip
hace lo mismo).

---

## 3. Reglas de memoria (violarlas = los bugs históricos)

| Región | Uso | Regla |
|---|---|---|
| `0x40000-0x7c000` | Firmware MCU (link.lds ORIGIN/LENGTH) | Reserva **SIN `no-map`** en el DT (`rtos@40000`). Con no-map, el hueco no-alineado a 2MB hace que `adjust_lowmem_bounds` borre TODA la RAM (doc 80 §3.3). |
| `0x7c000-0x7f000` | ramoops/log MCU | Ídem, sin no-map. |
| `0x208000+` | Kernel Linux | `TEXT_OFFSET=0x208000` (arch/arm/Makefile, fuera del ifeq THUNDER_BOOT) + FIT `load/entry=0x208000` + `kernel_addr_r`. |
| `0xff00000-0xff80000` | vrings rpmsg | Reserva no-map (alineada a 2MB, segura). Linux la recorta de System RAM — correcto. |
| `0xff80000-0x10000000` | pool DMA rpmsg (buffers) | Ídem, `shared-dma-pool`. |
| `0xff6fe000-0xff6fffff` | SRAM `hpmcu_sram` | Marcadores de diagnóstico (§8). **Persiste entre reboots** — limpiar antes de medir. |

**Caché del SCR1: APAGADA** (`# CONFIG_RT_USING_CACHE is not set` en
`board/pwncube/defconfig`). Dos razones: (a) el init del DCACHE
(`SystemInit`, bloque `0xff640000`) se cuelga en nuestro flujo de arranque;
(b) sin caché, el shared-memory DDR de los vrings es coherente con el A7 sin
mantenimiento. Si algún día se reactiva, hay que portar la danza de init del
wrap Y añadir flush/invalidate en el transporte.

---

## 4. Configuración del BSP (board/pwncube)

Archivos que definen al MCU (todos bajo
`src/mcu/rt-thread/bsp/rockchip/rv1106-mcu/`):

- **`board/pwncube/defconfig`** — claves:
  - `CONFIG_RT_USING_MAILBOX=y`, `CONFIG_RT_USING_PIN=y`, `CONFIG_RT_USING_CRU=y`
  - `CONFIG_RT_USING_RPMSG_LITE=y`
  - `# CONFIG_RT_USING_CACHE is not set` (ver §3)
  - **SIN consola/UART**: Linux posee uart2 (`ttyFIQ0`). La verificación del
    MCU es SIEMPRE por marcadores SRAM + IPC, nunca por printf.
- **`hal_conf.h`** — módulos HAL activos por `RT_USING_*` + 
  `HAL_SPI_MODULE_ENABLED` incondicional (el BSP RV1106 no tiene Kconfig de
  SPI; ver §6).
- **`link.lds`** — `RAM ORIGIN=0x40000 LENGTH=0x3c000` (¡debe casar con la
  reserva del DT y el RKBOOT ini!) y
  `__linux_share_rpmsg_start__ = 0x0FF00000` (¡debe casar con el `reg` del
  nodo rpmsg de Linux!).
- **`applications/`** — main.c (heartbeat), ping_echo.c (transporte+echo),
  radio_service.c, sx1262_port.{h,c→_rtt.c}, y copias sincronizadas de
  sx1262_cmd.c/sx1262_regs.h (NO editar; se regeneran del kmod en cada build).

---

## 5. Transporte rpmsg: el diseño que funciona (y por qué)

**Restricción de silicio (medida)**: el SCR1 **no puede leer** los registros
A2B del mailbox (STATUS/CMD/DATA devuelven 0), pero **sus escrituras sí
aterrizan** (W1C de STATUS funciona). B2A (MCU→A7) funciona completo.

Diseño resultante (`rpmsg_platform.c` RV1106 + `ping_echo.c`):

1. **ISR del mailbox = SOLO ack-ciego**: `A2B_STATUS = 0xF` (W1C de los 4
   canales) + `s_kicked = 1`. Nada más. Ack sub-µs → el TX del mailbox de
   Linux nunca ve el canal ocupado (con ack por poll a 2ms, una ráfaga de
   pingpong lo saturaba: "mbox send failed" tras ~22 mensajes).
2. **TODO el procesamiento de virtqueues en UN solo hilo** (`ping_echo_thread`):
   `remote_init` → espera de link **sondeando** (¡`rpmsg_lite_wait_for_link_up`
   se auto-bloquea!: el callback que marca link-up solo corre cuando NOSOTROS
   drenamos el vq) → `create_ept` + `ns_announce` → bucle de drenado cada 2ms
   (`rpmsg_rv1106_rx_poll`: `env_isr(vq0)` + `env_isr(vq1)`).
3. **Gate del primer kick**: jamás tocar los vrings antes del primer kick de
   Linux (contienen basura hasta que el host los inicializa → corrompe el
   probe de virtio).
4. **Respuesta diferida**: los handlers de endpoint NO envían desde el
   callback (un handler largo + send in-callback congelaba el drenado). El
   handler encola; `radio_service_poll()` la envía fuera del drenado.
5. **Parámetros de co-diseño** (deben ser idénticos en ambos lados):
   | Parámetro | Valor | Linux | MCU |
   |---|---|---|---|
   | Base shmem | `0xFF00000` | DT `rpmsg reg` | link.lds |
   | VRING_SIZE / ALIGN | `0x8000` / `0x1000` | rockchip_rpmsg.h | rpmsg_platform.h |
   | Buffers | 64 × (496+16) | RPMSG_BUF_* | RL_BUFFER_* |
   | link-id | `0x04` | DT `rockchip,link-id` | `RL_PLATFORM_SET_LINK_ID(0,4)` |
   | vq map | vq0@0xFF00000=TX del remoto, vq1@0xFF08000=RX del remoto | vring0=rvq host | callback[0]=tx |

---

## 6. Habilitar un periférico en el MCU (patrón general)

El BSP RV1106 de Rockchip **no cableó** la mayoría de periféricos para el MCU
(sin Kconfig, sin descriptores `g_xxxDev` en `hal_bsp.c` de RV1106). El patrón
que funciona es **HAL directo**, sin framework de drivers RT-Thread:

1. **Clocks**: identificar el CRU del dominio — ⚠️ no siempre es el CRU
   principal: **SPI0 vive en el VEPUCRU** (`0xff3ba000`). Los IDs compuestos
   están en `CMSIS/Device/RV1106/Include/rv1106.h` (ej.
   `PCLK_SPI0_GATE=0x6012`, `CLK_SPI0_GATE=0x6013`, `SCLK_IN_SPI0_GATE`).
   Habilitar con `HAL_CRU_ClkEnable(<GATE>)` ANTES de tocar registros del
   periférico (registro sin reloj = bus-stall del SCR1 = cuelgue sin traza).
2. **IOMUX**: sacar pines+función del DT de Linux
   (`rv1106-pinctrl.dtsi`, ej. spi0m0: CS0=GPIO1_C0 f4, CLK=C1 f4, MOSI=C2 f6,
   MISO=C3 f6) y aplicarlos con
   `HAL_PINCTRL_SetIOMUX(GPIO_BANKn, GPIO_PIN_xx|..., PIN_CONFIG_MUX_FUNCk)`.
3. **GPIOs de control**: mux FUNC0 + `HAL_GPIO_SetPinDirection` +
   `HAL_GPIO_SetPinLevel/GetPinLevel`. Respetar la polaridad del DT
   (ej. reset del SX1262 es ACTIVE_LOW).
4. **El periférico**: `HAL_xxx_Init` + operación síncrona (PIO/polling), sin
   IRQs mientras no haga falta.
   ⚠️ **Gotcha capital del SPI**: `HAL_SPI_PioTransfer` deja el controlador
   HABILITADO; hay que llamar `HAL_SPI_Stop()` tras CADA transferencia o la
   siguiente `HAL_SPI_Configure` cuelga la máquina de estados (el SCR1 se
   congela en el 2º transfer). El CS (`SER`) es registro aparte y sobrevive
   al Stop, así que un write-then-read multi-fase mantiene el frame.
5. **Ceder el bus desde Linux**: `status = "disabled"` en el nodo del DT
   (`dts/rv1106-sdk-ipc.dtsi`) + retirar el paquete/driver Linux
   correspondiente (`pkg/package-config`). Rebuild del kernel (DTB).

---

## 7. Driver compartido Linux↔MCU (patrón sx1262, replicar para sensores)

- El **core portable** (`sx1262_cmd.c` + `sx1262_regs.h`) es UNO solo, vive en
  `src/sx1262-kmod/` y compila idéntico en ambos mundos:
  ```c
  #ifdef __KERNEL__
  #include <linux/...>; #include "sx1262.h"
  #else
  #include "sx1262_port.h"       /* el shim RT-Thread */
  #endif
  ```
- `sx1262_port.h` provee los linux-ismos mínimos: `struct spi_transfer`,
  `msleep/udelay/usleep_range`, `jiffies/msecs_to_jiffies/time_after` (ticks
  RT-Thread), `div_u64`, `dev_info/dev_err` (no-op: sin consola),
  `reinit_completion/atomic_set` (el DIO1 se sondea), y el
  `struct sx1262_device` versión RT-Thread (handle HAL_SPI + pines).
- `sx1262_port_rtt.c` implementa las 7 funciones HAL (espejo de
  `sx1262_hal.c` de Linux): `spi_write`, `spi_write_then_read`,
  `spi_transfer`, `wait_busy`, `reset`, `set_antsw`, más `port_init`.
- **Sincronización**: `scripts/06-build-mcu.sh` copia cmd/regs del kmod a
  `applications/` en cada build (las copias están en .gitignore; NO editarlas).

---

## 7bis. RadioService: protocolo IPC y plano de paquetes (VALIDADO)

Estado: control + paquetes funcionando en **ambas** radios (SPI0 y
SPI1). Loopback LoRa en placa verificado en las dos direcciones
(radio0↔radio1, CRC ok, RSSI ~−24 dBm). El servicio soporta `N_RADIO=2`
instancias (`req[1]`=instancia 0|1).

**Protocolo** (endpoint `rpmsg-radio` 0x4005; petición `[cmd][inst][args]`,
respuesta espeja `cmd` con `[1]`=err — 0 ok, 0x10 no-inicializado, 0xED
instancia inválida, 0xEE cmd desconocido):

| cmd | nombre | args | respuesta extra |
|---|---|---|---|---|
| 0x01 | PING | — | `'R','D','I','O',ver` |
| 0x02 | RESET_STATUS | — | `status` (reset físico) |
| 0x03/0x04 | READ/WRITE_REG | `a_hi a_lo [v]` | `val` (read) |
| 0x05 | INIT | `f3..f0` (Hz BE) + opcional `[sf, bw_hi, bw_lo, cr, pwr]` | Init completo con los defaults de la referencia de estación terrena (20 dBm, BW125, SF7, CR4/5, preámbulo 12, CRC on, sync 0x1424, OCP 140 mA, Rx boosted gain). Los bytes extra sobrescriben modulación y potencia. |
| 0x06/0x07 | SET_FREQ/POWER | `f3..f0` / `dbm` | requiere init |
| 0x08/0x09 | CW / STANDBY | — | portadora on/off |
| 0x0A/0x0B | GET_STATUS/ERRORS | — | `status` / `e_hi e_lo` |
| 0x0C | SET_ANTSW | `mode` | 0=auto 1=tx 2=rx |
| **0x0D** | **TX** | `len data..` | envía 1 paquete, bloquea hasta TX_DONE |
| **0x0E** | **RX_START** | `t_hi t_lo` (ms, 0=cont) | arma la escucha con deadline por software; se re-arma tras cada paquete para captura multi-paquete dentro de la ventana |
| **0x0F** | **RX_STOP** | — | vuelve a standby |
| **0x10** | **SET_MOD_PARAMS** | `sf, bw_hi, bw_lo, cr` | Modulación LoRa (SF 5-12, BW en kHz, CR 1-4) |
| **0x11** | **SET_PKT_PARAMS** | `pre_hi, pre_lo, hdr, plen, crc, iq` | Formato de paquete. **`preamble`, `crc` e `iq` son todos pegajosos** (`s_preamble`/`s_crc`/`s_iq[inst]`, defaults 8/on/std): una vez seteados se re-aplican en cada TX y en cada armado de RX hasta cambiarlos o reiniciar. Ambos extremos del enlace deben compartir los tres. |
| **0x12** | **GET_PKT_STATUS** | — | `rssi_hi rssi_lo snr` (RSSI/SNR del último paquete recibido) |
| **0x13** | **GET_RSSI_INST** | — | `rssi_hi rssi_lo` (RSSI instantáneo) |
| **0x14** | **SET_SYNC** | `sw_hi, sw_lo` | Sync word LoRa: 0x1424 privada (default de reset), 0x3444 pública/LoRaWAN. Ambos extremos deben coincidir; se pierde con el reset del chip |

**Eventos no solicitados** (MCU→host, empujados desde el poll loop):
- `EVT_RX 0xE0`: `[0xE0,inst,flags,len,rssi_hi,rssi_lo,snr,data..]`
  (flags bit0 = CRC ok).
- `EVT_RX_TIMEOUT 0xE1`: `[0xE1,inst]` (venció la ventana sin paquete).

**Diseño clave del plano de paquetes** (patrón a replicar para RX de sensores
por IRQ):
- **TX síncrono, acotado**: `set_packet_params(len)` → `write_buffer` →
  `set_tx(0)` → `poll_irq(TX_DONE, 3s)`. Bloquea el poll thread ~50 ms (airtime);
  el ISR del mailbox sigue haciendo el ACK ciego, así que es seguro.
  ⚠️ En LoRa el chip transmite EXACTAMENTE `SetPacketParams.payloadLength` bytes
  → hay que fijarlo al tamaño real antes de cada TX (el init lo deja en 0xFF);
  restaurar 0xFF antes de RX.
- **RX dirigido por eventos, NO bloqueante**: `RX_START` arma `set_rx()` en modo continuo
  (timeout del chip = 0xFFFFFF); `radio_service_poll()` (cada ~2 ms) mira **DIO1 por GPIO**
  (`sx1262_dio1_is_high`, sin SPI) y, sólo si está alto, lee IRQ; en RX_DONE lee
  buffer+RSSI/SNR y **empuja** `EVT_RX` al host; luego **re-arma RX** con el tiempo
  restante si el deadline por software no ha vencido. Esto permite **captura multi-paquete**
  dentro de una sola ventana de RX. En timeout (deadline por software `s_rx_deadline` o IRQ
  `TIMEOUT` del chip) empuja `EVT_RX_TIMEOUT`. NUNCA hacer una escucha bloqueante dentro del
  handler (congelaría el transporte).

**Cliente** `radio_test` (`/usr/bin`, permanente): `-r <0|1>` elige la radio;
los códigos de error se muestran como cadenas legibles (OK, ERROR, NOT_INITED). Comandos:

| Comando | Descripción |
|---------|-------------|
| `init <freq> [key=val ...]` | Init robusto: cada parámetro de TX seteable individualmente por nombre en cualquier orden — `sf= bw= cr= power= pre= crc=on\|off iq=std\|inv sync=pub\|priv\|<hex>`. Los no especificados toman los defaults del flat-sat. Orquesta INIT (0x05) + SET_PKT_PARAMS (0x11 pre/crc/iq pegajosos) + SET_SYNC (0x14) en un solo endpoint, imprime la config completa resuelta. El posicional pelado `init <freq> sf bw cr pwr` sigue funcionando. |
| `tx <text>` | TX LoRa cruda. Envía los bytes de `<text>` directo al aire (sin envoltorio SPP). |
| `ccsds <apid_hex> [text]` | Construye y transmite un paquete CCSDS SPP (header big-endian de 6 bytes + payload). Imprime el header en hex para correlación. |
| `rx [ms]` | Recibe TODOS los paquetes dentro de la ventana. La salida siempre muestra **volcado hex crudo** más **parseo SPP** (v, tipo TM/TC, APID, seq, data) cuando los primeros 3 bits indican CCSDS versión 0. |
| `power`, `mod`, `pkt`, `pkts`, `rssi`, `reg`, `wreg`, ... | Control directo de registro/modulación. |

**Ejemplos de salida de RX** (hex crudo + auto-detección SPP):
```
# LoRa crudo:
rx: radio=1 raw[4]=68 6f 6c 61  rssi=-43 snr=10 crc=ok
# paquete CCSDS SPP:
rx: radio=1 raw[10]=10 01 c0 00 00 03 70 69 6e 67  SPP: v=0 TC apid=0x001 seq=0(0x3) pay_len=4 data="ping" rssi=-47 snr=12 crc=ok
```

`loopback <freq> <texto> [txi rxi]` (test completo en un solo proceso: inicializa
ambas, arma RX, transmite y demultiplexa respuesta+evento en UN fd — evita la
contención de dos procesos sobre `/dev/rpmsg0`).

---

**Mejoras de la app SX1262:**
- **Espera de idle en el write SPI**: `sx_spi_xfer` (`sx1262_port_rtt.c`) espera a que el
  controlador quede idle (`HAL_SPI_QueryBusState`) entre `HAL_SPI_PioTransfer` y `HAL_SPI_Stop`,
  igual que el `drv_spi.c rockchip_spi_wait_idle` de referencia. Es necesario porque `PioTransfer`
  retorna en cuanto el último byte está en el FIFO de TX mientras `Stop` deshabilita el
  controlador de inmediato — la espera de idle es lo que permite que todo el write llegue al
  cable antes de detener el controlador (si no, el último byte se pierde). Confirmado en
  hardware (loopback CRC ok).
- **RSSI/SNR corregidos**: `sx1262_get_packet_status` leía RSSI/SNR con un desfase de
  un byte. Tratado en el core compartido `sx1262_cmd.c`.
- **RX continuo con deadline por software**: `RX_START` con `t>0` usa un deadline por
  software (`s_rx_deadline[inst]`) en el poll loop. Tras cada `RX_DONE` el chip se re-arma
  con el tiempo restante, habilitando **captura multi-paquete** dentro de una sola ventana
  de RX. Cuando vence el deadline, se empuja `EVT_RX_TIMEOUT` aunque el chip siguiera en
  modo continuo.
- **Ruteo de eventos por instancia**: como antes — los eventos van a `s_rx_host[inst]`.
- **INIT extendido (cmd 0x05)**: acepta un opcional `[sf, bw_khz>>8, bw_khz, cr, power_dbm]`
  tras la frecuencia de 4 bytes. Si está presente, sobrescribe los defaults de `sx1262_init()`.
- **defaults on-air**: `sx1262_init()` pelado setea **SF7, BW125, CR4/5, preámbulo 12, 20 dBm,
  sync 0x1424 (privada), header explícito, CRC on, IQ estándar** en el chip. El camino
  OPERATIVO sobrescribe dos de estos: los defaults pegajosos del RadioService (`radio_do_tx`/
  `radio_do_rx_start`) usan **preámbulo 8**, y `mission.h` UPLINK/DOWNLINK corren a **BW250 kHz**
  (SF7, CR4/5, 20 dBm, CRC on). sync/IQ quedan configurables por el host; ambos extremos del
  enlace deben coincidir.
- **TX de banda ancha ("invasiva, crece con la potencia") — OCP**: el splatter que se
  ensanchaba a medida que subía la potencia era un OCP demasiado ajustado que hambreaba el PA.
  Un port previo del datasheet había puesto **OCP = 60 mA (0x18)**; el PA de alta potencia del
  SX1262 consume >60 mA a +22 dBm, así que clipeaba a mitad de ráfaga → regeneración espectral
  que escalaba con la potencia. `sx1262_set_output_power()` ahora setea **OCP = 140 mA (0x38)**
  (valor del datasheet SX1262, Tabla 5-2); el regulador queda en DC-DC. Mantiene la tabla de
  ajustes óptimos del PA por dBm (idéntica a los fijos 0x04/0x07 a +22, más limpia por debajo)
  y ahora es llamada por `sx1262_init`, RADIO_CMD_SET_POWER, el override del INIT, Y los
  llamadores directos en `telemetry_service`/`command_service` (que se saltaban PA/OCP/clamp).
- **Auto-cálculo de LDRO**: `sx1262_set_modulation_params` ahora enciende LDRO cuando el tiempo
  de símbolo LoRa supera los 16.38 ms (SF11/12 @125k, SF12 @250k). Estaba cableado en off →
  los enlaces SF11/12 fallaban en silencio. Sin cambios para el default SF7.
- **Máscara de IRQ de DIO** ahora incluye `CRC_ERR` para que el flag de CRC-ok empujado a Linux
  sea significativo.
- **Auditoría del datasheet (SX1262_datasheet.pdf, Rev 1.2) + el bug de InvertIQ**:
  cross-check completo de cada escritura de comando/registro en `sx1262_cmd.c` contra el
  datasheet. Todo verificado correcto — SetPaConfig (Tabla 13-20: deviceSel 0x00, paLut 0x01),
  OCP (Tabla 5-2: SX1262 = 0x38/140 mA tras SetPaConfig), RampTime (Tabla 13-41: 0x04 = 200 µs),
  SetRfFrequency (freq·2²⁵/32 MHz), códigos BW/CRC/HeaderType, erratas 15.1/15.2/15.4 — EXCEPTO
  un bug: `SetPacketParams` escribía **InvertIQ = 0x40** para invertida, pero la Tabla 13-70
  define **0x00 = estándar, 0x01 = invertida** (el bit 6 no es el campo InvertIQ). Fijado a
  0x01. Consecuencia: el camino previo de "IQ invertida" nunca estuvo invirtiendo realmente
  (0x40 tiene bit0=0 → el chip lo leía como estándar), así que la conclusión "el peer usa IQ
  invertida" fue un diagnóstico erróneo — el peer corre IQ estándar (el default de la referencia
  de estación terrena). La polaridad IQ ahora se comporta según el datasheet.
- **Auditoría exhaustiva del datasheet — correcciones adicionales**: una pasada completa línea por
  línea encontró y corrigió:
  - `sx1262_set_rx` tenía debug remanente — `msleep(110)+msleep(50)` (160 ms de bloqueo del
    poll thread por cada armado de RX) más un `clear_irq_status(0xFFFF)` **después** de SetRx
    que borraba un RxDone llegando en esa ventana (pérdida silenciosa de paquetes), más un
    `clear_dev_errors` por armado que borraba estado de error real. Reescrito al orden del
    datasheet §14.3: clear IRQ → SetRx → esperar BUSY, sin delay/clear post-armado.
  - `ClearDeviceErrors` (Tabla 13-86) enviaba sólo el opcode; necesita opcode + 0x00 + 0x00
    (3 bytes) — los errores nunca se limpiaban de verdad. Corregido.
  - `CalibrateImage` (Tabla 13-19) enviaba un 4º byte espurio; toma opcode + freq1 + freq2
    (3 bytes). Corregido.
  - **Rx Boosted Gain** (reg 0x08AC = 0x96, Tabla 9-3) agregado en el init — el default de
    reset 0x94 es de ahorro de energía (~unos pocos dB menos de sensibilidad).
  - `sx1262_set_frequency` ahora corre CalibrateImage tras SetRfFrequency internamente, así que
    cualquier retune cross-band en runtime queda calibrado (el paso 7b del init se quitó por
    redundante).
  - Se quitaron las lecturas espurias del init de los regs no documentados 0x01D4/0x01D5/0x01D7;
    se corrigió una truncación latente de longitud en `read_registers` con len≥255 (uint8_t→size_t).
  - Todo lo demás (SetPaConfig, SetTxParams/ramp, SetModulationParams BW/SF/CR/LDRO,
    SetRfFrequency, OCP 0x38, offsets de RSSI/SNR, erratas 15.1/15.2/15.4, máscaras de bits de
    IRQ, offsets de read/write buffer) verificado correcto contra el datasheet.
- **Orden de calibración de imagen**: `calibrate_image_for_freq` ahora corre DESPUÉS de
  `set_frequency` (paso 7b). Con el orden viejo corría antes, dejando la imagen centrada en el
  default de POR (~915 MHz) → el RX solo funcionaba cerca de 915 (el loopback probó RX muerto a
  916). Con este orden el RX funciona en todo 915-920 MHz y el RSSI mejoró de ~-90 a ~-18 dBm.
- **Defines de registro**: todas las direcciones de registro cableadas reemplazadas por defines
  `SX1262_REG_*` de `sx1262_regs.h` (habilitaciones de IRQ, OCP, syncword, config de clamp de TX).

**Polaridad IQ + interop:**
- **IQ configurable (pegajoso)**: `SET_PKT_PARAMS` (0x11) guarda `s_iq[inst]`, aplicado por
  `radio_do_tx` y `radio_do_rx_start` — antes ambos fijaban IQ estándar a fuego, lo que hacía
  imposible un enlace con IQ invertida. Se setea una vez con `radio_test pkt <pre> <hdr> <plen>
  <crc> <iq>`; vuelve a estándar en cada reinicio.
- **Errata 15.4 (operación con IQ invertida)**: `sx1262_set_packet_params` ahora escribe
  RegIqPolaritySetup (0x0736) en cada llamada — limpia el bit 2 para IQ invertida, lo setea
  para estándar. Sin este workaround los paquetes LoRa con IQ invertida se pierden con frecuencia.
- **La hipótesis "el peer usa IQ invertida" era errónea.** Por un tiempo, el no recibir al peer
  externo se atribuyó a un mismatch de polaridad IQ (convención de gateway LoRaWAN: TX invertido,
  RX estándar). Eso fue un **diagnóstico erróneo**: el bug del registro InvertIQ (0x40 vs 0x01,
  arriba) hacía que la IQ nunca invirtiera de verdad, y el peer de referencia corre **IQ estándar**.
  Los bloqueos reales, ambos ya resueltos, eran el **orden de calibración de imagen** (el RX solo
  funcionaba cerca de 915 MHz hasta mover `CalibrateImage` después de `SetRfFrequency` — RSSI ~−90
  → ~−18 dBm) y el propio valor del registro InvertIQ. Frecuencia, el loopback de RX en placa, y
  SF/BW/sync/antsw fueron todos descartados durante la búsqueda.

---

## 7ter. SensorService: BME280 + ICM-42670 por I²C0 (VALIDADO)

Segundo servicio migrado, misma forma que RadioService (§7bis). El MCU posee el
bus **I²C0** (0xFF310000); Linux lo cede en el DT (`&i2c0 status="disabled"`).

**Periférico (patrón §6, HAL directo en POLL, sin `drv_i2c`):**
- Clocks en **PERICRU**: `HAL_CRU_ClkEnable(PCLK_I2C0_GATE)` + `CLK_I2C0_GATE`
  antes de tocar registros. Rate de entrada: `HAL_CRU_ClkGetFreq(CLK_I2C0)`
  (mux por defecto = 200 MHz) → `HAL_I2C_Init(..., I2C_400K)`.
- Pines i2c0m0: `HAL_PINCTRL_SetIOMUX(GPIO_BANK1, A3|A4, FUNC2)` (SCL=GPIO1_A3,
  SDA=GPIO1_A4; el board tiene pull-ups externos).
- Lectura de registro: `HAL_I2C_ConfigureMode(REG_CON_MOD_REGISTER_TX,
  MRXADDR=slave<<1|VALID(0), MRXRADDR=reg|VALID(0))` + `SetupMsg(..., M_RD)` +
  bombeo `HAL_I2C_Transfer(POLL)` / `HAL_I2C_IRQHandler` hasta ≠BUSY + `Close`.
  Escritura: `REG_CON_MOD_TX` con buf `[reg,val]` (la HAL antepone `slave<<1`).
  A diferencia del SPI, el poll de I²C tiene timeout → un fallo de cableado da
  error, no congela el hilo. `applications/sensor_port_rtt.c`.

**Drivers (MCU-native, en `applications/`):** `bme280.c` (calibración 0x88/0xE1 +
compensación fixed-point del datasheet Bosch: T/H 32-bit, P 64-bit; modo forzado)
y `icm42670.c` (reset → WHO_AM_I 0x67 → PWR_MGMT0 accel+gyro low-noise; muestras
BE16, portado del mapa de registros de `src/icm42670-kmod/icm42670.c`). No se
comparten con Linux porque los drivers Linux son regmap/IIO (no portables).

**Protocolo** (endpoint `rpmsg-sensor` 0x4006; `[cmd][args]`, respuesta espeja
`cmd` con `[1]`=err — 0 ok, 0x11 error I²C, 0xEE desconocido):

| cmd | nombre | args | respuesta |
|---|---|---|---|
| 0x01 | PING | — | `'S','E','N','S',ver` |
| 0x02 | WHOAMI | `chip` (0=bme,1=icm) | `id` (0x60 / 0x67) |
| 0x10 | BME280_READ | — | `t(i32 LE m°C), p(u32 Pa), h(u32 m%RH)` |
| 0x20 | IMU_READ | — | `ax,ay,az,gx,gy,gz,temp` (7×i16 LE) |

Handlers con **respuesta diferida** (misma razón que radio: no enviar desde el
callback) flusheada por `sensor_service_poll()`. `applications/sensor_service.c`.

**Cliente** `sensor_test` (`/usr/bin`): `ping | whoami <bme|icm> | bme | imu | all`.

**Ruteo de endpoints rpmsg con DOS servicios (lección clave):** con dos canales
anunciados hay dos `/dev/rpmsg_ctrlN`. El `dst` del `RPMSG_CREATE_EPT_IOCTL`
**se ignora si el ctrl pertenece a otro canal** (el canal fuerza su propia
dirección). Por eso el cliente debe (1) elegir el ctrl cuyo
`/sys/class/rpmsg/rpmsg_ctrlN/device` es su canal (`rpmsg-radio`/`rpmsg-sensor`),
y (2) usar un `src` único por proceso y localizar su `/dev/rpmsgN` por
`/sys/class/rpmsg/rpmsgN/src` (no por orden de enumeración, que es carrera). Los
clientes destruyen su endpoint al salir (`RPMSG_DESTROY_EPT_IOCTL`) para no fugar
nodos. rcS liga AMBOS canales (`bind_rpmsg` en un bucle).

---

## 8. Diagnóstico: mapa de marcadores SRAM (leer con `devmem` desde Linux)

**⚠️ La SRAM persiste entre reboots: poner a cero antes de medir**
(`for a in 858 85c 860 ...; do devmem 0xff6ff$a 32 0; done`).

| Dirección | Escritor | Valores sanos |
|---|---|---|
| `0xff5c0030` | `_start` (startup.S) | `0xCAFE0005` = el SCR1 ejecutó |
| `0xff6ff810/814` | startup: stack/data OK | `0xCAFE0006/0007` |
| `0xff6ff81c` | SystemInit retornó | `0xCAFE0009` |
| `0xff6ff820-830` | etapas de rt_hw_board_init | `0xCAFE000A..000E` |
| `0xff6ff840` | etapa de la app rpmsg | `0xCAFE0101→0104` (announced) |
| `0xff6ff844` | heartbeat del poll | `0xCAB0xxxx` **avanzando** |
| `0xff6ff848` | etapa del rx_poll | `0xE4` en estado estable |
| `0xff6ff850` | contador de echo | `0xEC0xxxxx` |
| `0xff6ff858` | RadioService: último cmd | `0xAD00ccnn` (cc=contador, nn=cmd) |
| `0xff6ff85c/860` | err del handler / rc del send | `0xAD200000`/`0xAD300000` |
| `0xff6ff864/868` | RL_ASSERT disparado + dirección | `0xDEADA55E` = assert (mapear 868 con rtthread.map) |
| `0xff6ff888` | SensorService: último cmd | `0xAE0000nn` (nn=cmd) / `0xAE000001` = anunciado |
| `0xff6ff88c` | SensorService: err del handler | `0xAE1000ee` (ee=err, 0=ok) |
| `0xff6ff86c-878` | bisect de rx_callback (TEMP) | `0xCB000006` al final del drenado |
| `0xff6ff900` | heartbeat de main (si compilado) | `0xB000xxxx` avanzando |

Diagnóstico exprés: heartbeat `844` congelado = hilo de poll muerto (ver `848`
y `86c` para la etapa exacta); `858` sin actualizar = el comando nunca llegó
(¿bind?, ¿vrings?); firmware en `0x40000` intacto: `devmem 0x40000` = `0x0000A401`.

---

## 9. Build y flasheo (cadena completa, con TODOS los gotchas)

```sh
bash scripts/06-build-mcu.sh          # firmware MCU (+sincroniza sx1262_cmd)
./scripts/01-build-uboot.sh           # ⚠️ OBLIGATORIO tras 06: uboot.img/idblock EMBEBEN rtthread.bin
pkg/pkg.sh clean radio-client && pkg/pkg.sh build radio-client   # ⚠️ build NO reconstruye si existe staging
./scripts/03-build-rootfs.sh          # instala paquetes YA construidos + skeleton (rcS)
./scripts/02-build-kernel.sh          # solo si cambió kernel/DT
./scripts/04-pack-image.sh            # SIEMPRE antes de flashear
tools/upgrade_tool UF output/images/update.img   # ⚠️ SOLO UF; `DI -b` dice ok pero NO escribe
```

- A maskrom sin tocar la placa: `reboot loader` desde el shell Linux.
- Con placa colgada: botón de recovery al energizar — **soltarlo apenas
  empiece el flasheo** o el reboot post-UF queda mudo en RKUART.
- El bind del canal radio es automático (`rootfs/skeleton/etc/init.d/rcS`).

---

## 10. Checklist para replicar con un servicio nuevo (ej. SensorService/I²C)

> **Ya ejecutado para el SensorService (§7ter).** Este checklist es la receta
> general; abajo, entre paréntesis, cómo se resolvió cada punto para I²C.

1. ¿El periférico tiene Kconfig/descriptores en el BSP RV1106? I²C sí
   (`RT_USING_I2Cn` + drv_i2c); si no, patrón HAL-directo (§6).
2. Clocks: ¿en qué CRU vive? (`rv1106.h`, buscar `<PERIF>_GATE`). Ungate antes
   de todo.
3. Pines: copiar del DT de Linux (banco/pin/función) → `HAL_PINCTRL_SetIOMUX`.
4. Core del driver compartido: `#ifdef __KERNEL__` + `<driver>_port.h` +
   `<driver>_port_rtt.c` + sync en el script 06 (§7).
5. Servicio: copiar el patrón `radio_service.c` — endpoint propio
   (0x4006, "rpmsg-sensor"), `*_service_attach()` desde ping_echo tras el
   announce, **handlers con respuesta diferida**, protocolo [cmd][inst][args],
   error `0x10` para "sin init".
6. Estado que se pierde: si el chip pierde config al reset, gatear los
   comandos con un flag `inited` y devolver error explicativo.
7. Linux: ceder el bus en el DT + cliente en `src/<x>-client/` + paquete en
   `pkg/available/` + (si aplica) bind/insmod en `rcS`.
8. Probar SIEMPRE con marcadores limpios (§8) y de a un cambio por flasheo.
9. Al cambiar cualquier fuente del MCU: cadena 06→01→04→UF completa (§9).

---

## 11. Instrumentación temporal pendiente de retirar (cuando se declare estable)

- Kernel: earlyprintk/earlycon (bootargs), bloque DEBUG_LL del defconfig,
  centinelas S/P/M en head.S/head-common.S, printascii en init/main.c, volcado
  de memblock en mmu.c.
- MCU: marcadores CAFE del startup/board_base, RXCB en rpmsg_lite.c,
  RL_HANG instrumentado, marcadores AD del radio_service (útiles — quizá
  conservar los del servicio).
- U-Boot: write de MCU_CACHE_MISC en arch_cpu_init (redundante, inofensivo).
- Reactivar RT_USING_CACHE requiere resolver §3 (DCACHE init) + coherencia IPC.
