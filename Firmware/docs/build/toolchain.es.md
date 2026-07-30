# Toolchain: arm-rockchip830-linux-uclibcgnueabihf

## Descripción general

El SDK proporciona un toolchain de compilación cruzada generado con **crosstool-NG** que apunta a ARM Linux con uClibc. Se encuentra en `toolchain/arm-rockchip830-linux-uclibcgnueabihf/` y se utiliza para compilar U-Boot, el kernel de Linux y el sistema de archivos raíz para la plataforma RV1106.

### Especificaciones

| Propiedad            | Valor                                        |
|----------------------|----------------------------------------------|
| Triplete objetivo    | `arm-rockchip830-linux-uclibcgnueabihf`      |
| Arquitectura         | ARMv7-a                                      |
| ABI de coma flotante | hard (VFPv4-D16)                             |
| FPU                  | neon-vfpv4                                   |
| Endianness           | Little-endian                                |
| Versión de GCC       | 8.3.0                                        |
| Versión de uClibc    | 1.0.31                                       |
| Binutils             | 2.31+ (soporta gold linker, plugins, LTO)    |
| GDB                  | Incluido (depurador cruzado)                 |
| Biblioteca C++       | libstdc++ (incluida)                         |
| Hilos                | linuxthreads (vía uClibc)                    |
| Sistema de compilación | crosstool-NG                               |
| SO objetivo          | Linux (kernel 5.10)                          |

## Estructura de directorios

```
toolchain/arm-rockchip830-linux-uclibcgnueabihf/
├── arm-rockchip830-linux-uclibcgnueabihf/   # sysroot objetivo y herramientas
│   ├── bin/                                  # utilidades del lado objetivo (ldd, getconf)
│   ├── debug-root/                           # esqueleto del sistema de depuración
│   ├── include/                              # cabeceras C++ del objetivo (c++)
│   ├── lib/                                  # bibliotecas de ejecución del objetivo
│   └── sysroot/                              # raíz del sistema (usr/include, usr/lib)
│       ├── lib/                              # bibliotecas compartidas (ld-uClibc, libc, etc.)
│       ├── sbin/
│       └── usr/
│           ├── bin/                          # getconf, ldd
│           ├── include/                      # cabeceras del kernel Linux + uClibc
│           └── lib/                          # bibliotecas estáticas, objetos crt, pkgconfig
├── bin/                                      # herramientas de compilación cruzada (prefijo: arm-rockchip830-...)
│   ├── arm-rockchip830-linux-uclibcgnueabihf-gcc
│   ├── arm-rockchip830-linux-uclibcgnueabihf-g++
│   ├── arm-rockchip830-linux-uclibcgnueabihf-ld
│   ├── arm-rockchip830-linux-uclibcgnueabihf-gdb
│   └── ... (33 herramientas en total)
├── lib/                                      # bibliotecas internas de GCC y plugins
│   ├── bfd-plugins/                          # plugin LTO
│   ├── gcc/arm-rockchip830-linux-uclibcgnueabihf/8.3.0/
│   │   ├── crtbegin.o, crtend.o, ...         # objetos CRT
│   │   ├── libgcc.a, libgcov.a              # runtime de GCC
│   │   └── include/                          # cabeceras provistas por GCC (arm_neon.h, stdint.h, ...)
│   └── ldscripts/                            # scripts del enlazador (armelfb_linux_eabi.x*)
├── libexec/gcc/arm-rockchip830-linux-uclibcgnueabihf/8.3.0/
│   ├── cc1, cc1plus                          # frontends del compilador
│   ├── collect2, lto1, lto-wrapper           # optimización en tiempo de enlace
│   └── plugin/gengtype
├── runtime_lib/                              # bibliotecas de ejecución para el objetivo
│   └── lib.tar.bz2                           # bibliotecas compartidas comprimidas (desplegar en el objetivo)
├── share/
│   ├── gcc-8.3.0/python/libstdcxx/           # pretty-printers de GDB para libstdc++
│   ├── gdb/                                  # archivos de soporte de GDB (syscalls, init)
│   └── licenses/                             # textos de licencia de todos los componentes
├── arm-rockchip830-linux-uclibcgnueabihf_defconfig  # configuración de crosstool-NG
├── env_install_toolchain.sh                  # script de instalación (agrega al PATH)
└── readme.txt                                # instrucciones rápidas de instalación
```

## Configuración del entorno

La forma canónica de configurar el toolchain es ejecutando el script de configuración:

```bash
source scripts/00-setup-toolchain.sh
```

Esto exporta las siguientes variables de entorno:

### Variables

| Variable        | Valor / Ejemplo                                                        |
|-----------------|------------------------------------------------------------------------|
| `TOOLCHAIN_DIR` | `$SDK/toolchain/arm-rockchip830-linux-uclibcgnueabihf`                 |
| `CROSS_COMPILE` | `arm-rockchip830-linux-uclibcgnueabihf-`                               |
| `ARCH`          | `arm`                                                                  |
| `SYSROOT`       | `$TOOLCHAIN_DIR/arm-rockchip830-linux-uclibcgnueabihf/sysroot`          |
| `CC`            | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc`          |
| `CXX`           | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-g++`          |
| `AR`            | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-ar`           |
| `AS`            | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-as`           |
| `LD`            | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-ld`           |
| `NM`            | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-nm`           |
| `OBJCOPY`       | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-objcopy`      |
| `OBJDUMP`       | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-objdump`      |
| `RANLIB`        | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-ranlib`       |
| `READELF`       | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-readelf`      |
| `SIZE`          | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-size`         |
| `STRINGS`       | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-strings`      |
| `STRIP`         | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-strip`        |
| `GDB`           | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-gdb`          |
| `GCOV`          | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-gcov`         |
| `CPP`           | `$TOOLCHAIN_DIR/bin/arm-rockchip830-linux-uclibcgnueabihf-cpp`          |
| `CFLAGS`        | `-march=armv7-a -mfloat-abi=hard -mfpu=neon --sysroot=$SYSROOT`        |
| `CXXFLAGS`      | (igual que `CFLAGS`)                                                    |
| `LDFLAGS`       | `--sysroot=$SYSROOT`                                                    |
| `PATH`          | `$TOOLCHAIN_DIR/bin:$PATH` (se antepone automáticamente)                |

> La variable `SYSROOT` apunta a la raíz del sistema objetivo. El compilador busca automáticamente cabeceras y bibliotecas allí cuando se pasa `--sysroot`.

## Bibliotecas de ejecución

El archivo `runtime_lib/lib.tar.bz2` contiene las bibliotecas compartidas necesarias en el dispositivo objetivo. Extráigalo al directorio `/lib` del objetivo:

```bash
tar -xjf runtime_lib/lib.tar.bz2 -C /ruta/a/la/raiz/objetivo/
```

### Contenido

```
lib/
├── ld-uClibc-1.0.31.so      # enlazador dinámico de uClibc
├── ld-uClibc.so.0            #   enlace simbólico
├── ld-uClibc.so.1            #   enlace simbólico
├── libatomic.so              # runtime de operaciones atómicas de GCC
├── libatomic.so.1
├── libatomic.so.1.2.0
├── libc.so.0                 # biblioteca C de uClibc
├── libc.so.1                 #   enlace simbólico
├── libgcc_s.so               # biblioteca de soporte de GCC
├── libgcc_s.so.1
├── libitm.so                 # memoria transaccional de GCC
├── libitm.so.1
├── libitm.so.1.0.0
├── libstdc++.so              # biblioteca estándar de C++
├── libstdc++.so.6
├── libstdc++.so.6.0.25
├── libstdc++.so.6.0.25-gdb.py  # pretty-printers de GDB
├── libthread_db-1.0.31.so    # biblioteca de depuración de hilos
├── libthread_db.so.1
└── libuClibc-1.0.31.so      # uClibc estática (para depuración)
```

## Dependencias del anfitrión

El script de validación del SDK verifica estos paquetes en el equipo de compilación:

| Paquete                | Propósito                       |
|------------------------|---------------------------------|
| `git`                  | Gestión de fuentes              |
| `make`                 | Sistema de compilación          |
| `gcc`                  | Compilador anfitrión (herramientas) |
| `g++`                  | Compilador C++ anfitrión        |
| `bc`                   | Ayudante de compilación del kernel |
| `cpio`                 | Creación de archivos rootfs     |
| `rsync`                | Sincronización de archivos      |
| `bison`                | Generador de analizadores       |
| `flex`                 | Generador de analizadores léxicos |
| `libssl-dev`           | Criptografía (U-Boot, kernel)   |
| `device-tree-compiler` | Compilación de device tree (dtc) |

Instálelos con:

```bash
sudo apt install git make gcc g++ bc cpio rsync bison flex libssl-dev device-tree-compiler
```

> **Nota:** `fakeroot` y `gcc-multilib` **no** son necesarios para este toolchain — es un compilador cruzado independiente y no depende de bibliotecas anfitrionas de 32 bits.

## Inicio rápido

```bash
# 1. Configurar el entorno
source scripts/00-setup-toolchain.sh

# 2. Verificar que el compilador funciona
${CC} --version

# 3. Probar la compilación de un programa C simple
cat > hello.c << 'EOF'
#include <stdio.h>
int main(void) { printf("¡Hola, RV1106!\n"); return 0; }
EOF
${CC} ${CFLAGS} -o hello hello.c
file hello   # Debería mostrar: ELF 32-bit LSB executable, ARM, version 1 (SYSV)

# 4. Copiar las bibliotecas de ejecución al objetivo
# Extraer lib.tar.bz2 en el directorio /lib/ del dispositivo

# 5. Compilar el SDK completo
./build.sh
```

---

[English](toolchain.en.md)
