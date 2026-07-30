# RV1106 SDK — CubeSat flight-computer firmware

SDK for the Rockchip RV1106, CubeSat flight computer. **Dual-core** boot:
Cortex-A7 (Linux, mission) + RISC-V SCR1 "HPMCU" coprocessor (RT-Thread,
deterministic hardware control). The MCU owns the **2x SX1262 radios (SPI)**,
the **BME280 + ICM-42670 sensors (I²C0)**, and a real **u-blox NEO-6M GPS
receiver (UART0)** — exposing all of it to Linux over rpmsg (`radio_test` /
`sensor_test`). Based on the Luckfox Pico SDK V1.4.

Board: RV1106 SDK (SPI NAND, 256 MB)

> ⚠️ **This is a source repository, not a self-contained SDK archive.** Two
> large, third-party pieces are excluded — see
> **[Getting Started](docs/getting-started.en.md)** for what to download and
> where to place it before your first build.

## Components

| Component | Version |
|-----------|---------|
| Kernel | Linux 5.10.160 |
| U-Boot | 2017.09 + rkbin |
| MCU firmware | RT-Thread (Syntacore SCR1, rv32imc) — `rtthread.bin` |
| Busybox | 1.27.2 |
| Toolchain ARM | GCC 8.3, uClibc, ARMv7-a hard-float (not included, see above) |
| Toolchain RISC-V | xpack riscv-none-embed-gcc 10.2.0 (not included, see above) |
| Rootfs | ext4 (also supports squashfs, ubifs, initramfs) |

## Requirements

Install all host dependencies with **one command**:

```bash
./build.sh deps        # = ./scripts/install-deps.sh (apt, Debian/Ubuntu)
```

Installs: `git make gcc g++ bc cpio rsync fakeroot bison flex libssl-dev
device-tree-compiler scons gawk texinfo cmake unzip gperf autoconf
libncurses5-dev pkg-config python3` (the ARM and RISC-V toolchains are a
separate download, see **[Getting Started](docs/getting-started.en.md)**).
`scons` is required for the MCU firmware (RT-Thread) — without it, that
component is silently skipped.

## Usage

```bash
# Set up the toolchain (once per terminal)
source scripts/00-setup-toolchain.sh

# Full build from scratch
./build.sh

# Full rebuild (clean + build)
./build.sh rebuild

# Help
./build.sh help

# Individual components
./build.sh uboot     # U-Boot only (embeds the MCU's rtthread.bin)
./build.sh kernel    # Kernel only
./build.sh mcu       # RISC-V MCU firmware only (rtthread.bin)
./build.sh rootfs    # Rootfs + packages only
./build.sh pack      # Package update.img only
./build.sh flash     # Flash to the device (sudo)
./build.sh clean     # Clean everything
./build.sh info      # Show configuration

# Package management
./pkg/pkg.sh list                    # Available packages
./pkg/pkg.sh enable dropbear         # Enable SSH
./pkg/pkg.sh build-all               # Build enabled packages
./pkg/pkg.sh menuconfig              # whiptail TUI
```

## Updating the firmware (step by step)

Updating is **build → package → load**. You can rebuild a single component
or everything, but the image you flash is always `update.img`.

### 1. Build each component (individually)

```bash
source scripts/00-setup-toolchain.sh   # once per terminal

./build.sh mcu       # MCU RISC-V firmware (rtthread.bin)
./build.sh uboot     # U-Boot (idblock, uboot, trust)
./build.sh kernel    # kernel + DTB + boot.img
./build.sh rootfs    # busybox + packages + rootfs
```

> ⚠️ **Order matters:** `uboot` **embeds** `rtthread.bin` inside
> `uboot.img`/`trust.img`. If you touch the MCU, **always** run
> `./build.sh mcu` **before** `./build.sh uboot`, or the board will boot with
> the old MCU firmware. Packages are built after the kernel; if you edit a
> package, rebuild it with `./pkg/pkg.sh build-all` before `rootfs`.

### 2. Build everything together

```bash
./build.sh            # correct order: mcu → uboot → kernel → packages → rootfs → pack
./build.sh rebuild    # same, with a clean first
```

### 3. Assemble the final image

```bash
./build.sh pack       # combines every output/images/*.img into update.img
```

`./build.sh` (full) already does this step at the end. You only need a bare
`pack` if you rebuilt a single component and want to re-package without a
full rebuild.

### 4. Enter bootloader mode (maskrom) and flash

On the board:

1. **Hold the `BOOT` button.**
2. Without releasing `BOOT`, **press and release `RST` (reset)**.
3. Keep holding `BOOT` for ~**5 s** until the host detects the device in
   maskrom mode, then release `BOOT`.

**How do I know it's in maskrom?** Check from the host:

```bash
sudo tools/upgrade_tool LD        # lists connected Rockchip devices
# Maskrom OK →  DevNo=1  Vid=0x2207,Pid=0x350a,...  Mode=Maskrom

lsusb | grep 2207                 # alternative: Rockchip = VID 0x2207
# ID 2207:350a  → maskrom;  ID 2207:110a  → already in Loader mode (U-Boot)
```

If `LD` lists nothing or `lsusb` shows no `2207:xxxx`, the board did **not**
enter maskrom: repeat the button sequence. `Mode=Maskrom` (or `Loader`) is
the only reliable confirmation before flashing.

With the board already in Linux, you can enter without touching buttons via
`reboot loader` from the serial shell.

Then, from the host:

```bash
./build.sh flash                              # = upgrade_tool UF output/images/update.img (sudo)
# or directly:
sudo tools/upgrade_tool UF output/images/update.img
```

> ⚠️ Always use **`UF`** (full image). `DI -b` responds "ok" but **does not
> write** on this SPI-NAND. If you flashed using the recovery button,
> **release it as soon as flashing starts** or the subsequent reboot will be
> silent.

Register-level boot detail, dual A7+MCU boot, and every gotcha found along
the way:
[`docs/migration/implementation/90-mcu-config-replication.md`](docs/migration/implementation/90-mcu-config-replication.md)
and
[`docs/migration/implementation/80-dual-boot.md`](docs/migration/implementation/80-dual-boot.md).

## Outputs

After `./build.sh`, in `output/images/`:

| File | Description |
|------|-------------|
| `idblock.img` | Boot loader (DDR init + SPL) |
| `download.bin` | Raw loader for Rockchip tools |
| `uboot.img` | U-Boot proper |
| `trust.img` | OP-TEE + HPMCU |
| `boot.img` | Kernel FIT (Image + DTB) |
| `env.img` | U-Boot environment (partitions + bootargs) |
| `rootfs_base.img` | Rootfs |
| `update.img` | Full flashable image |

## Partition layout (SPI NAND — 256 MB)

```
256K(env), 512K@256K(idblock), 256K@768K(uboot), 32M@1024K(boot), -(rootfs)
```

## Documentation

Full documentation lives in `docs/`. Start with the index
[`docs/README.md`](docs/README.md).

- **Architecture** — `docs/architecture/`: overview, rpmsg IPC, and
  peripheral ownership (what the RISC-V MCU owns vs. Linux).
- **Build** — `docs/build/`: `toolchain`, `uboot`, `kernel`, `rootfs`,
  `packaging`, `pkg-system`.
- **Peripherals** (owned by the MCU, handled over rpmsg) — `docs/peripherals/`:
  `sx1262-radio`, `bme280`, `icm42670`, `gps-neo6m` (real GPS, UART0 — driver
  in `src/mcu/rt-thread/bsp/rockchip/rv1106-mcu/applications/gps_nmea.c`/`.h`,
  queried from Linux with `radio_test gps_status`).
- **Security** — `docs/security/exploitation-guide.md`.
- **RISC-V migration / MCU firmware** (bring-up log, dual boot, register-level
  configuration) — `docs/migration/`.

## License

This SDK integrates code under several different open-source licenses, by
component:

| Component | License |
|-----------|---------|
| PWNSAT-authored application code (`applications/`, `radio-client/`, `pwnsat-console/`, attack-facing tooling) | MIT |
| Linux kernel (`src/kernel/`) | GPL-2.0 |
| U-Boot (`src/u-boot/`) | GPL-2.0 |
| Busybox (`src/busybox/`) | GPL-2.0 |
| Toolchains (not included in this repo — see [Getting Started](docs/getting-started.en.md)) | GPL-3.0 / LGPL-2.1 |
| `rkbin` (not included in this repo — see [Getting Started](docs/getting-started.en.md)) | Proprietary, Rockchip |

Known limitation, documented rather than hidden: **the sensor telemetry
frame's altitude field can overflow its 16-bit fixed-point encoding**
(pressure-derived altitude × 100 exceeds `int16_t` range under normal
atmospheric conditions) — tracked as a known issue, not yet fixed. See
`docs/security/exploitation-guide.en.md` for the full, deliberately
reintroduced vulnerability catalog this platform demonstrates.
