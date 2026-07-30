#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# mkfs_squashfs.sh - Create a squashfs rootfs image from a source directory
#
# Usage: mkfs_squashfs.sh <source_dir> <output_image> [compression]
#
# Compression options: lz4, lzo, lzma, xz, gzip (default: xz)
#
# Adapted from Luckfox-Pico SDK: sysdrv/tools/pc/mksquashfs/mkfs_squashfs.sh

set -e

err_handler() {
    ret=$?
    [ "$ret" -eq 0 ] && return
    echo "[mkfs_squashfs] ERROR: exit code $ret from line ${BASH_LINENO[0]}" >&2
    exit $ret
}
trap 'err_handler' ERR

src=$1
dst=$2
SQUASHFS4_COMP=${3:-xz}

if [ -z "$src" ] || [ -z "$dst" ]; then
    echo "Usage: $(basename $0) <source_dir> <dest_image> [compression]"
    echo "  compression: lz4|lzo|lzma|xz|gzip (default: xz)"
    exit 1
fi

if [ ! -d "$src" ]; then
    echo "ERROR: source directory '$src' not found" >&2
    exit 1
fi

case $SQUASHFS4_COMP in
    lz4|lzo|lzma|xz|gzip)
        squashfs_compression_args="$SQUASHFS4_COMP"
        ;;
    *)
        squashfs_compression_args=xz
        ;;
esac

export PATH="$(dirname "$(readlink -f "$0")"):$PATH"
MKSQUASHFS_TOOL=mksquashfs

rm -f "$dst"
mkdir -p "$(dirname "$dst")"

parallel_jobs=$((1 + $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)))

if [ "$squashfs_compression_args" = "lz4" ]; then
    echo "$MKSQUASHFS_TOOL '$src' '$dst' -noappend -processors $parallel_jobs -comp $squashfs_compression_args -Xhc"
    $MKSQUASHFS_TOOL "$src" "$dst" -noappend -processors $parallel_jobs -comp $squashfs_compression_args -Xhc
else
    echo "$MKSQUASHFS_TOOL '$src' '$dst' -noappend -processors $parallel_jobs -comp $squashfs_compression_args"
    $MKSQUASHFS_TOOL "$src" "$dst" -noappend -processors $parallel_jobs -comp $squashfs_compression_args
fi

echo "[mkfs_squashfs] Created: $dst ($(du -h "$dst" | cut -f1))"
