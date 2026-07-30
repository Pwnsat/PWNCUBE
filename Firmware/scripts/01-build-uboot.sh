#!/bin/bash
#
# Build U-Boot for RV1106
#
# This script builds U-Boot 2017.09 for the Rockchip RV1106 SoC,
# producing uboot.img, trust.img, idblock.img, and download.bin.
#
# Usage:
#   ./scripts/01-build-uboot.sh                 # uses default CROSS_COMPILE
#   CROSS_COMPILE=... ./scripts/01-build-uboot.sh  # override toolchain
#
# Prerequisites:
#   - arm-rockchip830-linux-uclibcgnueabihf- cross-compiler on PATH
#   - Device tree compiler (dtc) installed
#   - Python 2 (for FIT image generation)

set -euo pipefail

# --- Paths ----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UBOOT_SRC="${SDK_DIR}/src/u-boot"
RKBIN_DIR="${SDK_DIR}/src/rkbin"
CONFIGS_DIR="${SDK_DIR}/configs/uboot"
OUTPUT_DIR="${SDK_DIR}/output/images"

DEFCONFIG_NAME="rv1106_sdk_defconfig"
DEFCONFIG_SRC="${CONFIGS_DIR}/${DEFCONFIG_NAME}"
DEFCONFIG_DST="${UBOOT_SRC}/configs/${DEFCONFIG_NAME}"

# --- Toolchain ------------------------------------------------------------
CROSS_COMPILE="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
export CROSS_COMPILE

# --- Build step -----------------------------------------------------------
echo "=========================================="
echo "U-Boot Build Script for RV1106"
echo "=========================================="
echo "Source:      ${UBOOT_SRC}"
echo "Defconfig:   ${DEFCONFIG_SRC}"
echo "rkbin:       ${RKBIN_DIR}"
echo "Output:      ${OUTPUT_DIR}"
echo "CROSS_COMPILE: ${CROSS_COMPILE}"
echo ""

# 1) Ensure output directory exists
mkdir -p "${OUTPUT_DIR}"

# 2) Copy defconfig from SDK configs into U-Boot configs/
if [ -f "${DEFCONFIG_DST}" ]; then
    echo "[*] Defconfig already present in U-Boot configs/, removing first"
    rm -f "${DEFCONFIG_DST}"
fi
echo "[*] Copying defconfig to U-Boot source tree"
cp -v "${DEFCONFIG_SRC}" "${DEFCONFIG_DST}"

# 2b) Recalculate CONFIG_SPL_FIT_IMAGE_KB at build time so uboot.itb (which
# embeds the MCU rtthread.bin) is sized to the actual firmware. Must match the
# uboot partition size that 04-pack-image.sh derives from the same firmware.
source "${SDK_DIR}/scripts/lib-layout.sh"
MCU_FW="${RKBIN_DIR}/bin/rv11/rtthread.bin"
FIT_KB=$(rk_uboot_fit_kb "${MCU_FW}")
MCU_FW_KB=0; [ -f "${MCU_FW}" ] && MCU_FW_KB=$(( ( $(stat -c %s "${MCU_FW}") + 1023 ) / 1024 ))
echo "[*] Sizing uboot FIT (CONFIG_SPL_FIT_IMAGE_KB) to ${FIT_KB}K  (MCU firmware ${MCU_FW_KB}K + base ${RK_UBOOT_BASE_KB}K, rounded up to 256K)"
if grep -q '^CONFIG_SPL_FIT_IMAGE_KB=' "${DEFCONFIG_DST}"; then
    sed -i "s/^CONFIG_SPL_FIT_IMAGE_KB=.*/CONFIG_SPL_FIT_IMAGE_KB=${FIT_KB}/" "${DEFCONFIG_DST}"
else
    echo "CONFIG_SPL_FIT_IMAGE_KB=${FIT_KB}" >> "${DEFCONFIG_DST}"
fi

# 3) Run defconfig step via make.sh
echo ""
echo "[*] Running: make -C ${UBOOT_SRC} ${DEFCONFIG_NAME}"
make -C "${UBOOT_SRC}" "${DEFCONFIG_NAME}"

# 3b) Override SPL boot order for SPI NAND only (skip MMC to avoid timeout delays)
# Set BOOT_MEDIUM=spi_nand in environment or board config to enable
dtsi="${UBOOT_SRC}/arch/arm/dts/rv1106-u-boot.dtsi"
dtsi_bak="${dtsi}.bak"

# Put the vendored dtsi back however we leave: an aborted build (e.g. the FIT
# overflowing the uboot partition) used to leave the tree patched and the .bak
# orphaned, which then shows up as a dirty working tree.
restore_dtsi() {
    if [ -f "${dtsi_bak}" ]; then
        mv "${dtsi_bak}" "${dtsi}"
        echo "[*] Restored original rv1106-u-boot.dtsi"
    fi
}
trap restore_dtsi EXIT

if [ "${BOOT_MEDIUM:-}" = "spi_nand" ]; then
    if [ -f "$dtsi" ] && ! grep -q "= &spi_nand, &emmc" "$dtsi"; then
        cp "$dtsi" "$dtsi_bak"
        sed -i 's/u-boot,spl-boot-order = .*;/u-boot,spl-boot-order = \&spi_nand, \&emmc;/' "$dtsi"
        echo "[*] SPL boot order set to: spi_nand, emmc"
    fi
fi

# 4) Build U-Boot with Rockchip's make.sh wrapper
#    --spl-new tells make.sh to pack the newly built SPL into the loader image
echo ""
echo "[*] Running: cd ${UBOOT_SRC} && ./make.sh --spl-new CROSS_COMPILE=${CROSS_COMPILE}"
cd "${UBOOT_SRC}"
set +e
./make.sh --spl-new "CROSS_COMPILE=${CROSS_COMPILE}"
MAKE_RC=$?
set -e
cd "${SCRIPT_DIR}"

# 4b) Report the real uboot.itb size against the partition we sized for. If it
# still overflows (e.g. U-Boot proper outgrew the base budget), show the full
# size, by how much it exceeds, and the recommended size — the upstream
# "actual/max limit" message is cryptic and doesn't say what to do.
ITB="${UBOOT_SRC}/fit/uboot.itb"
if [ -f "${ITB}" ]; then
    ITB_B=$(stat -c %s "${ITB}")
    ITB_KB=$(( (ITB_B + 1023) / 1024 ))
    LIMIT_B=$(( FIT_KB * 1024 ))
    if [ "${ITB_B}" -le "${LIMIT_B}" ]; then
        echo "[*] uboot.itb = ${ITB_B} B (${ITB_KB}K) fits the ${FIT_KB}K uboot partition ($(( (LIMIT_B - ITB_B) / 1024 ))K free)"
    else
        REC_KB=$(rk_roundup_256 "${ITB_KB}")
        C_RED="\e[31;1m"; C_OFF="\e[0m"
        echo ""                                                                                    >&2
        echo -e "${C_RED}ERROR: uboot.itb does not fit the uboot partition.${C_OFF}"               >&2
        echo -e "${C_RED}  embedded MCU firmware: ${MCU_FW_KB}K${C_OFF}"                            >&2
        echo -e "${C_RED}  uboot.itb (full):     ${ITB_B} B (${ITB_KB}K)${C_OFF}"                   >&2
        echo -e "${C_RED}  uboot partition:      ${LIMIT_B} B (${FIT_KB}K)${C_OFF}"                 >&2
        echo -e "${C_RED}  exceeds by:           $(( ITB_B - LIMIT_B )) B ($(( (ITB_B - LIMIT_B + 1023) / 1024 ))K)${C_OFF}" >&2
        echo -e "${C_RED}  recommended size:     ${REC_KB}K  — raise RK_UBOOT_BASE_KB in scripts/lib-layout.sh or trim the MCU firmware${C_OFF}" >&2
        exit 1
    fi
fi

if [ "${MAKE_RC}" -ne 0 ]; then
    echo -e "\e[31;1mERROR: U-Boot build (make.sh) failed with code ${MAKE_RC}\e[0m" >&2
    exit "${MAKE_RC}"
fi

# 5) Locate and copy generated images to output/
echo ""
echo "[*] Copying output images to ${OUTPUT_DIR}"

# uboot.img -- U-Boot proper binary
if [ -f "${UBOOT_SRC}/uboot.img" ]; then
    cp -v "${UBOOT_SRC}/uboot.img" "${OUTPUT_DIR}/uboot.img"
else
    echo "WARNING: uboot.img not found!"
fi

# trust.img -- Trusted Execution Environment (OP-TEE / TEE)
if [ -f "${UBOOT_SRC}/trust.img" ]; then
    cp -v "${UBOOT_SRC}/trust.img" "${OUTPUT_DIR}/trust.img"
else
    echo "WARNING: trust.img not found!"
fi

# idblock.img -- IDBlock (DDR init + SPL combined, 1KB-aligned)
# Make.sh generates rv1106_idblock_*.img
for f in "${UBOOT_SRC}"/rv1106_idblock*.img "${UBOOT_SRC}"/*idblock*.img; do
    [ -f "$f" ] && cp -v "$f" "${OUTPUT_DIR}/idblock.img" && break
done
if [ ! -f "${OUTPUT_DIR}/idblock.img" ]; then
    echo "WARNING: idblock.img not found!"
fi

# download.bin -- Full bootable download image (DDR + USB plug + SPL)
# Make.sh generates rv1106_download_*.bin
for f in "${UBOOT_SRC}"/rv1106_download*.bin "${UBOOT_SRC}"/download*.bin; do
    [ -f "$f" ] && cp -v "$f" "${OUTPUT_DIR}/download.bin" && break
done
if [ ! -f "${OUTPUT_DIR}/download.bin" ]; then
    echo "WARNING: download.bin not found!"
fi

# trust.img -- may be embedded in uboot.img; tee.bin is the raw TEE blob
if [ -f "${UBOOT_SRC}/trust.img" ]; then
    cp -v "${UBOOT_SRC}/trust.img" "${OUTPUT_DIR}/trust.img"
elif [ -f "${UBOOT_SRC}/tee.bin" ]; then
    cp -v "${UBOOT_SRC}/tee.bin" "${OUTPUT_DIR}/trust.img"
    echo "NOTE: trust.img created from tee.bin"
fi

# Also copy any loader/*.bin artifacts
if ls "${UBOOT_SRC}"/*loader*.bin 1>/dev/null 2>&1; then
    cp -v "${UBOOT_SRC}"/*loader*.bin "${OUTPUT_DIR}/" || true
fi

# 6) Clean up build artifacts from source tree (but keep the source itself)
echo ""
echo "[*] Cleaning source tree build artifacts"
restore_dtsi   # no-op if step 3b never patched it, or the trap already ran
cd "${UBOOT_SRC}"
make distclean 2>/dev/null || true
rm -f "${DEFCONFIG_DST}"  # remove our local copy of the defconfig
rm -f uboot.img trust.img tee.bin *idblock*.img *download*.bin rv1106_idblock*.img rv1106_download*.bin *loader*.bin
rm -f .config .config.old .cc
rm -rf spl tpl
echo "[*] Cleanup done"

echo ""
echo "=========================================="
echo "U-Boot build complete!"
echo "Output files in: ${OUTPUT_DIR}"
ls -la "${OUTPUT_DIR}"
echo "=========================================="
