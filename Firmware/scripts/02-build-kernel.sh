#!/bin/bash
#
# 02-build-kernel.sh
# Builds the Linux kernel for the RV1106 SDK target.
#
# This script:
#   1. Configures the kernel using a minimal defconfig
#   2. Builds the kernel + DTB + FIT boot image
#   3. Copies boot.img, vmlinux, and DTB to output/
#
# Usage:
#   ./scripts/02-build-kernel.sh              # uses default defconfig
#   ./scripts/02-build-kernel.sh -j4           # parallel build
#   CROSS_COMPILE=arm-linux-gnueabihf- ./scripts/02-build-kernel.sh
#

set -euo pipefail

# ---- Paths ----------------------------------------------------------------
SDK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_SRC="${SDK_ROOT}/src/kernel"
DEFCONFIG_SRC="${SDK_ROOT}/configs/kernel/rv1106_minimal_defconfig"
OBJ_DIR="${SDK_ROOT}/output/objs_kernel"
export PATH="${SDK_ROOT}/tools:${PATH}"
OUT_IMAGES="${SDK_ROOT}/output/images"
OUT_BOARD="${SDK_ROOT}/output/board_bin"
DTS_FILE="rv1106g-sdk"
BOOT_ITS="${KERNEL_SRC}/boot.its"

# ---- Toolchain defaults ---------------------------------------------------
ARCH="${ARCH:-arm}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
JOBS="${JOBS:-$(nproc)}"

# ---- Parse extra args (e.g. -j4) ------------------------------------------
EXTRA_ARGS=()
for arg in "$@"; do
    case "$arg" in
        -j*)
            JOBS="${arg#-j}"
            ;;
        *)
            EXTRA_ARGS+=("$arg")
            ;;
    esac
done

# ---- Pre-flight checks -----------------------------------------------------
if [ ! -d "$KERNEL_SRC" ]; then
    echo "[ERROR] Kernel source not found at: $KERNEL_SRC"
    echo "Did you run step 1 (copy kernel)?"
    exit 1
fi

if [ ! -f "$DEFCONFIG_SRC" ]; then
    echo "[ERROR] Defconfig not found at: $DEFCONFIG_SRC"
    exit 1
fi

if [ ! -f "$BOOT_ITS" ]; then
    echo "[ERROR] FIT source (boot.its) not found at: $BOOT_ITS"
    exit 1
fi

if ! command -v "${CROSS_COMPILE}gcc" &>/dev/null; then
    echo "[WARN] Cross-compiler '${CROSS_COMPILE}gcc' not found in PATH."
    echo "       Set CROSS_COMPILE or install the toolchain."
    echo "       Continuing anyway (build may fail)..."
fi

# ---- Create output directories --------------------------------------------
mkdir -p "$OBJ_DIR" "$OUT_IMAGES" "$OUT_BOARD"

# ---- Step 1: Configure kernel ---------------------------------------------
echo "============================================"
echo " RV1106 Kernel Build"
echo " ARCH:      ${ARCH}"
echo " SRC:       ${KERNEL_SRC}"
echo " DEFCONFIG: ${DEFCONFIG_SRC}"
echo " OBJ_DIR:   ${OBJ_DIR}"
echo " JOBS:      ${JOBS}"
echo "============================================"

echo ""
echo "[1/3] Copying defconfig to object directory..."
cp -f "$DEFCONFIG_SRC" "${OBJ_DIR}/.config"

echo "[2/3] Running olddefconfig (apply defaults to any missing symbols)..."
make O="$OBJ_DIR" -C "$KERNEL_SRC" \
    ARCH="$ARCH" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    olddefconfig

# ---- Step 2: Build kernel + DTB + FIT boot image --------------------------
echo ""
echo "[3/3] Building kernel, DTB, and FIT image (${DTS_FILE}.img)..."
echo "      Target: ${DTS_FILE}.img"
echo "      BOOT_ITS: ${BOOT_ITS}"

make O="$OBJ_DIR" -C "$KERNEL_SRC" \
    ARCH="$ARCH" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    BOOT_ITS="$BOOT_ITS" \
    "${DTS_FILE}.img" \
    -j"$JOBS" \
    "${EXTRA_ARGS[@]}"

echo ""
echo "Build completed successfully."
echo ""

# ---- Step 3: Copy artifacts to output directories -------------------------
echo "Copying artifacts..."

# boot.img – the packed FIT image (kernel + DTB + resource)
BOOT_IMG_SRC="${OBJ_DIR}/boot.img"
if [ -f "$BOOT_IMG_SRC" ]; then
    cp -fv "$BOOT_IMG_SRC" "${OUT_IMAGES}/boot.img"
    cp -fv "$BOOT_IMG_SRC" "${OUT_BOARD}/boot.img"
    echo "  boot.img -> ${OUT_IMAGES}/boot.img"
else
    echo "  [WARN] boot.img not found at ${BOOT_IMG_SRC}"
fi

# vmlinux – uncompressed ELF
VMLINUX_SRC="${OBJ_DIR}/vmlinux"
if [ -f "$VMLINUX_SRC" ]; then
    cp -fv "$VMLINUX_SRC" "${OUT_BOARD}/vmlinux"
    echo "  vmlinux  -> ${OUT_BOARD}/vmlinux"
else
    echo "  [WARN] vmlinux not found at ${VMLINUX_SRC}"
fi

# DTB – compiled device tree blob
DTB_SRC="${OBJ_DIR}/arch/${ARCH}/boot/dts/${DTS_FILE}.dtb"
if [ -f "$DTB_SRC" ]; then
    cp -fv "$DTB_SRC" "${OUT_IMAGES}/${DTS_FILE}.dtb"
    cp -fv "$DTB_SRC" "${OUT_BOARD}/${DTS_FILE}.dtb"
    echo "  dtb      -> ${OUT_IMAGES}/${DTS_FILE}.dtb"
else
    echo "  [WARN] DTB not found at ${DTB_SRC}"
    # Fallback: try to find it
    FOUND_DTB=$(find "${OBJ_DIR}/arch/${ARCH}/boot/dts" -name "${DTS_FILE}.dtb" 2>/dev/null | head -1)
    if [ -n "$FOUND_DTB" ]; then
        cp -fv "$FOUND_DTB" "${OUT_IMAGES}/${DTS_FILE}.dtb"
        cp -fv "$FOUND_DTB" "${OUT_BOARD}/${DTS_FILE}.dtb"
        echo "  dtb (fallback) -> ${OUT_IMAGES}/${DTS_FILE}.dtb"
    fi
fi

# zImage – compressed kernel image (alternative output)
ZIMAGE_SRC="${OBJ_DIR}/arch/${ARCH}/boot/zImage"
if [ -f "$ZIMAGE_SRC" ]; then
    cp -fv "$ZIMAGE_SRC" "${OUT_IMAGES}/zImage"
    echo "  zImage   -> ${OUT_IMAGES}/zImage"
fi

# resource.img – Rockchip multipack resource
RESOURCE_IMG_SRC="${OBJ_DIR}/resource.img"
if [ -f "$RESOURCE_IMG_SRC" ]; then
    cp -fv "$RESOURCE_IMG_SRC" "${OUT_IMAGES}/resource.img"
    echo "  resource.img -> ${OUT_IMAGES}/resource.img"
fi

echo ""
echo "============================================"
echo " Build artifacts:"
echo "  ${OUT_IMAGES}/"
ls -lh "${OUT_IMAGES}/" 2>/dev/null || echo "  (empty)"
echo ""
echo "  ${OUT_BOARD}/"
ls -lh "${OUT_BOARD}/" 2>/dev/null || echo "  (empty)"
echo "============================================"
echo "Kernel build complete."
