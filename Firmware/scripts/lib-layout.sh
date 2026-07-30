#!/bin/bash
# ---------------------------------------------------------------------------
# lib-layout.sh — runtime (build-time) partition-layout recalculation.
#
# The MCU firmware (rtthread.bin) is embedded inside uboot.itb, which the build
# pads to CONFIG_SPL_FIT_IMAGE_KB and flashes into the 'uboot' partition — so
# that partition MUST be at least as large as the FIT. As the firmware grows,
# both the FIT size and the uboot partition (and everything after it) have to
# grow with it. These helpers derive that size from the actual firmware, so the
# layout adapts at build time instead of a fixed value that silently overflows.
#
# Sourced by 01-build-uboot.sh (to size CONFIG_SPL_FIT_IMAGE_KB) and by
# 04-pack-image.sh (to size the uboot partition + shift meta/boot). Both read
# the same rtthread.bin, so they always agree.
# ---------------------------------------------------------------------------

# Base budget (KB) for U-Boot proper + FIT/DTB overhead inside uboot.itb.
# Measured ~183 KB on this board; 192 leaves headroom. If the FIT still overflows
# the uboot build fails loudly (raise this or trim the firmware) — never silent.
RK_UBOOT_BASE_KB="${RK_UBOOT_BASE_KB:-192}"

# rk_roundup_256 <kb> -> next multiple of 256 KB (partition granularity).
rk_roundup_256() { echo $(( ( ($1 + 255) / 256 ) * 256 )); }

# rk_uboot_fit_kb [rtthread.bin path] -> required uboot/FIT size in KB.
# Falls back to 256 (the historical minimum) when the firmware is absent.
rk_uboot_fit_kb() {
    local fw="${1:-}" fw_kb=0 need
    [ -f "$fw" ] && fw_kb=$(( ( $(stat -c %s "$fw") + 1023 ) / 1024 ))
    need=$(rk_roundup_256 $(( fw_kb + RK_UBOOT_BASE_KB )) )
    [ "$need" -lt 256 ] && need=256
    echo "$need"
}

# rk_partition_layout <uboot_kb> -> mtdparts string with uboot sized to
# <uboot_kb> and meta/boot shifted after it. env/idblock are fixed; with
# uboot_kb=256 this reproduces the historical layout byte-for-byte.
rk_partition_layout() {
    local u="$1" uboot_off=768 meta_off boot_off
    meta_off=$(( uboot_off + u ))       # meta starts right after uboot
    boot_off=$(( meta_off + 2048 ))     # boot starts after the 2 MB meta
    echo "256K(env),512K@256K(idblock),${u}K@768K(uboot),2M@${meta_off}K(meta),32M@${boot_off}K(boot),-(rootfs)"
}
