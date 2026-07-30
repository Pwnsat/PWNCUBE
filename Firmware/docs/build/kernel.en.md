# RV1106 SDK Kernel

[Español](kernel.es.md)

## Kernel Version

| Parameter          | Value                                    |
|--------------------|------------------------------------------|
| Linux version      | 5.10.160                                 |
| Architecture       | ARM32 (ARMv7-A)                          |
| CPU core           | Cortex-A7 (single-core)                  |
| Defconfig          | `rv1106_minimal_defconfig`               |
| Toolchain          | `arm-rockchip830-linux-uclibcgnueabihf-` |
| Cross-prefix       | `arm-rockchip830-linux-uclibcgnueabihf-` |
| Compiler flags     | `-march=armv7-a -mfloat-abi=hard -mfpu=neon` |

## Defconfig Strategy

The defconfig (`configs/kernel/rv1106_minimal_defconfig` → `rv1106_minimal_defconfig`) strips all non-essential subsystems to produce the smallest possible kernel for SPI NAND-based IPC cameras. The following are explicitly disabled:

| Subsystem            | Config                                  |
|----------------------|-----------------------------------------|
| MMC/SD               | `# CONFIG_MMC is not set`               |
| IPV6                 | `# CONFIG_IPV6 is not set`              |
| Netfilter            | `# CONFIG_NETFILTER is not set`         |
| UBI/UBIFS            | `# CONFIG_MTD_UBI is not set`, `# CONFIG_UBIFS_FS is not set` |
| SQUASHFS             | `# CONFIG_SQUASHFS is not set`          |
| Media subsystem      | `# CONFIG_MEDIA_SUPPORT is not set`     |
| NPU                  | `# CONFIG_ROCKCHIP_RKNPU is not set`    |
| Sound                | `# CONFIG_SOUND is not set`             |
| DRM                  | `# CONFIG_DRM is not set`               |
| Wireless             | `# CONFIG_WLAN is not set`              |
| DEBUG_FS             | `# CONFIG_DEBUG_FS is not set`          |
| earlycon             | (removed from bootargs in DTS)          |
| Staging drivers      | `# CONFIG_STAGING is not set`           |
| WireGuard            | `# CONFIG_WIREGUARD is not set`         |
| SCSI                 | `# CONFIG_SCSI is not set`              |
| USB_SERIAL           | `# CONFIG_USB_SERIAL_CH343 is not set`  |

SPI NAND (`CONFIG_MTD_SPI_NAND=y`) is the only flash medium.

## Silent Boot

The kernel command line includes `loglevel=3`, which suppresses messages below KERN_WARNING:

```text
CONFIG_CMDLINE="user_debug=31 loglevel=3"
```

Combined with `console=ttyFIQ0` (FIQ debugger as console), this produces a clean, near-silent boot with only warning-level and above messages visible.

## Device Tree Hierarchy

The final DTS is assembled from three layers:

```
rv1106g-sdk.dts
├── rv1106.dtsi           — SoC base (CPU, clocks, pinctrl, etc.)
├── rv1106-evb.dtsi       — EVB board peripherals
└── rv1106-sdk-ipc.dtsi   — IPC board specifics (CSI, audio, regulators)
    └── rv1106-amp.dtsi   — AMP (asymmetric multiprocessing) definitions
```

### Key DTS Changes vs. Stock Rockchip

| Change                          | Detail                                                      |
|---------------------------------|-------------------------------------------------------------|
| No `earlycon`                  | Removed from `bootargs` in `rv1106g-sdk.dts`                |
| `sdmmc` disabled               | `&sdmmc { status = "disabled"; }`                          |
| Console                        | `bootargs = "console=ttyFIQ0"` (no `earlycon` prefix)      |
| `vdd_arm` regulator            | `regulator-min-microvolt = <900000>; regulator-max-microvolt = <900000>;` — fixed to 0.9 V |
| SPI NAND                       | `&sfc` enabled at 75 MHz, quad RX                           |
| Ethernet                       | `&gmac` enabled                                             |
| USB                            | OTG enabled in peripheral mode                              |
| Audio codec                    | `&acodec` with PA control GPIO                              |
| CSI cameras                    | Dual sensor (SC3336 + MIS5001) via MIPI CSI-2               |
| `spi0` / `spi1` disabled        | Ceded to the RISC-V MCU — the two SX1262 radios (`RadioService`); Linux drives them over rpmsg |
| `i2c0` disabled                 | Ceded to the RISC-V MCU — BME280 + ICM-42670 sensors (`SensorService`); read over rpmsg |
| `uart3/4/5`, several `pwm*` disabled | Free the pins reused by the radios' control GPIOs / ANT_SW |

> The SPI/I²C/GPIO hand-off to the MCU is detailed in
> [`../architecture/peripheral-ownership.md`](../architecture/peripheral-ownership.md).

## FIT Image Format

The kernel is packaged as a **FIT (Flattened Image Tree)** image (`boot.img`) containing three components:

```
boot.img (FIT)
├── kernel    — compressed kernel image (zImage → Image)
├── fdt       — device tree blob (rv1106g-sdk.dtb)
└── resource  — Rockchip multipack resource (logo, kernel logo, etc.)
```

The FIT source is `dts/boot.its` (copied into the kernel tree during build) with SHA-256 hashes and RSA-2048 signing.

## Build Commands

### Automated Build (Full SDK)

```bash
./build.sh kernel
```

Or as part of a full build:

```bash
./build.sh                  # clean + uboot + kernel + rootfs + packages + pack
```

### Manual Build Steps

```bash
# 1. Source environment
source scripts/00-setup-toolchain.sh

# 2. Configure kernel
cp configs/kernel/rv1106_minimal_defconfig output/objs_kernel/.config
make O=output/objs_kernel -C src/kernel \
    ARCH=arm \
    CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- \
    olddefconfig

# 3. Build kernel, DTB and FIT image
make O=output/objs_kernel -C src/kernel \
    ARCH=arm \
    CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- \
    BOOT_ITS=${SDK_DIR}/dts/boot.its \
    rv1106g-sdk.img \
    -j$(nproc)
```

Using `${SDK_DIR}` variable (defined in `scripts/functions.sh`):

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

### Cleaning

```bash
./build.sh clean
# or manually:
make -C src/kernel ARCH=arm CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- distclean
rm -rf output/objs_kernel
```

## Output Artifacts

| File                    | Location                          | Description                               |
|-------------------------|-----------------------------------|-------------------------------------------|
| `boot.img`              | `output/images/`                  | FIT image (kernel + fdt + resource)       |
| `boot.img`              | `output/board_bin/`               | Copy for update.img packing               |
| `rv1106g-sdk.dtb`       | `output/images/`                  | Compiled device tree blob                 |
| `rv1106g-sdk.dtb`       | `output/board_bin/`               | Copy for update.img packing               |
| `vmlinux`               | `output/board_bin/`               | Uncompressed ELF (debug)                  |
| `zImage`                | `output/images/`                  | Compressed kernel image                   |
| `resource.img`          | `output/images/`                  | Rockchip resource image                   |
| `.config`               | `output/objs_kernel/.config`      | Actual kernel config used                 |

## File Locations in SDK Tree

```
SDK_DIR/
├── configs/
│   └── kernel/
│       └── rv1106_minimal_defconfig    # Kernel defconfig
├── dts/
│   ├── boot.its                        # FIT image source
│   ├── rv1106g-sdk.dts                 # Board DTS (top)
│   ├── rv1106.dtsi                     # SoC DTSI
│   ├── rv1106-sdk-ipc.dtsi             # IPC peripheral DTSI
│   ├── rv1106-evb.dtsi                 # EVB DTSI
│   └── rv1106-amp.dtsi                 # AMP DTSI
├── src/
│   └── kernel/                         # Linux 5.10.160 source tree
├── output/
│   ├── objs_kernel/                    # Kernel object/ build directory
│   ├── images/                         # Bootable images
│   └── board_bin/                      # Board binaries for update.img
├── scripts/
│   ├── 02-build-kernel.sh              # Kernel build script
│   └── functions.sh                    # Shared vars (SDK_DIR, CROSS_COMPILE, etc.)
├── toolchain/
│   └── arm-rockchip830-linux-uclibcgnueabihf/  # Toolchain
└── build.sh                            # SDK entry point
```

[Español](kernel.es.md)
