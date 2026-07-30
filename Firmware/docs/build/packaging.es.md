# Guía de Empaquetado de Firmware Rockchip

[English](packaging.en.md)

## Resumen

El firmware del RV1106 se empaqueta como una imagen `update.img` de Rockchip.
El proceso es:

```
afptool -pack  →  rkImageMaker -RK1106  →  update.img
```

`afptool` ensambla las imágenes de partición individuales en un blob de
firmware, luego `rkImageMaker` antepone el encabezado Rockchip y el bootloader
para producir la imagen final flasheable.

## Compilar y actualizar (build → pack → flash)

Actualizar es **compilar → empaquetar → cargar**. Puedes recompilar un solo
componente o todo; la imagen que se flashea es siempre `update.img`.

### 1. Componentes individuales

```bash
source scripts/00-setup-toolchain.sh   # una vez por terminal

./build.sh mcu       # firmware RISC-V del MCU (rtthread.bin)
./build.sh uboot     # U-Boot (idblock, uboot, trust)
./build.sh kernel    # kernel + DTB + boot.img
./build.sh rootfs    # busybox + paquetes + rootfs
```

> ⚠️ **Orden que importa:** `uboot` **embebe** `rtthread.bin` dentro de
> `uboot.img`/`trust.img`. Si tocas el MCU, corre **siempre** `./build.sh mcu`
> **antes** de `./build.sh uboot`, o la placa arrancará con el MCU viejo. Los
> paquetes se compilan tras el kernel; si editas un paquete, reconstrúyelo con
> `./pkg/pkg.sh build-all` antes de `rootfs`.

### 2. Todo junto

```bash
./build.sh            # orden correcto: mcu → uboot → kernel → paquetes → rootfs → pack
./build.sh rebuild    # igual pero con clean previo
```

### 3. Juntarlo en la imagen final

```bash
./build.sh pack       # combina output/images/*.img en update.img
```

`./build.sh` (completo) ya hace este paso al final; solo necesitas `pack`
suelto si recompilaste un componente individual y quieres reempaquetar sin
rebuild completo. Luego flashea (ver [Flasheo](#flasheo)).

## Diseño de Particiones

Definido en `configs/board/rv1106-sdk.mk` mediante `RK_PARTITION_CMD_IN_ENV`:

```
mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)
```

| Partición | Offset    | Tamaño | Descripción                     |
|-----------|-----------|--------|----------------------------------|
| env       | 0         | 256K   | Entorno U-Boot (MTD)             |
| idblock   | 256K      | 512K   | IDB (loader)                     |
| uboot     | 768K      | 256K   | U-Boot                           |
| boot      | 1024K     | 32M    | Kernel + DTB + initramfs         |
| rootfs    | a continuación | resto | Sistema de archivos raíz       |

> **Nota:** La partición `env` debe ser de **256K** para coincidir con
> `CONFIG_ENV_NAND_SIZE` en la configuración de U-Boot.

## Referencia de Herramientas

| Herramienta    | Ubicación                     | Propósito                                |
|----------------|-------------------------------|------------------------------------------|
| `rkImageMaker` | `tools/rkImageMaker`          | Envuelve el blob con el bootloader       |
| `afptool`      | `tools/afptool`               | Empaqueta / desempaqueta particiones     |
| `mkenvimage`   | `tools/mkenvimage`            | Genera env.img desde env.txt             |
| `upgrade_tool` | `tools/upgrade_tool`          | Flashea update.img al dispositivo        |
| `boot_merger`  | `tools/boot_merger`           | Fusiona U-Boot + TEE en download.bin     |
| `loaderimage`  | `tools/loaderimage`           | Convierte imágenes a formato loader      |

## Creación de env.img

La imagen de entorno se crea desde un archivo `.env.txt` usando `mkenvimage`:

```bash
tools/mkenvimage -s 262144 -p 0x0 -o output/images/env.img output/images/.env.txt
```

- `-s 262144` — tamaño de la partición env en bytes (256K)
- `-p 0x0` — byte de relleno (0x00)
- Formato de entrada: líneas `clave=valor`
- La cadena de partición y `sys_bootargs` son escritas por
  `scripts/04-pack-image.sh`

Ejemplo de `.env.txt`:

```
mtdparts=spi-nand0:256K(env),512K@256K(idblock),256K@768K(uboot),32M@1024K(boot),-(rootfs)
sys_bootargs=root=/dev/mtdblock4 rootfstype=ext4
```

## Flasheo

### Actualización completa

```bash
sudo tools/upgrade_tool UF output/images/update.img
```

### Particiones individuales

```bash
sudo tools/upgrade_tool DI -env output/images/env.img
sudo tools/upgrade_tool DI -boot output/images/boot.img
sudo tools/upgrade_tool DI -rootfs output/images/rootfs.img
sudo tools/upgrade_tool DI -idblock output/images/idblock.img
```

### Modo Boot ROM (Maskrom)

Secuencia con los botones de la placa:

1. **Mantén presionado el botón `BOOT`.**
2. Sin soltar `BOOT`, **presiona y suelta `RST` (reset)**.
3. Sigue sosteniendo `BOOT` ~**5 s** hasta que el host detecte el dispositivo
   en modo maskrom; entonces suelta `BOOT`.

Con la placa ya en Linux puedes entrar sin tocar botones: `reboot loader`
desde el shell serie.

**¿Cómo sé que está en maskrom?** Verifícalo desde el host:

```bash
sudo tools/upgrade_tool LD        # lista los dispositivos Rockchip conectados
# Maskrom OK →  DevNo=1  Vid=0x2207,Pid=0x350a,...  Mode=Maskrom

lsusb | grep 2207                 # alternativa: Rockchip = VID 0x2207
# 2207:350a → maskrom;  2207:110a → ya en modo Loader (U-Boot)
```

`Mode=Maskrom` (o `Loader`) es la única confirmación fiable. Si `LD` no lista
nada o no aparece el `2207:xxxx`, la placa **no** entró: repite la secuencia
del botón. Luego flashea como arriba.

## Estructura de Archivos del SDK

```
configs/board/rv1106-sdk.mk     — diseño de particiones y configuración
scripts/04-pack-image.sh        — script de empaquetado
tools/                          — herramientas (rkImageMaker, afptool, etc.)
output/images/                  — imágenes de partición y update.img
```

## Solución de Problemas

### bad CRC

La CRC de la partición de entorno es inválida — probablemente la partición env
no fue formateada o el tamaño es incorrecto. Re-flashear `env.img` o borrar
env:

```bash
sudo tools/upgrade_tool EF output/images/update.img
```

### MMC timeout / Sin respuesta

La placa no está en modo de descarga. Verificar:
- Conexión del cable USB
- Alimentación de la placa
- Detección Rockusb (`tools/upgrade_tool LD`)
- Reingresar al modo Maskrom

### mkimage no encontrado

La herramienta `mkimage` falta en el árbol de compilación de U-Boot o en el
PATH del sistema.

```bash
export PATH=$PATH:/ruta/a/u-boot/tools
```

O usar la herramienta precompilada en `tools/mkimage`.

### Dispositivo Rockusb no detectado

```bash
lsusb | grep 2207
```

Los dispositivos Rockchip tienen VID `2207`. Si no aparece nada:
- Verificar permisos del driver (Linux: regla `udev` para `2207`)
- Probar otro puerto USB o cable
- Reingresar al modo Maskrom

---

[English](packaging.en.md)
