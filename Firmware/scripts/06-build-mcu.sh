#!/bin/bash
# ---------------------------------------------------------------------------
# 06-build-mcu.sh -- Build the RISC-V coprocessor (HPMCU) firmware.
#
# The RV1106 pairs the Cortex-A7 (Linux) with a Syntacore SCR1 RISC-V core
# ("HPMCU") running RT-Thread. This builds rtthread.bin for the CubeSat MCU
# board variant and copies it to output/.
#
# Source tree vendored under src/mcu/ (RT-Thread BSP rv1106-mcu, from the
# Luckfox SDK used as technical reference). See docs/migration/.
#
# Usage:  bash scripts/06-build-mcu.sh [clean]
# ---------------------------------------------------------------------------
set -euo pipefail

SDK_DIR="$(cd "$(dirname "$(dirname "${BASH_SOURCE[0]}")")" && pwd)"
MCU_BSP="${SDK_DIR}/src/mcu/rt-thread/bsp/rockchip/rv1106-mcu"
RTT_ROOT="${SDK_DIR}/src/mcu/rt-thread"
BOARD="${MCU_BOARD:-pwncube}"
RISCV_TC_DIR="${SDK_DIR}/toolchain/riscv/xpack-riscv-none-embed-gcc-10.2.0-1.2"
RISCV_BIN="${RISCV_TC_DIR}/bin"
OUT_DIR="${SDK_DIR}/output/mcu"

log()  { echo -e "\e[32m[mcu]\e[0m $*"; }
err()  { echo -e "\e[31m[mcu!]\e[0m $*" >&2; exit 1; }

# ---- Toolchain check -----------------------------------------------------
if [ ! -x "${RISCV_BIN}/riscv-none-embed-gcc" ]; then
    err "RISC-V toolchain not found at ${RISCV_BIN}
    Download xPack riscv-none-embed-gcc 10.2.0-1.2 and extract into toolchain/riscv/ :
      url=https://github.com/xpack-dev-tools/riscv-none-embed-gcc-xpack/releases/download/v10.2.0-1.2/xpack-riscv-none-embed-gcc-10.2.0-1.2-linux-x64.tar.gz
      mkdir -p toolchain/riscv && curl -fSL \"\$url\" | tar xz -C toolchain/riscv"
fi

[ -d "${MCU_BSP}/board/${BOARD}" ] || err "MCU board '${BOARD}' not found under ${MCU_BSP}/board/"
command -v scons >/dev/null 2>&1 || err "scons not installed (apt install scons)"

export RTT_ROOT
export RTT_EXEC_PATH="${RISCV_BIN}"
export PATH="${RISCV_BIN}:${PATH}"

cd "${MCU_BSP}"

if [ "${1:-}" = "clean" ]; then
    log "Cleaning MCU build..."
    scons -c >/dev/null 2>&1 || true
    rm -rf build rtthread.elf rtthread.bin rtthread.map .config rtconfig.h
    log "Clean done."
    exit 0
fi

log "Board: ${BOARD}  Toolchain: $(${RISCV_BIN}/riscv-none-embed-gcc -dumpversion)"

# Sync the SHARED SX1262 command core from the Linux kmod (single source of
# truth, docs/migration/design/40-migration-design.md §4). sx1262_cmd.c compiles unchanged on both
# sides (its includes are #ifdef __KERNEL__-guarded); the RT-Thread glue lives
# in applications/sx1262_port.{h,c}. Do NOT edit the synced copies.
for f in sx1262_cmd.c sx1262_regs.h; do
    cp -f "${SDK_DIR}/src/sx1262-kmod/${f}" "${MCU_BSP}/applications/${f}"
done
log "Synced sx1262_cmd.c/sx1262_regs.h from src/sx1262-kmod/"

log "Generating config (rtconfig.h) from board/${BOARD}/defconfig..."
cp "board/${BOARD}/defconfig" .config
# building.py reads rtconfig.h before --useconfig regenerates it; ensure it exists
# (matches the vendored Luckfox build.sh behaviour).
[ -f rtconfig.h ] || touch rtconfig.h
scons --useconfig="board/${BOARD}/defconfig" >/dev/null 2>&1 || err "config generation failed"

log "Compiling RT-Thread firmware..."
# Drop the previous binary first: scons leaves it in place when it aborts while
# reading the SConscripts, and a stale rtthread.bin would silently pass the
# check below and get staged into the loader. The .elf goes too — rtthread.bin
# is produced by a post-link objcopy, which only runs when the .elf relinks.
rm -f rtthread.bin rtthread.elf
# Filter benign python3 regex SyntaxWarnings from the vendored RT-Thread tools.
# grep is last in the pipe, so scons' own status comes from PIPESTATUS.
set +e
scons -j"$(nproc)" 2>&1 | grep -vE 'SyntaxWarning|re\.(search|findall)|stdc ='
SCONS_RC=${PIPESTATUS[0]}
set -e
[ "${SCONS_RC}" -eq 0 ] || err "build failed: scons exited with code ${SCONS_RC}"

[ -f rtthread.bin ] || err "build failed: rtthread.bin not produced"

mkdir -p "${OUT_DIR}"
cp -f rtthread.bin rtthread.elf "${OUT_DIR}/"

# Stage the firmware into rkbin so boot_merger embeds it as the Hpmcu loader
# (RV1106MINIALL.ini: Hpmcu=bin/rv11/rtthread.bin). Consumed by build.sh uboot.
RKBIN_RV11="${SDK_DIR}/src/rkbin/bin/rv11"
if [ -d "${RKBIN_RV11}" ]; then
    cp -f rtthread.bin "${RKBIN_RV11}/rtthread.bin"
    log "Staged -> src/rkbin/bin/rv11/rtthread.bin"
fi

log "Firmware: ${OUT_DIR}/rtthread.bin ($(stat -c%s rtthread.bin) bytes)"
"${RISCV_BIN}/riscv-none-embed-size" rtthread.elf
log "Done."
