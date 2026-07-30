[English](uboot.en.md)

# U-Boot para RV1106 SDK

## Flujo de Arranque

```
BootROM
  └─► DDR init (binario rkbin)
        └─► SPL (Secondary Program Loader)
              └─► Trust/TEE (OP-TEE)
                    └─► U-Boot proper
                          └─► Kernel (imagen FIT)
```

BootROM es la máscara ROM dentro del RV1106. Lee el **IDBlock** (DDR init + USB plug + SPL) desde el medio de arranque, inicializa la DDR, carga el SPL en SRAM, luego el SPL carga Trust (OP-TEE) y U-Boot proper en DRAM.

## Compilación

### make.sh

U-Boot se compila usando el envoltorio `make.sh` de Rockchip en `src/u-boot/`. Este envuelve `make` y luego llama a las herramientas `rkbin` (`boot_merger`, `trust_merger`, `loaderimage`) para producir las imágenes finales.

### --spl-new

`make.sh --spl-new` le indica al sistema de compilación que use el **SPL recién compilado** (en lugar de uno precompilado de `rkbin`). Sin esta bandera, `make.sh` usa el binario SPL precompilado en `rkbin/bin/`.

```bash
cd src/u-boot && ./make.sh --spl-new CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
```

### Formato INI de boot_merger

`boot_merger` es una herramienta de Rockchip que empaqueta DDR init, USB plug y SPL en una sola imagen IDBlock. Usa un archivo INI que describe los binarios de entrada y las rutas de salida.

**`RV1106MINIALL.ini`** (de `src/rkbin/RKBOOT/`):

```ini
[CHIP_NAME]
NAME=RV1106
[VERSION]
MAJOR=1
MINOR=1
[CODE471_OPTION]
NUM=1
Path1=bin/rv11/rv1106_ddr_924MHz_v1.15.bin
Sleep=1
[CODE472_OPTION]
NUM=1
Path1=bin/rv11/rv1106_usbplug_v1.09.bin
[LOADER_OPTION]
NUM=2
LOADER1=FlashData
LOADER2=FlashBoot
FlashData=bin/rv11/rv1106_ddr_924MHz_v1.15.bin
FlashBoot=bin/rv11/rv1106_spl_v1.02.bin
[OUTPUT]
PATH=rv1106_download_v1.15.108.bin
IDB_PATH=rv1106_idblock_v1.15.102.img
[SYSTEM]
NEWIDB=true
[FLAG]
471_RC4_OFF=true
RC4_OFF=true
CREATE_IDB=true
```

Secciones:
- `[CHIP_NAME]` — nombre del SoC.
- `[VERSION]` — número de versión incrustado en la salida.
- `[CODE471_OPTION]` — binario DDR init (cargado por BootROM).
- `[CODE472_OPTION]` — binario USB plug (modo de descarga USB de Rockchip).
- `[LOADER_OPTION]` — FlashData (DDR init para el loader) y FlashBoot (SPL).
- `[OUTPUT]` — `PATH` = `download.bin` (loader completo), `IDB_PATH` = `idblock.img` (IDBlock alineado).
- `[FLAG]` — banderas de cifrado RC4; `CREATE_IDB=true` genera el IDBlock.

### Sobrescritura del Orden de Arranque del SPL

El SPL sondea los dispositivos de arranque en el orden especificado por la propiedad `u-boot,spl-boot-order` en `arch/arm/dts/rv1106-u-boot.dtsi`. El valor predeterminado es:

```
u-boot,spl-boot-order = &sdmmc, &spi_nor, &spi_nand, &emmc;
```

Cuando `BOOT_MEDIUM=spi_nand`, el script de compilación (`01-build-uboot.sh`) modifica el DTSI para mover `&spi_nand` al frente:

```
u-boot,spl-boot-order = &spi_nand, &emmc;
```

Esto evita el tiempo de espera de MMC (sdmmc) en placas que no tienen tarjeta SD, acelerando el arranque varios segundos. El DTSI original se respalda y se restaura después de la compilación.

## Diseño de Particiones (SPI NAND — 256 MB)

`mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)`

| Partición | Offset | Tamaño | Descripción                 |
|-----------|--------|--------|-----------------------------|
| env       | 0      | 256K   | Entorno de U-Boot           |
| idblock   | 256K   | 512K   | Cargador DDR init + SPL     |
| uboot     | 768K   | 256K   | U-Boot proper               |
| boot      | 1024K  | 32M    | Imagen FIT del kernel       |
| rootfs    | —      | resto  | Sistema de archivos raíz    |

La partición `idblock` es la salida combinada de `boot_merger`. `uboot.img` se escribe en la partición `uboot`. La partición `boot` almacena el kernel FIT (árbol de imágenes planas con kernel + DTB).

## Archivos de Salida

Todas las imágenes generadas se colocan en `output/images/`:

| Archivo         | Origen                                                                     | Descripción                            |
|-----------------|----------------------------------------------------------------------------|----------------------------------------|
| `idblock.img`   | Salida de `boot_merger`, normalizado desde `rv1106_idblock_*.img`          | IDBlock: DDR init + SPL (alineado 1KB) |
| `download.bin`  | Salida de `boot_merger`, normalizado desde `rv1106_download_*.bin`         | Loader raw completo para herramientas Rockchip |
| `uboot.img`     | U-Boot proper (imagen FIT de u-boot con relleno ITB)                       | Binario U-Boot para la partición `uboot` |
| `trust.img`     | Salida de `trust_merger`, o fallback a `tee.bin`                           | Firmware de confianza OP-TEE + HPMCU   |

El script de compilación normaliza los nombres con versión de Rockchip (ej. `rv1106_idblock_v1.15.102.img`, `rv1106_download_v1.15.108.bin`) a nombres canónicos (`idblock.img`, `download.bin`) para consistencia entre compilaciones. `trust.img` es producido por `trust_merger`; si solo `tee.bin` está disponible (sin INI de trust_merger), se usa directamente como `trust.img`.
