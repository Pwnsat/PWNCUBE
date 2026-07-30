# Librería CCSDS SPP + TC seguro

Librería CCSDS pequeña y autocontenida para el firmware del CubeSat. Provee el
codec del encabezado primario del Space Packet Protocol (SPP) y una capa de
**telecomando (TC) seguro** **alineada a FlatSat (ElectronicCats)** — secondary
header con timestamp + cifrado por niveles (plano / XOR / AES-128-CTR) + CRC-16 —
sobre el frame SPP.

> El núcleo AES-128, la construcción del IV de CTR, los niveles de dificultad y el
> orden CRC-sobre-plaintext son un **match byte-a-byte** del firmware FlatSat
> (ElectronicCats), así que los frames construidos aquí los acepta ese firmware y
> viceversa (con el mismo nivel de dificultad). Para la comparación completa de
> los tres protocolos (ElectronicCats vs PWNSat vs esta librería) ver
> `docs/flatsat-protocol-comparison.md` (en la raíz del repo).

> Es un **ejercicio de CTF**. La crypto es real y estándar, pero se usa con
> **debilidades intencionales** (clave fija, IV de baja entropía, sin enforcement)
> para que el enlace seguro se pueda romper. Ver "Debilidades intencionales". No
> reutilizar tal cual.

## Archivos

| Archivo | Rol |
|------|------|
| `../spp.h` / `../spp.c` | Encabezado primario SPP (6 B, big-endian), builders TM/TC/idle, `spp_unpack_packet` (parse). Lleva el over-read deliberado (vuln #9). |
| `ccsds_crc.h` / `ccsds_crc.c` | CRC-16-CCITT (poly 0x1021, init 0xFFFF), el campo Packet Error Control de CCSDS. |
| `ccsds_aes.h` / `ccsds_aes.c` | AES-128 (port byte-a-byte del núcleo de ElectronicCats) + keystream XOR en modo CTR. Contiene la clave fija `"PWNSAT_K3Y_2026!"`. |
| `ccsds_tc.h` / `ccsds_tc.c` | TC seguro: construir (`ccsds_tc_build`), descifrar/verificar in-place (`ccsds_tc_unsecure`), selector de dificultad. |
| `ccsds_133_space_packet.*`, `ccsds_types.h` | Modelo CCSDS previo standalone (sin usar por la misión; referencia). |

## Formato de wire del TC seguro

```
+------------------+-----------------+---------------------+--------+
| SPP primary (6)  | sec-hdr: TS (4) | payload             | CRC(2) |
+------------------+-----------------+---------------------+--------+
   type=TC             timestamp u32     cifrado por          CRC-16
   sec_hdr_flag=1      (big-endian)      nivel de dificultad  CCITT
   APID (11)           = IV de AES-CTR   (sin padding)
```

- `packet_data_length` = `(4 + payload + 2) − 1`.
- El timestamp de 32 bits del secondary header es además el **IV de AES-128-CTR**
  (`IV = timestamp || 0×8 || índice_de_bloque`).
- Nivel de cifrado (`ccsds_tc_set_difficulty`): `0/1` plano, `2` XOR `"PWNSAT"`,
  `≥3` AES-128-CTR. CTR/XOR son cifradores de flujo → **sin padding de bloque**.
- **El CRC-16-CCITT se calcula sobre el frame en PLAINTEXT** (primary + sec-hdr +
  payload plano), y luego se cifra el payload. El receptor descifra primero y
  después verifica el CRC — es el orden de ElectronicCats.

## API

```c
void ccsds_tc_set_difficulty(uint8_t level);   /* 0/1 plano, 2 XOR, >=3 AES */

/* Construye un TC seguro (sec-hdr timestamp + CRC sobre plaintext + payload cifrado). */
int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint32_t timestamp, const uint8_t *payload,
                   uint16_t payload_len, uint16_t *out_total);

/* Descifra + verifica in-place. sec_hdr=1: lee timestamp (*timestamp_out),
 * descifra según nivel, reporta validez del CRC (*crc_ok, NO se exige), reescribe
 * pkt->data a los args en claro. sec_hdr=0: CCSDS_TC_ERR_NOSEC, no toca pkt. */
int ccsds_tc_unsecure(space_packet_t *pkt, uint32_t *timestamp_out, int *crc_ok);
```

Ambos extremos deben elegir el **mismo nivel de dificultad**
(`ccsds_tc_set_difficulty`) — el nivel es out-of-band, igual que en el FlatSat real.

### Integración en la misión

`command_service.c :: process_rx_packet` llama a `ccsds_tc_unsecure` justo después
de `spp_unpack_packet`, y luego despacha por APID. El valor de retorno y `crc_ok`
se **ignoran** a propósito (ver abajo).

## Debilidades intencionales (CTF)

El AES-128 es correcto de libro (verificado contra el vector NIST FIPS-197). Las
debilidades explotables están en cómo se gestiona la clave y en que no se exige:

1. **Clave fija, en claro.** `CCSDS_TC_AES_KEY` (`ccsds_aes.c`) es una sola clave
   de 128 bits compilada, ASCII `"PWNSAT_K3Y_2026!"`, así que aparece literal en un
   dump de flash por el telecomando FLASH (APID 0x07). Recuperarla → descifrar o
   forjar cualquier TC seguro.
2. **IV de baja entropía, reuso de keystream.** El IV de CTR es solo un timestamp
   de 32 bits; el nivel XOR es una clave de 6 bytes repetida. Con known-plaintext
   o keystreams capturados se rompe.
3. **El CRC es un check, no un MAC.** Alterar + recalcular el CRC-16 es trivial —
   no da autenticación.
4. **El receptor NO exige la capa.** `process_rx_packet` acepta un TC en claro
   (`sec_hdr=0`) y nunca rechaza por CRC, así que todos los exploits previos por
   RF siguen funcionando — la vuln de sin-autenticación.

Preservadas de la base: **#9** over-read en el unpack SPP (`spp.c`), **#10**
underflow de longitud en broadcast (`command_service.c`), **#6** secondary header
sin inicializar en el path TM de FLASH.

## Pruebas

`test/tc_test.c` es un test host: comprueba el núcleo AES-128 contra el vector
NIST FIPS-197, hace round-trip build→unsecure en cada nivel de dificultad, replica
la lógica exacta del receptor ElectronicCats (descifrado AES-CTR +
CRC-sobre-plaintext) para probar que un FlatSat real aceptaría nuestro frame, y
confirma que un TC en claro pasa intacto. Vive en `test/` para que el build de la
misión (que hace glob de `ccsds/*.c`, no recursivo) nunca arrastre su `main()`.
Ejecutar desde este directorio:

```sh
gcc -Wall -Wextra -I. -I.. -o /tmp/tc_test \
    test/tc_test.c ../spp.c ccsds_crc.c ccsds_aes.c ccsds_tc.c && /tmp/tc_test
```

## Construir otro comando seguro (ejemplo)

```c
ccsds_tc_set_difficulty(CCSDS_TC_DIFF_AES);          /* igualar el nivel del peer */
packet_counter_t cnt; spp_counters_init(&cnt);
uint8_t args[] = { 0x00, 0xC8 };                     /* thruster 0, potencia 200 */
space_packet_t pkt; uint16_t total;
ccsds_tc_build(&pkt, &cnt, SPP_APID_TC_SET_THRUSTER, /*timestamp=*/0x11223344,
               args, sizeof(args), &total);
/* transmitir los primeros `total` bytes de `pkt` por el uplink */
```
