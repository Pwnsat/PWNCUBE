# Toolchain: arm-rockchip830-linux-uclibcgnueabihf

## Overview

The SDK provides a prebuilt **crosstool-NG** generated cross-compilation toolchain targeting ARM Linux with uClibc. It is located at `toolchain/arm-rockchip830-linux-uclibcgnueabihf/` and is used to build U-Boot, the Linux kernel, and the root filesystem for the RV1106 platform.

### Specs

| Property             | Value                                        |
|----------------------|----------------------------------------------|
| Target triple        | `arm-rockchip830-linux-uclibcgnueabihf`      |
| Architecture         | ARMv7-a                                      |
| Floating-point ABI   | hard (VFPv4-D16)                             |
| FPU                  | neon-vfpv4                                   |
| Endianness           | Little-endian                                |
| GCC version          | 8.3.0                                        |
| uClibc version       | 1.0.31                                       |
| Binutils             | 2.31+ (supports gold linker, plugins, LTO)   |
| GDB                  | Included (cross-debugger)                    |
| C++ standard library | libstdc++ (included)                         |
| Threading            | linuxthreads (via uClibc)                    |
| Build system         | crosstool-NG                                 |
| Target OS            | Linux (kernel 5.10)                          |

## Directory Structure

```
toolchain/arm-rockchip830-linux-uclibcgnueabihf/
├── arm-rockchip830-linux-uclibcgnueabihf/   # Target sysroot & tools
│   ├── bin/                                  # Target-side utilities (ldd, getconf)
│   ├── debug-root/                           # Debug root filesystem skeleton
│   ├── include/                              # Target C++ headers (c++)
│   ├── lib/                                  # Target runtime libraries
│   └── sysroot/                              # System root (usr/include, usr/lib)
│       ├── lib/                              # Shared libraries (ld-uClibc, libc, etc.)
│       ├── sbin/
│       └── usr/
│           ├── bin/                          # getconf, ldd
│           ├── include/                      # Linux kernel + uClibc headers
│           └── lib/                          # Static libraries, crt objects, pkgconfig
├── bin/                                      # Cross-compilation tools (prefix: arm-rockchip830-...)
│   ├── arm-rockchip830-linux-uclibcgnueabihf-gcc
│   ├── arm-rockchip830-linux-uclibcgnueabihf-g++
│   ├── arm-rockchip830-linux-uclibcgnueabihf-ld
│   ├── arm-rockchip830-linux-uclibcgnueabihf-gdb
│   └── ... (33 tools total)
├── lib/                                      # GCC internal libraries & plugins
│   ├── bfd-plugins/                          # LTO plugin
│   ├── gcc/arm-rockchip830-linux-uclibcgnueabihf/8.3.0/
│   │   ├── crtbegin.o, crtend.o, ...         # CRT objects
│   │   ├── libgcc.a, libgcov.a              # GCC runtime
│   │   └── include/                          # GCC-provided headers (arm_neon.h, stdint.h, ...)
│   └── ldscripts/                            # Linker scripts (armelfb_linux_eabi.x*)
├── libexec/gcc/arm-rockchip830-linux-uclibcgnueabihf/8.3.0/
│   ├── cc1, cc1plus                          # Compiler frontends
│   ├── collect2, lto1, lto-wrapper           # Link-time optimization
│   └── plugin/gengtype
├── runtime_lib/                              # Runtime libraries for target
│   └── lib.tar.bz2                           # Compressed shared libraries (deploy to target)
├── share/
│   ├── gcc-8.3.0/python/libstdcxx/           # GDB pretty-printers for libstdc++
│   ├── gdb/                                  # GDB support files (syscalls, init)
│   └── licenses/                             # License texts for all components
├── arm-rockchip830-linux-uclibcgnueabihf_defconfig  # crosstool-NG config
├── env_install_toolchain.sh                  # Install script (adds to PATH)
└── readme.txt                                # Quick install instructions
```

## Environment Setup

The canonical way to set up the toolchain is to source the setup script:

```bash
source scripts/00-setup-toolchain.sh
```

This exports the following environment variables:

### Variables

| Variable        | Value / Example                                                        |
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
| `CXXFLAGS`      | (same as `CFLAGS`)                                                      |
| `LDFLAGS`       | `--sysroot=$SYSROOT`                                                    |
| `PATH`          | `$TOOLCHAIN_DIR/bin:$PATH` (prepended automatically)                    |

> The `SYSROOT` variable points to the target system root. The compiler automatically searches for headers and libraries there when `--sysroot` is passed.

## Runtime Libraries

The `runtime_lib/lib.tar.bz2` archive contains the shared libraries needed on the target device. Extract it to the target's `/lib` directory:

```bash
tar -xjf runtime_lib/lib.tar.bz2 -C /path/to/target/root/
```

### Contents

```
lib/
├── ld-uClibc-1.0.31.so      # uClibc dynamic linker
├── ld-uClibc.so.0            #   symlink
├── ld-uClibc.so.1            #   symlink
├── libatomic.so              # GCC atomic ops runtime
├── libatomic.so.1
├── libatomic.so.1.2.0
├── libc.so.0                 # uClibc C library
├── libc.so.1                 #   symlink
├── libgcc_s.so               # GCC support library
├── libgcc_s.so.1
├── libitm.so                 # GCC transactional memory
├── libitm.so.1
├── libitm.so.1.0.0
├── libstdc++.so              # C++ standard library
├── libstdc++.so.6
├── libstdc++.so.6.0.25
├── libstdc++.so.6.0.25-gdb.py  # GDB pretty-printers
├── libthread_db-1.0.31.so    # Thread debugging library
├── libthread_db.so.1
└── libuClibc-1.0.31.so      # Static uClibc (for debugging)
```

## Host Dependencies

The SDK validation script checks for these packages on the build host:

| Package                 | Purpose                        |
|-------------------------|--------------------------------|
| `git`                   | Source management              |
| `make`                  | Build system                   |
| `gcc`                   | Host compiler (for tools)      |
| `g++`                   | Host C++ compiler              |
| `bc`                    | Kernel build helper            |
| `cpio`                  | Rootfs archive creation        |
| `rsync`                 | File syncing                   |
| `bison`                 | Parser generator               |
| `flex`                  | Lexer generator                |
| `libssl-dev`            | Crypto (U-Boot, kernel)        |
| `device-tree-compiler`  | Device tree compilation (dtc)  |

Install them with:

```bash
sudo apt install git make gcc g++ bc cpio rsync bison flex libssl-dev device-tree-compiler
```

> **Note:** `fakeroot` and `gcc-multilib` are **not** required for this toolchain — it is a standalone cross-compiler and does not rely on 32-bit host libraries.

## Quick Start

```bash
# 1. Set up the environment
source scripts/00-setup-toolchain.sh

# 2. Verify the compiler works
${CC} --version

# 3. Test compilation of a simple C program
cat > hello.c << 'EOF'
#include <stdio.h>
int main(void) { printf("Hello, RV1106!\n"); return 0; }
EOF
${CC} ${CFLAGS} -o hello hello.c
file hello   # Should show: ELF 32-bit LSB executable, ARM, version 1 (SYSV)

# 4. Copy runtime libraries to target
# Extract lib.tar.bz2 to the device's /lib/

# 5. Build the full SDK
./build.sh
```

---

[Español](toolchain.es.md)
