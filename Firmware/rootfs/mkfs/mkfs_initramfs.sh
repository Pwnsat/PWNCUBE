#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# mkfs_initramfs.sh - Create an initramfs cpio archive from a source directory
#
# Usage: mkfs_initramfs.sh <source_dir> <output_cpio>
#
# Adapted from Luckfox-Pico SDK: sysdrv/tools/pc/initramfs/mkfs_initramfs.sh

set -e

err_handler() {
    ret=$?
    [ "$ret" -eq 0 ] && return
    echo "[mkfs_initramfs] ERROR: exit code $ret from line ${BASH_LINENO[0]}" >&2
    exit $ret
}
trap 'err_handler' ERR

src=$1
dst=$2

if [ -z "$src" ] || [ -z "$dst" ]; then
    echo "Usage: $(basename $0) <source_dir> <dest_cpio>"
    exit 1
fi

if [ ! -d "$src" ]; then
    echo "ERROR: source directory '$src' not found" >&2
    exit 1
fi

if ! command -v cpio >/dev/null 2>&1; then
    echo "ERROR: cpio not found. Please install cpio first." >&2
    exit 1
fi

rm -f "$dst"
mkdir -p "$(dirname "$dst")"

# Package rootfs in cpio (newc format)
(cd "$src"; find . | cpio --quiet -o -H newc > "$dst")

echo "[mkfs_initramfs] Created: $dst ($(du -h "$dst" | cut -f1))"
