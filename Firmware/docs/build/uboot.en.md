[Español](uboot.es.md)

# U-Boot for RV1106 SDK

## Boot Flow

```
BootROM
  └─► DDR init (rkbin binary)
        └─► SPL (Secondary Program Loader)
              └─► Trust/TEE (OP-TEE)
                    └─► U-Boot proper
                          └─► Kernel (FIT image)
```

BootROM is mask ROM inside the RV1106. It reads the **IDBlock** (DDR init + USB plug + SPL) from boot media, initialises DDR, loads SPL into SRAM, then SPL loads Trust (OP-TEE) and U-Boot proper into DRAM.

## Build

### make.sh

U-Boot is built using Rockchip's `make.sh` wrapper in `src/u-boot/`. It wraps `make`, then calls `rkbin` tools (`boot_merger`, `trust_merger`, `loaderimage`) to produce the final images.

### --spl-new

`make.sh --spl-new` tells the build system to use the **newly compiled SPL** (instead of a prebuilt one from `rkbin`). Without this flag, `make.sh` falls back to the prebuilt SPL binary in `rkbin/bin/`.

```bash
cd src/u-boot && ./make.sh --spl-new CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
```

### boot_merger INI Format

`boot_merger` is a Rockchip tool that packs DDR init, USB plug, and SPL into a single IDBlock image. It takes an INI file describing the input binaries and output paths.

**`RV1106MINIALL.ini`** (from `src/rkbin/RKBOOT/`):

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

Sections:
- `[CHIP_NAME]` — SoC name.
- `[VERSION]` — version number embedded in the output.
- `[CODE471_OPTION]` — DDR init binary (loaded by BootROM).
- `[CODE472_OPTION]` — USB plug binary (Rockchip USB download mode).
- `[LOADER_OPTION]` — FlashData (DDR init for loader) and FlashBoot (SPL).
- `[OUTPUT]` — `PATH` = `download.bin` (full loader), `IDB_PATH` = `idblock.img` (aligned IDBlock).
- `[FLAG]` — RC4 encryption flags; `CREATE_IDB=true` generates the IDBlock.

### SPL Boot Order Override

The SPL probes boot devices in the order specified by the `u-boot,spl-boot-order` property in `arch/arm/dts/rv1106-u-boot.dtsi`. The default is:

```
u-boot,spl-boot-order = &sdmmc, &spi_nor, &spi_nand, &emmc;
```

When `BOOT_MEDIUM=spi_nand`, the build script (`01-build-uboot.sh`) patches the DTSI to move `&spi_nand` to the front:

```
u-boot,spl-boot-order = &spi_nand, &emmc;
```

This skips the MMC timeout (sdmmc) on boards that have no SD card, speeding up boot by several seconds. The original DTSI is backed up and restored after the build.

## Partition Layout (SPI NAND — 256 MB)

`mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)`

| Partition | Offset | Size   | Description              |
|-----------|--------|--------|--------------------------|
| env       | 0      | 256K   | U-Boot environment       |
| idblock   | 256K   | 512K   | DDR init + SPL loader    |
| uboot     | 768K   | 256K   | U-Boot proper            |
| boot      | 1024K  | 32M    | Kernel FIT image         |
| rootfs    | —      | rest   | Root filesystem          |

The `idblock` partition is the combined output of `boot_merger`. `uboot.img` is written to the `uboot` partition. The `boot` partition stores the kernel FIT (flattened image tree with kernel + DTB).

## Output Files

All generated images land in `output/images/`:

| File            | Source                                                                    | Description                         |
|-----------------|---------------------------------------------------------------------------|-------------------------------------|
| `idblock.img`   | `boot_merger` output, normalised from `rv1106_idblock_*.img`              | IDBlock: DDR init + SPL (1KB-aligned) |
| `download.bin`  | `boot_merger` output, normalised from `rv1106_download_*.bin`             | Full raw loader for Rockchip tools  |
| `uboot.img`     | U-Boot proper (u-boot FIT image with ITB padding)                         | U-Boot binary for `uboot` partition |
| `trust.img`     | `trust_merger` output, or `tee.bin` fallback                              | OP-TEE + HPMCU trusted firmware     |

The build script normalises the Rockchip-versioned filenames (e.g. `rv1106_idblock_v1.15.102.img`, `rv1106_download_v1.15.108.bin`) to canonical names (`idblock.img`, `download.bin`) for consistency across builds. `trust.img` is produced by `trust_merger`; if only `tee.bin` is available (no trust_merger INI), it is used directly as `trust.img`.
