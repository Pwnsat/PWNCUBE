# Rockchip Firmware Packaging Guide

[Español](packaging.es.md)

## Overview

The RV1106 firmware is packaged as a `update.img` Rockchip update image. The
pipeline is:

```
afptool -pack  →  rkImageMaker -RK1106  →  update.img
```

`afptool` assembles individual partition images into a firmware blob, then
`rkImageMaker` prepends the Rockchip header and bootloader to produce the
final flashable image.

## Building and updating (build → pack → flash)

Updating is **build → pack → load**. You can rebuild a single component or the
whole tree; the image that gets flashed is always `update.img`.

### 1. Individual components

```bash
source scripts/00-setup-toolchain.sh   # once per terminal

./build.sh mcu       # RISC-V MCU firmware (rtthread.bin)
./build.sh uboot     # U-Boot (idblock, uboot, trust)
./build.sh kernel    # kernel + DTB + boot.img
./build.sh rootfs    # busybox + packages + rootfs
```

> ⚠️ **Order matters:** `uboot` **embeds** `rtthread.bin` inside
> `uboot.img`/`trust.img`. If you touch the MCU, **always** run `./build.sh mcu`
> **before** `./build.sh uboot`, or the board boots the stale MCU. Packages are
> built after the kernel; if you edit a package, rebuild it with
> `./pkg/pkg.sh build-all` before `rootfs`.

### 2. All together

```bash
./build.sh            # correct order: mcu → uboot → kernel → packages → rootfs → pack
./build.sh rebuild    # same, with a clean first
```

### 3. Assemble the final image

```bash
./build.sh pack       # combines output/images/*.img into update.img
```

A full `./build.sh` already does this at the end; you only need a standalone
`pack` if you rebuilt a single component and want to repackage without a full
rebuild. Then flash (see [Flashing](#flashing)).

## Partition Layout

Defined in `configs/board/rv1106-sdk.mk` via `RK_PARTITION_CMD_IN_ENV`:

```
mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)
```

| Partition | Offset    | Size   | Description                    |
|-----------|-----------|--------|--------------------------------|
| env       | 0         | 256K   | U-Boot environment (MTD)       |
| idblock   | 256K      | 512K   | IDB (loader)                   |
| uboot     | 768K      | 256K   | U-Boot proper                  |
| boot      | 1024K     | 32M    | Kernel + DTB + initramfs       |
| rootfs    | follow-on | rest   | Root filesystem                |

> **Note:** The `env` partition must be **256K** to match
> `CONFIG_ENV_NAND_SIZE` in the U-Boot configuration.

## Tool Reference

| Tool           | Location                     | Purpose                               |
|----------------|------------------------------|---------------------------------------|
| `rkImageMaker` | `tools/rkImageMaker`         | Wraps firmware blob with bootloader   |
| `afptool`      | `tools/afptool`              | Packs / unpacks partition images      |
| `mkenvimage`   | `tools/mkenvimage`           | Generates env.img from env.txt        |
| `upgrade_tool` | `tools/upgrade_tool`         | Flashes update.img to device          |
| `boot_merger`  | `tools/boot_merger`          | Merges U-Boot + TEE into download.bin |
| `loaderimage`  | `tools/loaderimage`          | Converts images to loader format      |

## Creating env.img

The environment image is created from a `.env.txt` file using `mkenvimage`:

```bash
tools/mkenvimage -s 262144 -p 0x0 -o output/images/env.img output/images/.env.txt
```

- `-s 262144` — env partition size in bytes (256K)
- `-p 0x0` — padding byte (0x00)
- Input format: `key=value` lines
- The partition string and `sys_bootargs` are written into `.env.txt` by
  `scripts/04-pack-image.sh`

Example `.env.txt`:

```
mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)
sys_bootargs=root=/dev/mtdblock4 rootfstype=ext4
```

## Flashing

### Full update

```bash
sudo tools/upgrade_tool UF output/images/update.img
```

### Individual partitions

```bash
sudo tools/upgrade_tool DI -env output/images/env.img
sudo tools/upgrade_tool DI -boot output/images/boot.img
sudo tools/upgrade_tool DI -rootfs output/images/rootfs.img
sudo tools/upgrade_tool DI -idblock output/images/idblock.img
```

### Boot ROM (Maskrom) mode

Board button sequence:

1. **Hold the `BOOT` button down.**
2. Without releasing `BOOT`, **press and release `RST` (reset)**.
3. Keep holding `BOOT` for ~**5 s** until the host detects the device in
   maskrom mode; then release `BOOT`.

With the board already in Linux you can enter without touching buttons:
`reboot loader` from the serial shell.

**How do I know it's in maskrom?** Verify from the host:

```bash
sudo tools/upgrade_tool LD        # lists connected Rockchip devices
# Maskrom OK →  DevNo=1  Vid=0x2207,Pid=0x350a,...  Mode=Maskrom

lsusb | grep 2207                 # alternative: Rockchip = VID 0x2207
# 2207:350a → maskrom;  2207:110a → already in Loader (U-Boot) mode
```

`Mode=Maskrom` (or `Loader`) is the only reliable confirmation. If `LD` lists
nothing or no `2207:xxxx` shows up, the board did **not** enter maskrom: repeat
the button sequence. Then flash as above.

## SDK File Layout

```
configs/board/rv1106-sdk.mk     — partition layout & board config
scripts/04-pack-image.sh        — packaging script
tools/                          — host tools (rkImageMaker, afptool, etc.)
output/images/                  — generated partition images & update.img
```

## Troubleshooting

### bad CRC

The environment partition CRC is invalid — likely the env partition was not
formatted or the size is wrong. Re-flash `env.img` or erase env:

```bash
sudo tools/upgrade_tool EF output/images/update.img
```

### MMC timeout / No response

The board is not in download mode. Check:
- USB cable connection
- Board power
- Rockusb detection (`tools/upgrade_tool LD`)
- Try re-entering Maskrom mode

### mkimage not found

The `mkimage` tool is missing from the U-Boot build tree or the host path.

```bash
export PATH=$PATH:/path/to/u-boot/tools
```

Or use the pre-built tool at `tools/mkimage`.

### Rockusb device not detected

```bash
lsusb | grep 2207
```

Rockchip devices have VID `2207`. If nothing appears:
- Check driver permissions (Linux: `udev` rule for `2207`)
- Try a different USB port or cable
- Re-enter Maskrom mode

---

[Español](packaging.es.md)
