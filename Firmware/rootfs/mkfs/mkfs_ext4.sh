#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# mkfs_ext4.sh - Create an ext4 rootfs image from a source directory
#
# Usage: mkfs_ext4.sh <source_dir> <output_image> <partition_size_bytes>
#
# Adapted from Luckfox-Pico SDK: sysdrv/tools/pc/e2fsprogs/mkfs_ext4.sh

set -e

err_handler() {
    ret=$?
    [ "$ret" -eq 0 ] && return
    echo "[mkfs_ext4] ERROR: exit code $ret from line ${BASH_LINENO[0]}" >&2
    exit $ret
}
trap 'err_handler' ERR

src=$1
dst=$2
part_size=$3

if [ -z "$src" ] || [ -z "$dst" ] || [ -z "$part_size" ]; then
    echo "Usage: $(basename $0) <source_dir> <dest_image> <partition_size_bytes>"
    exit 1
fi

if [ ! -d "$src" ]; then
    echo "ERROR: source directory '$src' not found" >&2
    exit 1
fi

dst_size="$(( part_size / 1024 / 1024 ))M"

rm -f "$dst"
mkdir -p "$(dirname "$dst")"

echo "mkfs.ext4 -d '$src' -r 1 -N 0 -m 5 -L '' -O ^64bit,^huge_file '$dst' '$dst_size'"
mkfs.ext4 -d "$src" -r 1 -N 0 -m 5 -L "" -O ^64bit,^huge_file "$dst" "$dst_size"

echo "resize2fs -M '$dst'"
resize2fs -M "$dst"

echo "e2fsck -fy '$dst'"
e2fsck -fy "$dst"

echo "tune2fs -m 5 '$dst'"
tune2fs -m 5 "$dst"

echo "resize2fs -M '$dst'"
resize2fs -M "$dst"

echo "[mkfs_ext4] Created: $dst ($(du -h "$dst" | cut -f1))"
