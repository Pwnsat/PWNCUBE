#!/bin/bash
# RV1106 SDK - Entry point
set -euo pipefail
cd "$(dirname "$0")"

# 'deps' needs no toolchain — handle it before sourcing anything that checks for one.
if [ "${1:-}" = "deps" ]; then exec bash scripts/install-deps.sh; fi

source scripts/00-setup-toolchain.sh
source scripts/functions.sh

case "${1:-}" in
    "")          bash scripts/build-all.sh ;;
    help|-h|--help)
        echo "Usage: $0 <command>"
        echo ""
        echo "Commands:"
        echo "  deps           Install all host build dependencies (apt, one command)"
        echo "  (no args)      Full build: clean + uboot + kernel + rootfs + packages + pack"
        echo "  rebuild        Same as full build (clean + build all)"
        echo "  clean          Remove output/ and distclean uboot/kernel"
        echo "  uboot          Build U-Boot + rkbin (idblock, uboot, trust)"
        echo "  kernel         Build kernel + DTB + FIT boot image"
        echo "  rootfs         Build busybox + staging rootfs"
        echo "  mcu            Build RISC-V coprocessor firmware (RT-Thread, rtthread.bin)"
        echo "  pack           Package update.img from built components"
        echo "  flash          Flash update.img via upgrade_tool (needs sudo)"
        echo "  menuconfig     Enable/disable packages (openssl, dropbear, etc)"
        echo "  info           Show build configuration"
        ;;
    clean)
        log "Cleaning..."; rm -rf output/
        make -C "$UBOOT_SRC" CROSS_COMPILE="$CROSS_COMPILE" distclean &>/dev/null || true
        make -C "$KERNEL_SRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" distclean &>/dev/null || true
        log "Done." ;;
    rebuild)     bash scripts/build-all.sh ;;
    uboot)       BOOT_MEDIUM="${RK_BOOT_MEDIUM:-}" bash scripts/01-build-uboot.sh "${2:-}" ;;
    kernel)      bash scripts/02-build-kernel.sh "${2:-}" ;;
    rootfs)      bash scripts/03-build-rootfs.sh ;;
    mcu)         bash scripts/06-build-mcu.sh "${2:-}" ;;
    pack)        bash scripts/04-pack-image.sh ;;
    flash)       bash scripts/05-flash.sh ;;
    menuconfig)  bash pkg/pkg.sh menuconfig ;;
    info)        log "ARCH=$ARCH CROSS=$CROSS_COMPILE DTS=${KERNEL_DTS:-rv1106g-sdk} BOARD=${RK_BOOT_MEDIUM:-spi_nand}" ;;
    *)           bash "$0" help; exit 1 ;;
esac
