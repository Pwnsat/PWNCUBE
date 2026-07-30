#!/bin/bash
# RV1106 SDK - Shared functions and variables
set -euo pipefail
SDK_DIR="$(cd "$(dirname "$(dirname "$0")")" && pwd)"
OUTPUT_DIR="${SDK_DIR}/output"; IMAGE_DIR="${OUTPUT_DIR}/images"
BOARD_DIR="${OUTPUT_DIR}/board_bin"; ROOTFS_DIR="${OUTPUT_DIR}/rootfs"
KERNEL_OBJ_DIR="${OUTPUT_DIR}/objs_kernel"
ARCH="${ARCH:-arm}"; CROSS_COMPILE="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
TOOLCHAIN_DIR="${SDK_DIR}/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
SYSROOT="${TOOLCHAIN_DIR}/arm-rockchip830-linux-uclibcgnueabihf/sysroot"
KERNEL_SRC="${SDK_DIR}/src/kernel"; UBOOT_SRC="${SDK_DIR}/src/u-boot"
RKBIN_DIR="${SDK_DIR}/src/rkbin"; TOOLS_DIR="${SDK_DIR}/tools"
export PATH="${TOOLS_DIR}:${PATH}"
log() { echo -e "\e[32m[*]\e[0m $*"; }
warn() { echo -e "\e[33m[w]\e[0m $*"; }
err() { echo -e "\e[31m[!]\e[0m $*"; exit 1; }
prepare_dirs() { mkdir -p "$IMAGE_DIR" "$BOARD_DIR" "$ROOTFS_DIR" "$KERNEL_OBJ_DIR"; }
# Load board config if available
BOARD_CONFIG_DIR="${SDK_DIR}/configs/board"
for f in "$BOARD_CONFIG_DIR"/*.mk; do
    if [ -f "$f" ]; then source "$f"; break; fi
done
