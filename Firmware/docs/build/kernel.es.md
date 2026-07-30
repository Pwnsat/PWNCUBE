# RV1106 SDK Kernel

[English](kernel.en.md)

## Versión del Kernel

| Parámetro           | Valor                                    |
|---------------------|------------------------------------------|
| Versión Linux       | 5.10.160                                 |
| Arquitectura        | ARM32 (ARMv7-A)                          |
| CPU                 | Cortex-A7 (un solo núcleo)               |
| Defconfig           | `rv1106_minimal_defconfig`               |
| Toolchain           | `arm-rockchip830-linux-uclibcgnueabihf-` |
| Prefijo cruzado     | `arm-rockchip830-linux-uclibcgnueabihf-` |
| Banderas compilador | `-march=armv7-a -mfloat-abi=hard -mfpu=neon` |

## Estrategia de Defconfig

La defconfig (`configs/kernel/rv1106_minimal_defconfig` → `rv1106_minimal_defconfig`) elimina todos los subsistemas no esenciales para producir el kernel más pequeño posible para cámaras IPC basadas en SPI NAND. Los siguientes están explícitamente deshabilitados:

| Subsistema           | Config                                  |
|----------------------|-----------------------------------------|
| MMC/SD               | `# CONFIG_MMC is not set`               |
| IPV6                 | `# CONFIG_IPV6 is not set`              |
| Netfilter            | `# CONFIG_NETFILTER is not set`         |
| UBI/UBIFS            | `# CONFIG_MTD_UBI is not set`, `# CONFIG_UBIFS_FS is not set` |
| SQUASHFS             | `# CONFIG_SQUASHFS is not set`          |
| Subsistema media     | `# CONFIG_MEDIA_SUPPORT is not set`     |
| NPU                  | `# CONFIG_ROCKCHIP_RKNPU is not set`    |
| Sonido               | `# CONFIG_SOUND is not set`             |
| DRM                  | `# CONFIG_DRM is not set`               |
| WiFi                 | `# CONFIG_WLAN is not set`              |
| DEBUG_FS             | `# CONFIG_DEBUG_FS is not set`          |
| earlycon             | (eliminado de bootargs en DTS)          |
| Drivers staging      | `# CONFIG_STAGING is not set`           |
| WireGuard            | `# CONFIG_WIREGUARD is not set`         |
| SCSI                 | `# CONFIG_SCSI is not set`              |
| USB_SERIAL           | `# CONFIG_USB_SERIAL_CH343 is not set`  |

SPI NAND (`CONFIG_MTD_SPI_NAND=y`) es el único medio de almacenamiento.

## Arranque Silencioso

La línea de comandos del kernel incluye `loglevel=3`, que suprime mensajes por debajo de KERN_WARNING:

```text
CONFIG_CMDLINE="user_debug=31 loglevel=3"
```

Combinado con `console=ttyFIQ0` (depurador FIQ como consola), produce un arranque limpio y casi silencioso, mostrando solo mensajes de nivel warning o superior.

## Jerarquía del Device Tree

El DTS final se ensambla a partir de tres capas:

```
rv1106g-sdk.dts
├── rv1106.dtsi           — Base del SoC (CPU, clocks, pinctrl, etc.)
├── rv1106-evb.dtsi       — Periféricos de la placa EVB
└── rv1106-sdk-ipc.dtsi   — Específicos de la placa IPC (CSI, audio, reguladores)
    └── rv1106-amp.dtsi   — Definiciones AMP (procesamiento asimétrico)
```

### Cambios Clave en DTS vs. Rockchip Stock

| Cambio                         | Detalle                                                     |
|--------------------------------|-------------------------------------------------------------|
| Sin `earlycon`                | Eliminado de `bootargs` en `rv1106g-sdk.dts`                |
| `sdmmc` deshabilitado         | `&sdmmc { status = "disabled"; }`                          |
| Consola                        | `bootargs = "console=ttyFIQ0"` (sin prefijo `earlycon`)    |
| Regulador `vdd_arm`            | `regulator-min-microvolt = <900000>; regulator-max-microvolt = <900000>;` — fijo a 0.9 V |
| SPI NAND                       | `&sfc` habilitado a 75 MHz, RX cuádruple                    |
| Ethernet                       | `&gmac` habilitado                                          |
| USB                            | OTG habilitado en modo peripheral                           |
| Códec de audio                 | `&acodec` con GPIO de control PA                            |
| Cámaras CSI                    | Sensor dual (SC3336 + MIS5001) vía MIPI CSI-2               |
| `spi0` / `spi1` deshabilitados  | Cedidos al MCU RISC-V — los dos radios SX1262 (`RadioService`); Linux los maneja por rpmsg |
| `i2c0` deshabilitado            | Cedido al MCU RISC-V — sensores BME280 + ICM-42670 (`SensorService`); leídos por rpmsg |
| `uart3/4/5`, varios `pwm*` deshabilitados | Liberan los pines que reusan los GPIOs de control / ANT_SW de los radios |

> El traspaso de SPI/I²C/GPIO al MCU se detalla en
> [`../architecture/peripheral-ownership.md`](../architecture/peripheral-ownership.md).

## Formato de Imagen FIT

El kernel se empaqueta como una imagen **FIT (Flattened Image Tree)** (`boot.img`) que contiene tres componentes:

```
boot.img (FIT)
├── kernel    — Imagen del kernel comprimida (zImage → Image)
├── fdt       — Device tree blob (rv1106g-sdk.dtb)
└── resource  — Recurso multipack de Rockchip (logo, logo del kernel, etc.)
```

La fuente FIT es `dts/boot.its` (copiada al árbol del kernel durante la compilación) con hashes SHA-256 y firma RSA-2048.

## Comandos de Compilación

### Compilación Automatizada (SDK Completo)

```bash
./build.sh kernel
```

O como parte de una compilación completa:

```bash
./build.sh                  # clean + uboot + kernel + rootfs + packages + pack
```

### Pasos de Compilación Manual

```bash
# 1. Configurar entorno
source scripts/00-setup-toolchain.sh

# 2. Configurar kernel
cp configs/kernel/rv1106_minimal_defconfig output/objs_kernel/.config
make O=output/objs_kernel -C src/kernel \
    ARCH=arm \
    CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- \
    olddefconfig

# 3. Compilar kernel, DTB e imagen FIT
make O=output/objs_kernel -C src/kernel \
    ARCH=arm \
    CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- \
    BOOT_ITS=${SDK_DIR}/dts/boot.its \
    rv1106g-sdk.img \
    -j$(nproc)
```

Usando la variable `${SDK_DIR}` (definida en `scripts/functions.sh`):

```bash
source scripts/00-setup-toolchain.sh
source scripts/functions.sh

cp configs/kernel/rv1106_minimal_defconfig "${KERNEL_OBJ_DIR}/.config"
make O="${KERNEL_OBJ_DIR}" -C "${KERNEL_SRC}" \
    ARCH="${ARCH}" \
    CROSS_COMPILE="${CROSS_COMPILE}" \
    olddefconfig

make O="${KERNEL_OBJ_DIR}" -C "${KERNEL_SRC}" \
    ARCH="${ARCH}" \
    CROSS_COMPILE="${CROSS_COMPILE}" \
    BOOT_ITS="${SDK_DIR}/dts/boot.its" \
    rv1106g-sdk.img \
    -j"$(nproc)"
```

### Limpieza

```bash
./build.sh clean
# o manualmente:
make -C src/kernel ARCH=arm CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- distclean
rm -rf output/objs_kernel
```

## Artefactos de Salida

| Archivo                 | Ubicación                           | Descripción                                 |
|-------------------------|-------------------------------------|---------------------------------------------|
| `boot.img`              | `output/images/`                    | Imagen FIT (kernel + fdt + resource)        |
| `boot.img`              | `output/board_bin/`                 | Copia para empaquetado update.img           |
| `rv1106g-sdk.dtb`       | `output/images/`                    | Device tree blob compilado                  |
| `rv1106g-sdk.dtb`       | `output/board_bin/`                 | Copia para empaquetado update.img           |
| `vmlinux`               | `output/board_bin/`                 | ELF sin comprimir (depuración)              |
| `zImage`                | `output/images/`                    | Imagen del kernel comprimida                |
| `resource.img`          | `output/images/`                    | Imagen de recurso Rockchip                  |
| `.config`               | `output/objs_kernel/.config`        | Configuración real del kernel usada         |

## Ubicaciones de Archivos en el Árbol SDK

```
SDK_DIR/
├── configs/
│   └── kernel/
│       └── rv1106_minimal_defconfig    # Defconfig del kernel
├── dts/
│   ├── boot.its                        # Fuente de imagen FIT
│   ├── rv1106g-sdk.dts                 # DTS de la placa (raíz)
│   ├── rv1106.dtsi                     # DTSI del SoC
│   ├── rv1106-sdk-ipc.dtsi             # DTSI de periféricos IPC
│   ├── rv1106-evb.dtsi                 # DTSI de EVB
│   └── rv1106-amp.dtsi                 # DTSI de AMP
├── src/
│   └── kernel/                         # Árbol fuente Linux 5.10.160
├── output/
│   ├── objs_kernel/                    # Directorio de objetos/compilación del kernel
│   ├── images/                         # Imágenes arrancables
│   └── board_bin/                      # Binarios de placa para update.img
├── scripts/
│   ├── 02-build-kernel.sh              # Script de compilación del kernel
│   └── functions.sh                    # Variables compartidas (SDK_DIR, CROSS_COMPILE, etc.)
├── toolchain/
│   └── arm-rockchip830-linux-uclibcgnueabihf/  # Toolchain
└── build.sh                            # Punto de entrada del SDK

[English](kernel.en.md)
