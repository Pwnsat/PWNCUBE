# Root Filesystem (rootfs)

[Español](rootfs.es.md)

## Overview

```
src/busybox/busybox-1.27.2.tar.bz2
              |
         [extract + patches]
              |
    config_tiny_arm (static ARM build)
              |
         make oldconfig
         make -j$(nproc)
         make install
              |
        _install/ ─────┐
                        │
rootfs/skeleton/ ───────┤
rootfs/init-scripts/ ───┤──→ output/rootfs/ (staging)
                        │
toolchain/.../lib.tar.bz2
                        │
                   [strip ELF]
                   [rm debug libs]
                        |
              mkfs.ext4 -d output/rootfs/
                        ↓
              output/images/rootfs_base.img
                        |
              pkg/pkg.sh install-all (optional packages)
```

## Busybox 1.27.2

Configuration: `src/busybox/config_tiny_arm`

- `CONFIG_STATIC=y` — static build (no external shared library dependencies)
- Shell: `ash` (Busybox built-in)
- Networking: `ping`, `ifconfig`, `route`, `udhcpc`, `telnet`, `wget`, `nc`
- Coreutils: `cp`, `mv`, `ls`, `cat`, `mkdir`, `rm`, `mount`, `ps`, `grep`, `sed`, `vi`, `find`, `tar`, `gzip`, etc.
- No `httpd`, no `ftpd`
- `CONFIG_CROSS_COMPILER_PREFIX="arm-rockchip830-linux-uclibcgnueabihf-"`
- `CONFIG_EXTRA_CFLAGS="-mthumb"`
- `CONFIG_EXTRA_LDFLAGS="-Wl,-z,noexecstack"` — avoids the `missing exec-shield` kernel warning on boot (`requires: CONFIG_PAX_NOEXEC_STACK` / `CONFIG_ARM_EXEC_STACK` equivalent — this marks the stack as non-executable and suppresses the soft lockup / security warning the kernel emits when an ELF binary has an executable stack)

Patches applied: 0001 through 0010 (networking patches, halt/reboot argument support, display encoding, stime removal, etc.)

Busybox is built in `output/busybox-1.27.2/`; `_install/` is copied into the staging rootfs.

## Skeleton structure

```
rootfs/skeleton/
├── etc/
│   ├── fstab
│   ├── group
│   ├── hostname
│   ├── hosts
│   ├── init.d/
│   │   ├── rcK
│   │   └── rcS
│   ├── inittab
│   ├── network/
│   │   └── interfaces
│   ├── nsswitch.conf
│   ├── passwd
│   ├── profile
│   ├── profile.d/
│   │   └── umask.sh
│   ├── protocols
│   ├── resolv.conf
│   ├── services
│   └── shadow
├── dev/          (empty — populated by devtmpfs)
├── lib/          (runtime libraries from toolchain)
├── proc/         (mount point)
├── sys/          (mount point)
├── tmp/          (mount point)
└── var/          (mount point)
```

### Key config files

| File | Content |
|---|---|
| **inittab** | `::sysinit:/etc/init.d/rcS` — `::respawn:-/bin/sh` — `::ctrlaltdel:/sbin/reboot` — `::shutdown:/etc/init.d/rcK` |
| **fstab** | `proc /proc proc defaults 0 0` — `sysfs /sys sysfs defaults 0 0` — `tmpfs /tmp tmpfs defaults 0 0` — `tmpfs /var tmpfs defaults 0 0` |
| **hostname** | `rv1106` |
| **hosts** | localhost, rv1106, IPv6 entries |
| **passwd** | root (no password), daemon, bin, sys, sync, mail, nobody |
| **shadow** | root blank password, other accounts locked |
| **profile** | `PATH=/bin:/sbin:/usr/bin:/usr/sbin`, sets `PS1`, sources `/etc/profile.d/*.sh` |
| **group** | root, daemon, bin, sys, tty, disk, dialout, audio, video, usb, i2c, gpio, nogroup |

## Init sequence

```
Kernel → /init
           │
           ├─ mount devtmpfs /dev
           └─ exec /sbin/init
                    │
                    └─ inittab: ::sysinit → /etc/init.d/rcS
                                            │
                                            ├─ mount -t proc  proc  /proc
                                            ├─ mount -t sysfs sysfs /sys
                                            ├─ mount -t tmpfs tmpfs /tmp
                                            ├─ mount -t tmpfs tmpfs /var
                                            ├─ mdev -s
                                            └─ hostname -F /etc/hostname
                                    
                    inittab: ::respawn → -/bin/sh (login shell on console)

                    inittab: ::ctrlaltdel → /sbin/reboot
                    inittab: ::shutdown    → /etc/init.d/rcK
```

The kernel loads the rootfs image, executes `/init` (which mounts `devtmpfs`), then hands off to `/sbin/init` (Busybox init). Init reads `/etc/inittab` and spawns the system init script and shell.

## Runtime libraries

Extracted from `toolchain/arm-rockchip830-linux-uclibcgnueabihf/runtime_lib/lib.tar.bz2` into `output/rootfs/lib/`.

Debug/sanitizer libraries are removed after extraction:

```bash
for dbg_lib in libasan libtsan libubsan liblsan libhwasan libgcov; do
    find output/rootfs/lib -name "${dbg_lib}*" -type f -delete
done
```

After stripping (via `arm-rockchip830-linux-uclibcgnueabihf-strip --strip-unneeded`), the rootfs is typically ~8–12 MB for a base system.

## ext4 image creation

The rootfs image is created directly from the staging directory — no loopback mount, no sudo required:

```bash
mkfs.ext4 -F -L "rootfs" -d output/rootfs/ output/images/rootfs_base.img 64M
```

- `-F` — force overwrite
- `-L "rootfs"` — filesystem label
- `-d output/rootfs/` — populate the filesystem from this directory
- `64M` — image size (default, auto-scaled to contents + 25% + 8 MB padding with a minimum of 64 MB)

The script `rootfs/mkfs/mkfs_ext4.sh` is also available for standalone use with additional post-processing (resize2fs, e2fsck, tune2fs).

Output: `output/images/rootfs_base.img` → symlinked as `output/images/rootfs.img`

## Package installation

After the base image is created, optional packages are installed via `pkg/pkg.sh install-all`. Packages are defined in `pkg/available/` and controlled by `pkg/package-config`. See [the package system](pkg-system.en.md) for details.

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `CROSS_COMPILE` | `arm-rockchip830-linux-uclibcgnueabihf-` | Toolchain prefix |
| `ARCH` | `arm` | Target architecture |
| `TOOLCHAIN_DIR` | `toolchain/arm-rockchip830-linux-uclibcgnueabihf` | Toolchain path |
| `BUSYBOX_DIR` | `src/busybox` | Busybox source directory |
| `SKELETON_DIR` | `rootfs/skeleton` | Rootfs skeleton directory |
| `INIT_SCRIPT` | `rootfs/init-scripts/init` | /init script source |
| `ROOTFS_DIR` | `output/rootfs` | Staging rootfs directory |
| `ROOTFS_IMG` | `output/images/rootfs_base.img` | Output image path |
| `IMAGE_SIZE_MB` | `64` | Minimum image size in MB |
| `PKG_SCRIPT` | `pkg/pkg.sh` | Package manager script |

[Español](rootfs.es.md)
