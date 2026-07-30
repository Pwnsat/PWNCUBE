# Getting started — build and update the firmware from scratch

[Español](getting-started.es.md)

A tutorial for someone who has **never built this SDK**. From downloading the
repository to running your own image on the board. No prior knowledge of
Rockchip, cross-compiling, or RT-Thread assumed.

> **What you'll build:** a single flashable image, `update.img`, containing
> everything (bootloader, U-Boot, Linux kernel, rootfs, and the RISC-V
> coprocessor firmware). Building = producing that file; updating = loading it
> onto the board.

The SDK derives from the **Luckfox Pico SDK**. The original internal SDK
archive is fully self-contained (both toolchains and `rkbin` versioned
alongside the source) — **this public repository excludes two large,
third-party pieces** to keep it clone-friendly (see "Before you build" below):
the ARM/RISC-V toolchains (~2.6 GB, freely redistributable but too large for
a source repo) and `src/rkbin/` (Rockchip's proprietary boot-stage blobs,
redistribution terms not confirmed for this release). You need both before
building — see step 2.

> **Before you build — two things this repo does not include:**
> - **Toolchains** (ARM `arm-rockchip830-linux-uclibcgnueabihf` + RISC-V
>   `xpack-riscv-none-embed-gcc-10.2.0`): download `toolchain.tar.gz` from
>   this repository's **[Releases](../../../releases)** page and extract it
>   at the repo root (`tar -xzf toolchain.tar.gz -C .`) — it unpacks into
>   `toolchain/`, preserving that folder name.
> - **`rkbin`** (Rockchip's proprietary boot-stage binaries): download
>   `rkbin.tar.gz` from the same **[Releases](../../../releases)** page and
>   extract it into `src/` (`tar -xzf rkbin.tar.gz -C src/`) — it unpacks
>   into `src/rkbin/`. Licensing/redistribution terms for this specific file
>   have not been independently confirmed; see that release's notes for
>   detail before redistributing it further yourself.
>
> Once both are in place, the rest of this guide works exactly as written —
> the build scripts expect them at those exact paths.

---

## 0. What you need

| Requirement | Detail |
|-------------|--------|
| Host OS | **Ubuntu 20.04 or 22.04 x86-64** (native or WSL2/VM). Other Debian distros work; the rest of this guide assumes `apt`. |
| Disk | The repo is ~5 GB; keep **≥ 40 GB free** for build objects. |
| RAM | 4 GB minimum, 8 GB comfortable (the kernel build is the heaviest step). |
| Cable | A **USB-C data cable** between board and PC (for flashing). |
| Privileges | A `sudo`-capable account (to install packages and to flash). |

> You don't need the board to *build* — only to *flash*. Steps 1–4 run with no
> hardware connected.

---

## 1. Install the host dependencies

One command (needs the repo already cloned — see step 2, or clone it first):

```bash
./build.sh deps        # = ./scripts/install-deps.sh (apt, Debian/Ubuntu)
```

Installs every system package needed to build the kernel, U-Boot, and the MCU
firmware. Equivalent to:

```bash
sudo apt-get update
sudo apt-get install -y \
    git make gcc g++ bc cpio rsync fakeroot bison flex \
    libssl-dev device-tree-compiler scons \
    gawk texinfo cmake unzip gperf autoconf \
    libncurses5-dev pkg-config python3
```

What each group is for:

- `git make gcc g++ bc cpio rsync` — build base and rootfs assembly.
- `bison flex libssl-dev device-tree-compiler` — kernel and U-Boot (parser, crypto, `dtc`).
- **`scons`** — **required** for the RISC-V MCU firmware (RT-Thread). Without
  it, the MCU part is silently skipped.
- `python3` — used by the kernel build system.
- The rest (`gawk texinfo cmake unzip gperf autoconf libncurses5-dev pkg-config`)
  — utilities the various sub-builds touch.

> **Note:** you do *not* need `gcc-multilib` or 32-bit libraries. The toolchain
> is a self-contained cross-compiler and the host tools (`upgrade_tool`,
> `afptool`, …) are static x86-64 binaries.

---

## 2. Download the SDK

```bash
git clone <this-repo-url> PWNCUBE-FIRMWARE
cd PWNCUBE-FIRMWARE/Firmware
```

Then fetch the two pieces this repo excludes (see the callout above) and
place them at `toolchain/` and `src/rkbin/` respectively, exactly as named.

---

## 3. Set up the toolchain (once per terminal)

```bash
source scripts/00-setup-toolchain.sh
```

This **installs nothing** — it just exports `CC`, `CROSS_COMPILE`, `PATH`, etc.
pointing at the ARM cross-compiler already in `toolchain/`. It also warns if any
host package (step 1) is missing.

> Must be run with `source` (not `./`), and **in every new terminal** before
> building. Open another tab → run it again.

Quick check it took:

```bash
${CC} --version      # should print arm-rockchip830-... gcc 8.3.0
```

---

## 4. Build the image for the first time

```bash
./build.sh
```

This builds **everything in the right order** and finishes by packing the
image:

```
mcu → uboot → kernel → packages → rootfs → pack
```

The first run takes a while (kernel included): from several minutes to ~half an
hour depending on your CPU. When it finishes you have the final image:

```
output/images/update.img          ← this is what gets flashed
```

> Build stopped or toolchain not set up? The `Toolchain not configured` error
> means you skipped step 3 in this terminal.

To rebuild just one part later, there are per-component commands
(`./build.sh mcu|uboot|kernel|rootfs|pack`). Details in
[build/packaging.md](build/packaging.md).

---

## 5. Connect the board and flash

### 5.1 Put the board in bootloader (maskrom) mode

With the board connected to the PC over USB, using the board buttons:

1. **Hold the `BOOT` button down.**
2. Without releasing `BOOT`, **press and release `RST` (reset)**.
3. Keep holding `BOOT` for ~**5 s**; then release it.

### 5.2 Confirm it entered maskrom

```bash
sudo tools/upgrade_tool LD
# Maskrom OK →  DevNo=1  Vid=0x2207,Pid=0x350a,...  Mode=Maskrom
```

If it lists nothing, the board didn't enter: repeat 5.1. `Mode=Maskrom` is the
only reliable confirmation.

### 5.3 Load the firmware

```bash
sudo ./build.sh flash
# equivalent to:  sudo tools/upgrade_tool UF output/images/update.img
```

> `sudo` is used because raw USB access needs privileges. To flash without
> `sudo`, add a udev rule for the Rockchip VID `2207` (see «Troubleshooting»).

When it finishes, the board reboots into your new image. Done!

---

## 6. Later updates (the normal cycle)

You don't repeat everything. Change something, rebuild **only that part**,
repack, and flash:

```bash
source scripts/00-setup-toolchain.sh   # if you opened a new terminal
./build.sh kernel                      # e.g. you touched the kernel
./build.sh pack                        # repacks update.img
sudo ./build.sh flash                  # maskrom (step 5) + load
```

⚠️ **Order matters:** if you touch the MCU firmware, run `./build.sh mcu`
**before** `./build.sh uboot` — U-Boot **embeds** `rtthread.bin`. The full
per-component build/pack/flash guide is in
[build/packaging.md](build/packaging.md).

---

## 7. Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `Toolchain not configured` | Step 3 missing in this terminal: `source scripts/00-setup-toolchain.sh`. |
| `scons not installed` | `scons` missing (step 1): `sudo apt-get install scons`. |
| MCU didn't update | You rebuilt `uboot` without rebuilding `mcu` first. U-Boot embeds `rtthread.bin`: run `mcu` then `uboot`. |
| `upgrade_tool` doesn't detect the board | Not in maskrom (repeat 5.1), non-data USB cable, or missing `sudo`. Check with `lsusb \| grep 2207`. |
| `Download Boot Fail` / `please check ddr` | Maskrom goes stale after retries. **One maskrom entry = one `UF` attempt**: if it fails, re-enter maskrom (5.1) and flash on the **first** try, not in a loop. |
| Flashes but won't boot | Always use **`UF`** (full image). `DI -b` replies "ok" but **does not write** on this SPI-NAND. `sudo ./build.sh flash` already uses `UF`. |
| Flash without `sudo` | Create `/etc/udev/rules.d/99-rockchip.rules` with:<br>`SUBSYSTEM=="usb", ATTR{idVendor}=="2207", MODE="0666"`<br>then `sudo udevadm control --reload && sudo udevadm trigger`. |

---

## Next

- Image assembly and flashing in detail — [build/packaging.md](build/packaging.md)
- What runs on each core (Linux vs RISC-V MCU) — [architecture/overview.md](architecture/overview.md)
- The toolchain internals — [build/toolchain.md](build/toolchain.md)

---

[Español](getting-started.es.md)
