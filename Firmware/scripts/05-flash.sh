#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# 05-flash.sh - Flash Rockchip RV1106 firmware to target device
#
# This script flashes the assembled firmware to a Rockchip RV1106 device
# via USB using upgrade_tool.
#
# Usage:
#   ./scripts/05-flash.sh                    # Flash full update.img
#   ./scripts/05-flash.sh update             # Flash full update.img
#   ./scripts/05-flash.sh individual         # Flash individual partitions
#   ./scripts/05-flash.sh loader <file>      # Flash bootloader only
#   ./scripts/05-flash.sh uboot <file>       # Flash uboot only
#   ./scripts/05-flash.sh boot <file>        # Flash boot only
#   ./scripts/05-flash.sh trust <file>       # Flash trust only
#   ./scripts/05-flash.sh rootfs <file>      # Flash rootfs only
#   ./scripts/05-flash.sh list               # List connected devices
#
# Prerequisites:
#   - tools/upgrade_tool (in SDK tools directory)
#   - Device connected via USB in Rockchip download mode
#   - For full flash: output/images/update.img

set -eE

###############################################################################
# Paths
###############################################################################
SDK_DIR="$(cd "$(dirname "$(readlink -f "$0")")/.."; pwd)"
TOOLS_DIR="$SDK_DIR/tools"
OUTPUT_DIR="$SDK_DIR/output/images"
UPGRADE_TOOL="$TOOLS_DIR/upgrade_tool"
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
# Confirmation prompt
###############################################################################
confirm() {
    local prompt="$1"
    echo -e "${C_YELLOW}$prompt${C_NORMAL}"
    read -r -p "Are you sure? [y/N] " response
    case "$response" in
        [yY][eE][sS]|[yY])
            return 0
            ;;
        *)
            msg_info "Aborted."
            exit 1
            ;;
    esac
}

###############################################################################
# Check device connection
###############################################################################
check_device() {
    msg_info "Checking for connected Rockchip devices..."
    if ! "$UPGRADE_TOOL" LD 2>&1 | grep -qi "rockusb\|loader\|maskrom\|device"; then
        # Try to list devices anyway
        if "$UPGRADE_TOOL" LD 2>&1 | head -5 | grep -q "No\|not\|error"; then
            msg_warn "No Rockchip device detected."
            msg_warn "Ensure your device is connected via USB and in download mode."
            msg_warn "  - For empty devices: connect USB and power on"
            msg_warn "  - For devices with bootloader: hold recovery button while powering on"
            msg_info "Continuing anyway (device may appear later)..."
        else
            msg_info "Device appears to be connected."
        fi
    else
        msg_info "Device detected."
    fi
    "$UPGRADE_TOOL" LD 2>&1 || true
}

###############################################################################
# Flash full update.img
###############################################################################
flash_update() {
    local img="$OUTPUT_DIR/update.img"

    if [ -n "$1" ]; then
        img="$1"
    fi

    if [ ! -f "$img" ]; then
        msg_error "update.img not found: $img"
        msg_error "Run ./scripts/04-pack-image.sh first to create it."
        exit 1
    fi

    msg_info "Flashing full firmware: $(ls -lh "$img" | awk '{print $5}')"
    confirm "This will OVERWRITE all partitions on the device."

    msg_info "Running: upgrade_tool UF $img"
    "$UPGRADE_TOOL" UF "$img"
    msg_info "Firmware flash complete!"
}

###############################################################################
# Flash individual partitions
###############################################################################
flash_individual() {
    msg_info "Flashing individual partitions..."

    # Default image paths
    local loader="$OUTPUT_DIR/download.bin"
    local parameter="$OUTPUT_DIR/env.img"
    local uboot="$OUTPUT_DIR/uboot.img"
    local trust="$OUTPUT_DIR/trust.img"
    local boot="$OUTPUT_DIR/boot.img"
    local rootfs="$OUTPUT_DIR/rootfs.img"

    # Check which files exist
    local missing=0
    for f in "$loader" "$parameter" "$uboot" "$trust" "$boot" "$rootfs"; do
        if [ ! -f "$f" ]; then
            msg_warn "Missing: $f"
            missing=1
        fi
    done

    if [ "$missing" -eq 1 ]; then
        msg_warn "Some images are missing. Flashing will continue with available images."
    fi

    confirm "This will flash individual partitions to the device."

    # Step 1: Download bootloader
    if [ -f "$loader" ]; then
        msg_info "Flashing bootloader (download.bin)..."
        "$UPGRADE_TOOL" UL "$loader"
    fi

    # Step 2: Flash parameter (env.img with partition table)
    if [ -f "$parameter" ]; then
        msg_info "Flashing parameter (env.img)..."
        "$UPGRADE_TOOL" DI -p "$parameter"
    fi

    # Step 3: Flash uboot
    if [ -f "$uboot" ]; then
        msg_info "Flashing uboot..."
        "$UPGRADE_TOOL" DI -uboot "$uboot"
    fi

    # Step 4: Flash trust
    if [ -f "$trust" ]; then
        msg_info "Flashing trust..."
        "$UPGRADE_TOOL" DI -t "$trust"
    fi

    # Step 5: Flash boot
    if [ -f "$boot" ]; then
        msg_info "Flashing boot..."
        "$UPGRADE_TOOL" DI -b "$boot"
    fi

    # Step 6: Flash rootfs
    if [ -f "$rootfs" ]; then
        msg_info "Flashing rootfs..."
        "$UPGRADE_TOOL" DI -rootfs "$rootfs"
    fi

    # Step 7: Reset device
    msg_info "Resetting device..."
    "$UPGRADE_TOOL" RD

    msg_info "Individual partition flash complete!"
}

###############################################################################
# Flash a single component
###############################################################################
flash_component() {
    local component="$1"
    local file="$2"

    if [ ! -f "$file" ]; then
        msg_error "File not found: $file"
        exit 1
    fi

    confirm "Flash $component from $file?"

    case "$component" in
        loader)
            "$UPGRADE_TOOL" UL "$file"
            ;;
        uboot)
            "$UPGRADE_TOOL" DI -uboot "$file"
            ;;
        boot)
            "$UPGRADE_TOOL" DI -b "$file"
            "$UPGRADE_TOOL" RD
            ;;
        trust)
            "$UPGRADE_TOOL" DI -t "$file"
            ;;
        rootfs)
            "$UPGRADE_TOOL" DI -rootfs "$file"
            ;;
        parameter)
            "$UPGRADE_TOOL" DI -p "$file"
            ;;
        *)
            msg_error "Unknown component: $component"
            msg_info "Valid: loader, uboot, boot, trust, rootfs, parameter"
            exit 1
            ;;
    esac

    msg_info "Component '$component' flashed successfully."
}

###############################################################################
# Usage
###############################################################################
usage() {
    cat <<EOF
Usage: $SCRIPT_NAME [command] [args]

Commands:
  update [path]      Flash full update.img (default: output/images/update.img)
  individual         Flash individual partitions (loader + parameter + uboot + trust + boot + rootfs)
  loader <file>      Flash bootloader only
  uboot <file>       Flash uboot only
  boot <file>        Flash boot only
  trust <file>       Flash trust only
  rootfs <file>      Flash rootfs only
  parameter <file>   Flash parameter (env.img) only
  list               List connected Rockchip devices
  help               Show this help

Examples:
  $SCRIPT_NAME                     Flash full update.img
  $SCRIPT_NAME update              Flash full update.img
  $SCRIPT_NAME update /path/to/update.img   Flash custom update.img
  $SCRIPT_NAME individual          Flash partitions individually
  $SCRIPT_NAME uboot output/images/uboot.img
  $SCRIPT_NAME list
EOF
}

###############################################################################
# Main
###############################################################################
main() {
    if [ ! -x "$UPGRADE_TOOL" ]; then
        msg_error "upgrade_tool not found or not executable: $UPGRADE_TOOL"
        exit 1
    fi

    local cmd="${1:-update}"
    shift 2>/dev/null || true

    case "$cmd" in
        update|full)
            check_device
            flash_update "$@"
            ;;
        individual|parts)
            check_device
            flash_individual
            ;;
        loader|uboot|boot|trust|rootfs|parameter)
            local file="${1:-}"
            if [ -z "$file" ]; then
                # Default path based on component
                case "$cmd" in
                    loader)    file="$OUTPUT_DIR/download.bin" ;;
                    uboot)     file="$OUTPUT_DIR/uboot.img" ;;
                    boot)      file="$OUTPUT_DIR/boot.img" ;;
                    trust)     file="$OUTPUT_DIR/trust.img" ;;
                    rootfs)    file="$OUTPUT_DIR/rootfs.img" ;;
                    parameter) file="$OUTPUT_DIR/env.img" ;;
                esac
            fi
            check_device
            flash_component "$cmd" "$file"
            ;;
        list|ls)
            "$UPGRADE_TOOL" LD
            ;;
        help|--help|-h)
            usage
            ;;
        *)
            msg_error "Unknown command: $cmd"
            usage
            exit 1
            ;;
    esac
}

main "$@"
