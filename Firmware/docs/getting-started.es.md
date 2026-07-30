# Primeros pasos — compilar y actualizar el firmware desde cero

[English](getting-started.en.md)

Tutorial para alguien que **nunca ha compilado este SDK**. Desde descargar el
repositorio hasta tener la placa corriendo tu propia imagen. No asume
conocimiento previo de Rockchip, cross-compiling ni RT-Thread.

> **Qué vas a construir:** una sola imagen flasheable, `update.img`, que
> contiene todo (bootloader, U-Boot, kernel Linux, rootfs y el firmware del
> coprocesador RISC-V). Compilar = producir ese archivo; actualizar = cargarlo
> en la placa.

El SDK deriva del **Luckfox Pico SDK**, pero es **autocontenido**: los dos
toolchains (ARM y RISC-V) y todo el código fuente ya están versionados. Tras
clonar **no se descarga nada más** — no necesitas la red durante la compilación.

---

## 0. Lo que necesitas

| Requisito | Detalle |
|-----------|---------|
| SO host | **Ubuntu 20.04 o 22.04 x86-64** (nativo o WSL2/VM). Otras distros Debian sirven; el resto de la guía asume `apt`. |
| Disco | El repo ocupa ~5 GB; deja **≥ 40 GB libres** para los objetos de compilación. |
| RAM | 4 GB mínimo, 8 GB cómodo (la compilación del kernel es lo más pesado). |
| Cable | Un **USB-C de datos** entre la placa y el PC (para flashear). |
| Permisos | Cuenta con `sudo` (para instalar paquetes y para flashear). |

> No necesitas placa para *compilar* — solo para *flashear*. Puedes hacer los
> pasos 1–4 sin hardware conectado.

---

## 1. Instalar las dependencias del host

Un único comando (necesitas el repo ya clonado — ver paso 2, o clónalo antes):

```bash
./build.sh deps        # = ./scripts/install-deps.sh (apt, Debian/Ubuntu)
```

Instala todos los paquetes del sistema para compilar el kernel, U-Boot y el
firmware del MCU. Equivale a:

```bash
sudo apt-get update
sudo apt-get install -y \
    git make gcc g++ bc cpio rsync fakeroot bison flex \
    libssl-dev device-tree-compiler scons \
    gawk texinfo cmake unzip gperf autoconf \
    libncurses5-dev pkg-config python3
```

Qué aporta cada grupo:

- `git make gcc g++ bc cpio rsync` — base de compilación y ensamblado del rootfs.
- `bison flex libssl-dev device-tree-compiler` — kernel y U-Boot (parser, cripto, `dtc`).
- **`scons`** — **obligatorio** para el firmware del MCU RISC-V (RT-Thread). Sin
  él, la parte del MCU se salta en silencio.
- `python3` — usado por el sistema de build del kernel.
- El resto (`gawk texinfo cmake unzip gperf autoconf libncurses5-dev pkg-config`)
  — utilidades que tocan distintos sub-builds.

> **Nota:** *no* hace falta `gcc-multilib` ni librerías de 32 bits. El toolchain
> es un cross-compiler independiente y las herramientas de host (`upgrade_tool`,
> `afptool`, …) son binarios x86-64 estáticos.

---

## 2. Descargar el SDK

```bash
git clone https://github.com/ElectronicCats/RV1106_SDK_lite.git
cd RV1106_SDK_lite
```

La descarga es grande (toolchains + fuentes versionadas). Si solo quieres
compilar y no te interesa el historial de git, clona superficial para bajar
menos:

```bash
git clone --depth 1 https://github.com/ElectronicCats/RV1106_SDK_lite.git
```

---

## 3. Configurar el toolchain (una vez por terminal)

```bash
source scripts/00-setup-toolchain.sh
```

Esto **no instala nada** — solo exporta `CC`, `CROSS_COMPILE`, `PATH`, etc.
apuntando al cross-compiler ARM que ya viene en `toolchain/`. También avisa si
falta algún paquete del host (paso 1).

> Debe hacerse con `source` (no `./`), y **en cada terminal nueva** antes de
> compilar. Si abres otra pestaña, vuelve a ejecutarlo.

Comprobación rápida de que quedó bien:

```bash
${CC} --version      # debe imprimir arm-rockchip830-... gcc 8.3.0
```

---

## 4. Compilar la imagen por primera vez

```bash
./build.sh
```

Esto compila **todo en el orden correcto** y termina empaquetando la imagen:

```
mcu → uboot → kernel → paquetes → rootfs → pack
```

La primera vez tarda (kernel incluido): de varios minutos a ~media hora según
tu CPU. Al terminar tendrás la imagen final:

```
output/images/update.img          ← esto es lo que se flashea
```

> ¿Compilación interrumpida o toolchain sin configurar? El error
> `Toolchain not configured` significa que olvidaste el paso 3 en esta terminal.

Si solo quieres reconstruir una parte más adelante, hay comandos por
componente (`./build.sh mcu|uboot|kernel|rootfs|pack`). El detalle está en
[build/packaging.md](build/packaging.md).

---

## 5. Conectar la placa y flashear

### 5.1 Poner la placa en modo bootloader (maskrom)

Con la placa conectada por USB al PC, usando los botones de la placa:

1. **Mantén presionado el botón `BOOT`.**
2. Sin soltar `BOOT`, **presiona y suelta `RST` (reset)**.
3. Sigue sosteniendo `BOOT` ~**5 s**; entonces suéltalo.

### 5.2 Confirmar que entró en maskrom

```bash
sudo tools/upgrade_tool LD
# Maskrom OK →  DevNo=1  Vid=0x2207,Pid=0x350a,...  Mode=Maskrom
```

Si no lista nada, la placa no entró: repite 5.1. `Mode=Maskrom` es la única
confirmación fiable.

### 5.3 Cargar el firmware

```bash
sudo ./build.sh flash
# equivale a:  sudo tools/upgrade_tool UF output/images/update.img
```

> Se usa `sudo` porque acceder al USB en crudo requiere privilegios. Para
> flashear sin `sudo`, crea una regla udev para el VID Rockchip `2207` (ver
> «Problemas comunes»).

Al terminar, la placa reinicia con tu imagen nueva. ¡Listo!

---

## 6. Actualizaciones posteriores (el ciclo normal)

Ya no repites todo. Cambias algo, recompilas **solo esa parte**, reempaquetas y
flasheas:

```bash
source scripts/00-setup-toolchain.sh   # si abriste terminal nueva
./build.sh kernel                      # p.ej. tocaste el kernel
./build.sh pack                        # reempaqueta update.img
sudo ./build.sh flash                  # maskrom (paso 5) + carga
```

⚠️ **Orden que importa:** si tocas el firmware del MCU, corre `./build.sh mcu`
**antes** de `./build.sh uboot` — U-Boot **embebe** `rtthread.bin`. La guía
completa de build/pack/flash por componente está en
[build/packaging.md](build/packaging.md).

---

## 7. Problemas comunes

| Síntoma | Causa / solución |
|---------|------------------|
| `Toolchain not configured` | Falta el paso 3 en esta terminal: `source scripts/00-setup-toolchain.sh`. |
| `scons not installed` | Falta `scons` (paso 1): `sudo apt-get install scons`. |
| El MCU no se actualizó | Recompilaste `uboot` sin recompilar `mcu` primero. U-Boot embebe `rtthread.bin`: corre `mcu` y luego `uboot`. |
| `upgrade_tool` no detecta la placa | No está en maskrom (repite 5.1), cable USB sin datos, o falta `sudo`. Verifica con `lsusb \| grep 2207`. |
| `Download Boot Fail` / `please check ddr` | El maskrom se «ensucia» tras reintentos. **Una entrada de maskrom = un intento de `UF`**: si falla, vuelve a hacer maskrom (5.1) y flashea al **primer** intento, no en bucle. |
| Flashea pero no arranca | Usa **siempre `UF`** (imagen completa). `DI -b` responde «ok» pero **no escribe** en esta SPI-NAND. `sudo ./build.sh flash` ya usa `UF`. |
| Flashear sin `sudo` | Crea `/etc/udev/rules.d/99-rockchip.rules` con:<br>`SUBSYSTEM=="usb", ATTR{idVendor}=="2207", MODE="0666"`<br>luego `sudo udevadm control --reload && sudo udevadm trigger`. |

---

## Siguiente

- Ensamblado de imagen y flasheo en detalle — [build/packaging.md](build/packaging.md)
- Qué corre en cada núcleo (Linux vs MCU RISC-V) — [architecture/overview.md](architecture/overview.md)
- El toolchain por dentro — [build/toolchain.md](build/toolchain.md)

---

[English](getting-started.en.md)
