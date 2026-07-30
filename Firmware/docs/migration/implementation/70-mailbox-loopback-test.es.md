# Test de loopback por mailbox crudo (sin rpmsg)

Diagnóstico de transporte A7 ↔ MCU basado en el **patrón probado por Rockchip**
(mailbox crudo + memoria compartida, **sin vrings**), documentado en
`riscv2arm.md` (codemap del SDK Luckfox).

## Por qué

El camino rpmsg-lite se **cuelga**: al llegar el primer kick de Linux, el poll
thread entra en `env_isr → virtqueue_notification` y se congela (heartbeat del
poller clavado). Además ya está confirmado que el SCR1 **lee `A2B_STATUS` como 0**
aunque el A7 vea el bit pendiente (por eso `HAL_MBOX_IrqHandler` nunca encuentra
el canal). La referencia Luckfox **nunca** usa rpmsg ni recibe A2B en la MCU
(ver [`luckfox-amp-is-b2a-only`]): solo hace B2A crudo MCU→ARM.

Este test aísla el problema en dos preguntas:

1. **¿Funciona MCU→ARM (B2A)?** — la dirección que Rockchip sí ejercita.
2. **¿Qué registros de RX (A2B) puede LEER el SCR1?** — `A2B_STATUS` no; ¿y
   `A2B_CMD(3)` / `A2B_DAT(3)`? Si la MCU puede leer `A2B_DAT(3)`, el enfoque limpio
   es sondear ahí el magic `0x524D5347`, sin int_mux ni virtqueue.

## Piezas del paquete

| Pieza | Ruta |
|-------|------|
| Switch de build | `src/mcu/.../rv1106-mcu/applications/ipc_test_cfg.h` (`IPC_RAW_MBOX_TEST 1`) |
| App de test MCU | `src/mcu/.../rv1106-mcu/applications/ipc_mbox_test.c` |
| rpmsg ping/echo | `ping_echo.c` — se compila fuera cuando el switch está en 1 |
| Referencia | `riscv2arm.md` |

**Seguridad:** el test toca SOLO los registros del mailbox (`0xff5c0000`) y el
scratch de `hpmcu_sram` (`0xff6ff900+`). **Nunca** toca los vrings en DDR
(`0x0ff00000`) — que es lo que corrompía el probe de virtio de Linux. Por eso es
seguro de arrancar sin importar el estado de Linux.

## Cómo construir y flashear

```sh
# 1) Con IPC_RAW_MBOX_TEST = 1 en ipc_test_cfg.h
./build.sh mcu                                   # -> output/mcu/rtthread.bin
cp output/mcu/rtthread.bin src/rkbin/bin/rv11/rtthread.bin
./build.sh                                       # repack -> output/images/update.img
# 2) Placa en maskrom (corto SPI-NAND CLK<->GND al alimentar)
sudo ./tools/upgrade_tool UF output/images/update.img
```

## Observación desde Linux (serial, `busybox devmem`)

```sh
# Heartbeat MCU->ARM (B2A ch2 DATA): debe avanzar 0xC0DExxxx  -> prueba TX del MCU
devmem 0xff5c0044 32

# Lo que la MCU LEE (scratch):
devmem 0xff6ff900 32   # heartbeat del thread   (0xB000xxxx avanzando)
devmem 0xff6ff904 32   # A2B_STATUS visto x MCU  (A7 ve 0x08 con kick pendiente)
devmem 0xff6ff908 32   # A2B_CMD(3)  visto x MCU  (A7 escribió 0x00000004)   <- CLAVE
devmem 0xff6ff90c 32   # A2B_DAT(3)  visto x MCU  (A7 escribió 0x524D5347)   <- CLAVE
devmem 0xff6ff910 32   # int_mux status0 visto x MCU
devmem 0xff6ff914 32   # A2B_INTEN visto x MCU    (sanity: 0x08)
devmem 0xff6ff918 32   # latch de kick detectado  (0x600Dxxxx si DAT(3)==magic)
devmem 0xff6ff91c 32   # marker de arranque del test (0x1EE70001)
```

### Enviar un kick A2B controlado a la MCU (imita al kernel)

```sh
devmem 0xff5c0024 32 0x524D5347   # A2B_DAT(3) = magic
devmem 0xff5c0020 32 0x00000004   # A2B_CMD(3) = link_id (levanta el doorbell)
# luego re-leer 0xff6ff908 / 0xff6ff90c / 0xff6ff918
```

## Interpretación

- `0xff5c0044` avanza → **MCU→ARM (B2A) OK** (transporte base vivo).
- `0xff6ff908` = `0x04` y `0xff6ff90c` = `0x524D5347` → **la MCU SÍ puede leer
  A2B_CMD/DAT** → enfoque limpio: sondear `A2B_DAT(3)==magic` como detector de kick,
  descartar `A2B_STATUS`/int_mux/virtqueue-por-IRQ.
- `0xff6ff908`/`0x90c` = 0 → la MCU tampoco lee CMD/DAT → queda solo el camino
  int_mux (`0xff6ff910` bit2) como detector.
- `0xff6ff900` **no avanza** → el RTOS de la MCU murió (excepción) — revisar.
