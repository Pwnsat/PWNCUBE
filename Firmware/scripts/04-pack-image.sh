#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# 04-pack-image.sh - Assemble the Rockchip update.img for RV1106
#
# This script:
#   1. Sources board config from configs/board/*.mk
#   2. Creates env.img from the partition layout using mkenvimage
#   3. Detects rootfs image format and normalizes to rootfs.img
#   4. Creates package-file for afptool
#   5. Runs afptool -pack to create a firmware blob
#   6. Runs rkImageMaker to produce the final update.img
#   7. Cleans up temporary files
#
# Prerequisites:
#   - tools/ directory with rkImageMaker, afptool, mkenvimage
#   - output/images/ with: download.bin, uboot.img, trust.img, boot.img,
#     and a rootfs image (rootfs_base.img, rootfs.ext4, rootfs.squashfs,
#     or rootfs.img)
#   - configs/board/*.mk with RK_PARTITION_CMD_IN_ENV (and optionally
#     RK_BOOT_MEDIUM) defined

set -eE

###############################################################################
# Paths
###############################################################################
SDK_DIR="$(cd "$(dirname "$(readlink -f "$0")")/.."; pwd)"
TOOLS_DIR="$SDK_DIR/tools"
OUTPUT_DIR="$SDK_DIR/output/images"
BOARD_CONFIG_DIR="$SDK_DIR/configs/board"
SCRIPT_NAME="$(basename "$0")"

###############################################################################
# Color helpers
###############################################################################
C_RED="\e[31;1m"
C_GREEN="\e[32;1m"
C_YELLOW="\e[33;1m"
C_CYAN="\e[36;1m"
C_NORMAL="\033[0m"

msg_info()  { echo -e "${C_GREEN}[${SCRIPT_NAME}:info] $*${C_NORMAL}"; }
msg_warn()  { echo -e "${C_YELLOW}[${SCRIPT_NAME}:warn] $*${C_NORMAL}"; }
msg_error() { echo -e "${C_RED}[${SCRIPT_NAME}:error] $*${C_NORMAL}"; }

###############################################################################
# Error handler
###############################################################################
err_handler() {
    local ret=$?
    [ "$ret" -eq 0 ] && return
    msg_error "Failed at line ${BASH_LINENO[0]}: $BASH_COMMAND"
    exit $ret
}
trap 'err_handler' ERR

###############################################################################
# Helper: check required tools
###############################################################################
check_tools() {
    local missing=0
    for tool in "$@"; do
        if [ ! -x "$TOOLS_DIR/$tool" ]; then
            msg_error "Required tool not found: $TOOLS_DIR/$tool"
            missing=1
        fi
    done
    if [ "$missing" -eq 1 ]; then
        msg_error "Missing required tools. Please run setup or copy tools first."
        exit 1
    fi
}

###############################################################################
# Helper: check required image files
###############################################################################
check_images() {
    local missing=0
    for img in "$@"; do
        if [ ! -f "$OUTPUT_DIR/$img" ]; then
            msg_warn "Image not found (optional?): $OUTPUT_DIR/$img"
        fi
    done
}

###############################################################################
# Helper: print rkImageMaker usage for verification
###############################################################################
verify_rkImageMaker_syntax() {
    if [ -x "$TOOLS_DIR/rkImageMaker" ]; then
        msg_info "Verifying rkImageMaker syntax (--help)..."
        "$TOOLS_DIR/rkImageMaker" 2>&1 | head -20 || true
        echo "---"
    fi
}

###############################################################################
# Main
###############################################################################
main() {
    msg_info "=== Rockchip update.img Packer for RV1106 ==="

    # ------------------------------------------------------------------
    # 0. Verify tools
    # ------------------------------------------------------------------
    check_tools rkImageMaker afptool mkenvimage
    verify_rkImageMaker_syntax

    # ------------------------------------------------------------------
    # 1. Source board configuration
    # ------------------------------------------------------------------
    BOARD_CONFIG=""
    # Find board config - prefer a specific one if BOARD env is set
    if [ -n "$BOARD" ] && [ -f "$BOARD_CONFIG_DIR/$BOARD" ]; then
        BOARD_CONFIG="$BOARD_CONFIG_DIR/$BOARD"
    else
        # Pick the first .mk file found
        for f in "$BOARD_CONFIG_DIR"/*.mk; do
            if [ -f "$f" ]; then
                BOARD_CONFIG="$f"
                break
            fi
        done
    fi

    if [ -z "$BOARD_CONFIG" ]; then
        msg_error "No board config found in $BOARD_CONFIG_DIR/"
        msg_error "Create configs/board/<name>.mk with RK_PARTITION_CMD_IN_ENV"
        exit 1
    fi

    msg_info "Loading board config: $BOARD_CONFIG"
    source "$BOARD_CONFIG"

    # Partition layout from board config (must be defined)
    if [ -z "$RK_PARTITION_CMD_IN_ENV" ]; then
        msg_error "RK_PARTITION_CMD_IN_ENV not defined in $BOARD_CONFIG"
        msg_error "Example: 32K(env),512K@32K(idblock),256K(uboot),32M(boot),256M(userdata),-(rootfs)"
        exit 1
    fi

    # Recalculate the layout at build time from the actual MCU firmware size:
    # the uboot partition (and meta/boot after it) must be large enough to hold
    # the rtthread.bin embedded in uboot.itb. Must match the CONFIG_SPL_FIT_IMAGE_KB
    # that 01-build-uboot.sh derived from the same firmware.
    source "$SDK_DIR/scripts/lib-layout.sh"
    _fit_kb=$(rk_uboot_fit_kb "$SDK_DIR/src/rkbin/bin/rv11/rtthread.bin")
    RK_PARTITION_CMD_IN_ENV="$(rk_partition_layout "$_fit_kb")"
    msg_info "Firmware-driven layout: uboot=${_fit_kb}K"
    msg_info "Partition layout: $RK_PARTITION_CMD_IN_ENV"

    # Boot medium
    RK_BOOT_MEDIUM="${RK_BOOT_MEDIUM:-sd_card}"
    msg_info "Boot medium: $RK_BOOT_MEDIUM"

    # Output directory
    mkdir -p "$OUTPUT_DIR"

    # ------------------------------------------------------------------
    # 2. Create env.img
    # ------------------------------------------------------------------
    msg_info "Creating env.img with mkenvimage..."

    # Determine the partition table prefix string based on boot medium
    case "$RK_BOOT_MEDIUM" in
        spi_nand) BLKDEV_PREFIX="mtdparts=spi-nand0" ;;
        spi_nor)  BLKDEV_PREFIX="mtdparts=sfc_nor" ;;
        *)        BLKDEV_PREFIX="mtdparts=spi-nand0" ;;
    esac

    # Determine root device based on boot medium & partition count
    # Count partitions in layout to determine rootfs partition index:
    #   env(0) idblock(1) uboot(2) boot(3) rootfs(4) → mtdblock4
    local part_count=0
    IFS=, read -ra PARTS <<< "$RK_PARTITION_CMD_IN_ENV"
    for part in "${PARTS[@]}"; do
        part_count=$((part_count + 1))
    done
    local rootfs_idx=$((part_count - 1))
    case "$RK_BOOT_MEDIUM" in
        spi_nand) ROOT_DEV="/dev/mtdblock${rootfs_idx}" ;;
        spi_nor)  ROOT_DEV="/dev/mtdblock${rootfs_idx}" ;;
        *)        ROOT_DEV="/dev/mtdblock${rootfs_idx}" ;;
    esac

    # Default rootfstype
    ROOTFS_TYPE="${RK_ROOTFS_TYPE:-ext4}"

    # Create env.txt
    ENV_TXT="$OUTPUT_DIR/.env.txt"
    cat > "$ENV_TXT" <<EOF
${BLKDEV_PREFIX}:${RK_PARTITION_CMD_IN_ENV}
sys_bootargs=root=${ROOT_DEV} rootfstype=${ROOTFS_TYPE}
EOF
    msg_info "Environment file:"
    cat "$ENV_TXT"

    # Calculate env partition size from layout
    # Format example: 32K(env),512K@32K(idblock),...
    ENV_SIZE=""
    IFS=, read -ra PARTS <<< "$RK_PARTITION_CMD_IN_ENV"
    for part in "${PARTS[@]}"; do
        if [[ "$part" =~ \([Ee][Nn][Vv]\) ]]; then
            ENV_SIZE=$(echo "$part" | cut -d '(' -f1 | cut -d '@' -f1)
            break
        fi
    done

    if [ -z "$ENV_SIZE" ]; then
        msg_warn "Could not determine env size from partition layout, using 32K"
        ENV_SIZE="32K"
    fi

    # Convert size to bytes
    local suffix="${ENV_SIZE: -1}"
    local num="${ENV_SIZE%$suffix}"
    case "$suffix" in
        K|k) ENV_SIZE_BYTES=$((num * 1024)) ;;
        M|m) ENV_SIZE_BYTES=$((num * 1024 * 1024)) ;;
        *)   ENV_SIZE_BYTES=$((ENV_SIZE)) ;;
    esac

    msg_info "env partition size: $ENV_SIZE ($ENV_SIZE_BYTES bytes)"

    # Generate env.img
    "$TOOLS_DIR/mkenvimage" -s "$ENV_SIZE_BYTES" -p 0x0 -o "$OUTPUT_DIR/env.img" "$ENV_TXT"
    chmod +r "$OUTPUT_DIR/env.img"
    msg_info "Created env.img: $(ls -lh "$OUTPUT_DIR/env.img" | awk '{print $5}')"

    # ------------------------------------------------------------------
    # 3. Detect rootfs image and normalize to rootfs.img
    # ------------------------------------------------------------------
    msg_info "Detecting rootfs image..."

    # Priority order for rootfs images:
    #   rootfs_base.img  (from buildroot SDK naming)
    #   rootfs.ext4       (raw ext4 image)
    #   rootfs.squashfs   (squashfs image)
    #   rootfs.ubifs      (ubifs image)
    #   rootfs.img        (already the target name)
    ROOTFS_SRC=""
    for candidate in rootfs_base.img rootfs.ext4 rootfs.squashfs rootfs.ubifs rootfs.img; do
        if [ -f "$OUTPUT_DIR/$candidate" ]; then
            ROOTFS_SRC="$OUTPUT_DIR/$candidate"
            msg_info "Found rootfs image: $candidate"
            break
        fi
    done

    if [ -z "$ROOTFS_SRC" ]; then
        msg_warn "No rootfs image found in $OUTPUT_DIR/"
        msg_warn "Looked for: rootfs_base.img, rootfs.ext4, rootfs.squashfs, rootfs.ubifs, rootfs.img"
        msg_warn "Will continue without rootfs in package-file (update.img will be incomplete)"
    fi

    # Normalize to rootfs.img (copy/link if name differs)
    if [ -n "$ROOTFS_SRC" ]; then
        local rootfs_dst="$OUTPUT_DIR/rootfs.img"
        if [ "$(readlink -f "$ROOTFS_SRC")" != "$(readlink -f "$rootfs_dst" 2>/dev/null || echo "")" ]; then
            msg_info "Normalizing $ROOTFS_SRC -> $rootfs_dst"
            rm -f "$rootfs_dst"
            cp -f "$ROOTFS_SRC" "$rootfs_dst"
        else
            msg_info "rootfs.img already points to rootfs_base.img"
        fi
    fi

    # Detect rootfs type for sys_bootargs
    if [ -n "$ROOTFS_SRC" ]; then
        local fname
        fname="$(basename "$ROOTFS_SRC")"
        case "$fname" in
            *.ext4|*ext4*)   ROOTFS_TYPE="ext4" ;;
            *.squashfs)      ROOTFS_TYPE="squashfs" ;;
            *.ubifs)         ROOTFS_TYPE="ubifs" ;;
            *.cpio*)         ROOTFS_TYPE="initramfs" ;;
            *.erofs)         ROOTFS_TYPE="erofs" ;;
            *.jffs2)         ROOTFS_TYPE="jffs2" ;;
            *)               ROOTFS_TYPE="${ROOTFS_TYPE:-ext4}" ;;
        esac
        msg_info "Detected rootfs type: $ROOTFS_TYPE"
    fi

    # ------------------------------------------------------------------
    # 4. Check required images
    # ------------------------------------------------------------------
    msg_info "Checking for required component images..."
    check_images download.bin env.img uboot.img trust.img boot.img rootfs.img

    # ------------------------------------------------------------------
    # 5. Create package-file for afptool
    # ------------------------------------------------------------------
    msg_info "Creating package-file for afptool..."

    PACKAGE_FILE="$OUTPUT_DIR/package-file"
    # Build package-file entries - use partition names from board config
    > "$PACKAGE_FILE"
    echo -e "package-file\tpackage-file" >> "$PACKAGE_FILE"
    echo -e "bootloader\tdownload.bin" >> "$PACKAGE_FILE"
    echo -e "env\tenv.img" >> "$PACKAGE_FILE"
    for entry in idblock.img uboot.img meta.img trust.img boot.img rootfs.img userdata.img; do
        if [ -f "$OUTPUT_DIR/$entry" ]; then
            local part_name="${entry%.img}"
            echo -e "${part_name}\t${entry}" >> "$PACKAGE_FILE"
        fi
    done

    msg_info "package-file contents:"
    cat "$PACKAGE_FILE"

    # ------------------------------------------------------------------
    # 6. Run afptool -pack
    # ------------------------------------------------------------------
    msg_info "Running afptool -pack ..."
    "$TOOLS_DIR/afptool" -pack "$OUTPUT_DIR" "$OUTPUT_DIR/update_tmp.img"
    msg_info "afptool -pack succeeded: $(ls -lh "$OUTPUT_DIR/update_tmp.img" | awk '{print $5}')"

    # ------------------------------------------------------------------
    # 7. Run rkImageMaker
    # ------------------------------------------------------------------
    if [ ! -f "$OUTPUT_DIR/download.bin" ]; then
        msg_error "download.bin (bootloader) not found in $OUTPUT_DIR/"
        msg_error "Cannot create update.img without the bootloader."
        exit 1
    fi

    msg_info "Running rkImageMaker -RK1106 ..."
    "$TOOLS_DIR/rkImageMaker" -RK1106 "$OUTPUT_DIR/download.bin" \
        "$OUTPUT_DIR/update_tmp.img" "$OUTPUT_DIR/update.img" -os_type:androidos
    msg_info "rkImageMaker succeeded!"

    # ------------------------------------------------------------------
    # 8. Cleanup temporary files
    # ------------------------------------------------------------------
    msg_info "Cleaning up temporary files..."
    rm -f "$OUTPUT_DIR/update_tmp.img"
    rm -f "$OUTPUT_DIR/package-file"
    rm -f "$OUTPUT_DIR/.env.txt"

    msg_info ""
    msg_info "=== Packaging complete ==="
    msg_info "Output: $OUTPUT_DIR/update.img"
    ls -lh "$OUTPUT_DIR/update.img"
}

main "$@"
