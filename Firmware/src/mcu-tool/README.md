# mcutool — hot-load the RISC-V MCU from Linux

`mcutool` resets, loads, and releases the RV1106 SCR1 RISC-V MCU **from Linux
userspace** (`/dev/mem`), using the exact register sequence our U-Boot SPL uses
to release `mcu0` (`src/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c:557`).

Purpose: iterate MCU firmware **without a full reflash** (build → uboot → pack →
`reboot loader` → `upgrade_tool UF`), and recover a wedged MCU without rebooting
the whole board. Adapted from [luyi1888/rv1106-mcu](https://github.com/luyi1888/rv1106-mcu) (GPL-3.0).

## Build / install

Enabled as the `mcu-tool` package (`PKG_ENABLE_mcu-tool=y`), cross-compiled and
installed to `/usr/bin/mcutool`. Needs `CONFIG_DEVMEM=y` (added to the kernel
defconfig) and root on the device.

## Usage (on the device)

```
mcutool execute FILE [ADDR]   # load firmware + run (ADDR default 0x40000, our DDR)
mcutool load    FILE [ADDR]   # load firmware only (stays in reset)
mcutool run                   # release (start) the MCU
mcutool stop                  # reset (halt) the MCU
```

Get the firmware onto the device first (the image is **not** on the rootfs — it
is normally embedded in U-Boot):

```sh
# host: build the MCU, then copy the raw binary to the device
scp src/mcu/output/image/rtthread.bin root@<device>:/tmp/
# device:
mcutool execute /tmp/rtthread.bin      # loads to 0x40000 and starts it
```

## Caveat: rpmsg after a hot-reload (dual-boot)

The MCU is normally released by the SPL with a **live rpmsg link** (virtio0 +
reserved DDR + `/dev/rpmsg*` bound to `rpmsg_chrdev`). Resetting and reloading
tears the MCU side down; the new firmware re-announces its endpoints, but the
Linux char bindings are stale. After `mcutool execute`, re-bind them (same
commands as `rootfs/skeleton/etc/init.d/rcS`):

```sh
bind() { echo rpmsg_chrdev > /sys/bus/rpmsg/devices/$1/driver_override; \
         echo $1 > /sys/bus/rpmsg/drivers/rpmsg_chrdev/bind; }
bind virtio0.rpmsg-radio.-1.16389
bind virtio0.rpmsg-sensor.-1.16390
bind virtio0.rpmsg-telemetry.-1.16391
bind virtio0.rpmsg-command.-1.16392
```

If the virtio/vring state has desynced past recovery, reboot. This tool is a
**dev aid**; for firmware that owns rpmsg it is best-effort. It is flawless for
firmware that does not depend on a live rpmsg link (bring-up, standalone tests).
