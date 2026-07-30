# Sistema de Archivos Raíz (rootfs)

[English](rootfs.en.md)

## Resumen

```
src/busybox/busybox-1.27.2.tar.bz2
              |
         [extraer + parches]
              |
    config_tiny_arm (compilación estática ARM)
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
                   [rm libs debug]
                        |
              mkfs.ext4 -d output/rootfs/
                        ↓
              output/images/rootfs_base.img
                        |
              pkg/pkg.sh install-all (paquetes opcionales)
```

## Busybox 1.27.2

Configuración: `src/busybox/config_tiny_arm`

- `CONFIG_STATIC=y` — compilación estática (sin dependencias externas de bibliotecas compartidas)
- Shell: `ash` (integrado en Busybox)
- Red: `ping`, `ifconfig`, `route`, `udhcpc`, `telnet`, `wget`, `nc`
- Coreutils: `cp`, `mv`, `ls`, `cat`, `mkdir`, `rm`, `mount`, `ps`, `grep`, `sed`, `vi`, `find`, `tar`, `gzip`, etc.
- Sin `httpd`, sin `ftpd`
- `CONFIG_CROSS_COMPILER_PREFIX="arm-rockchip830-linux-uclibcgnueabihf-"`
- `CONFIG_EXTRA_CFLAGS="-mthumb"`
- `CONFIG_EXTRA_LDFLAGS="-Wl,-z,noexecstack"` — evita la advertencia del kernel `missing exec-shield` al arrancar (marca la pila como no ejecutable, eliminando la advertencia de seguridad del kernel)

Parches aplicados: 0001 al 0010 (correcciones de red, soporte de argumentos halt/reboot, codificación de pantalla, eliminación de stime, etc.)

Busybox se compila en `output/busybox-1.27.2/`; `_install/` se copia al rootfs staging.

## Estructura del skeleton

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
├── dev/          (vacío — lo pobla devtmpfs)
├── lib/          (bibliotecas runtime del toolchain)
├── proc/         (punto de montaje)
├── sys/          (punto de montaje)
├── tmp/          (punto de montaje)
└── var/          (punto de montaje)
```

### Archivos de configuración principales

| Archivo | Contenido |
|---|---|
| **inittab** | `::sysinit:/etc/init.d/rcS` — `::respawn:-/bin/sh` — `::ctrlaltdel:/sbin/reboot` — `::shutdown:/etc/init.d/rcK` |
| **fstab** | `proc /proc proc defaults 0 0` — `sysfs /sys sysfs defaults 0 0` — `tmpfs /tmp tmpfs defaults 0 0` — `tmpfs /var tmpfs defaults 0 0` |
| **hostname** | `rv1106` |
| **hosts** | localhost, rv1106, entradas IPv6 |
| **passwd** | root (sin contraseña), daemon, bin, sys, sync, mail, nobody |
| **shadow** | root sin contraseña, otras cuentas bloqueadas |
| **profile** | `PATH=/bin:/sbin:/usr/bin:/usr/sbin`, define `PS1`, ejecuta `/etc/profile.d/*.sh` |
| **group** | root, daemon, bin, sys, tty, disk, dialout, audio, video, usb, i2c, gpio, nogroup |

## Secuencia de inicio

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

                    inittab: ::respawn → -/bin/sh (shell de login en consola)

                    inittab: ::ctrlaltdel → /sbin/reboot
                    inittab: ::shutdown    → /etc/init.d/rcK
```

El kernel carga la imagen rootfs, ejecuta `/init` (que monta `devtmpfs`), luego entrega el control a `/sbin/init` (Busybox init). Init lee `/etc/inittab` y ejecuta el script de inicio del sistema y la shell.

## Bibliotecas runtime

Extraídas de `toolchain/arm-rockchip830-linux-uclibcgnueabihf/runtime_lib/lib.tar.bz2` a `output/rootfs/lib/`.

Las bibliotecas debug/sanitizer se eliminan después de la extracción:

```bash
for dbg_lib in libasan libtsan libubsan liblsan libhwasan libgcov; do
    find output/rootfs/lib -name "${dbg_lib}*" -type f -delete
done
```

Después del strip (mediante `arm-rockchip830-linux-uclibcgnueabihf-strip --strip-unneeded`), el rootfs suele ocupar ~8–12 MB para un sistema base.

## Creación de imagen ext4

La imagen rootfs se crea directamente desde el directorio staging — sin loopback, sin necesidad de sudo:

```bash
mkfs.ext4 -F -L "rootfs" -d output/rootfs/ output/images/rootfs_base.img 64M
```

- `-F` — sobrescritura forzada
- `-L "rootfs"` — etiqueta del sistema de archivos
- `-d output/rootfs/` — poblar el sistema de archivos desde este directorio
- `64M` — tamaño de imagen (por defecto, se ajusta automáticamente al contenido + 25% + 8 MB de margen, con un mínimo de 64 MB)

El script `rootfs/mkfs/mkfs_ext4.sh` también está disponible para uso independiente con post-procesamiento adicional (resize2fs, e2fsck, tune2fs).

Salida: `output/images/rootfs_base.img` → enlace simbólico `output/images/rootfs.img`

## Instalación de paquetes

Después de crear la imagen base, los paquetes opcionales se instalan mediante `pkg/pkg.sh install-all`. Los paquetes se definen en `pkg/available/` y se controlan con `pkg/package-config`. Consulte [el sistema de paquetes](pkg-system.es.md) para más detalles.

## Variables de entorno

| Variable | Valor por defecto | Descripción |
|---|---|---|
| `CROSS_COMPILE` | `arm-rockchip830-linux-uclibcgnueabihf-` | Prefijo del toolchain |
| `ARCH` | `arm` | Arquitectura objetivo |
| `TOOLCHAIN_DIR` | `toolchain/arm-rockchip830-linux-uclibcgnueabihf` | Ruta del toolchain |
| `BUSYBOX_DIR` | `src/busybox` | Directorio fuente de Busybox |
| `SKELETON_DIR` | `rootfs/skeleton` | Directorio skeleton del rootfs |
| `INIT_SCRIPT` | `rootfs/init-scripts/init` | Archivo fuente de /init |
| `ROOTFS_DIR` | `output/rootfs` | Directorio staging del rootfs |
| `ROOTFS_IMG` | `output/images/rootfs_base.img` | Ruta de la imagen de salida |
| `IMAGE_SIZE_MB` | `64` | Tamaño mínimo de imagen en MB |
| `PKG_SCRIPT` | `pkg/pkg.sh` | Script del gestor de paquetes |

[English](rootfs.en.md)
