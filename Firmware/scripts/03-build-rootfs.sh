#!/bin/bash
#
# 03-build-rootfs.sh - Build root filesystem for RV1106 SDK
#
# This script:
#   1. Builds Busybox with the tiny ARM config
#   2. Staging rootfs from skeleton + busybox _install + runtime libs
#   3. Strips binaries and removes debug libraries
#   4. Creates rootfs ext4 image
#   5. Installs packages via pkg system
#

set -e

# Source the project environment
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

# Default configuration
: "${CROSS_COMPILE:=arm-rockchip830-linux-uclibcgnueabihf-}"
: "${TOOLCHAIN_DIR:=${SDK_DIR}/toolchain/arm-rockchip830-linux-uclibcgnueabihf}"
: "${BUSYBOX_DIR:=${SDK_DIR}/src/busybox}"
: "${SKELETON_DIR:=${SDK_DIR}/rootfs/skeleton}"
: "${INIT_SCRIPT:=${SDK_DIR}/rootfs/init-scripts/init}"
: "${OUTPUT_DIR:=${SDK_DIR}/output}"
: "${ROOTFS_DIR:=${OUTPUT_DIR}/rootfs}"
: "${IMAGES_DIR:=${OUTPUT_DIR}/images}"
: "${ROOTFS_IMG:=${IMAGES_DIR}/rootfs_base.img}"
: "${ROOTFS_SYMLINK:=${IMAGES_DIR}/rootfs.img}"
: "${IMAGE_SIZE_MB:=64}"
: "${PKG_SCRIPT:=${SDK_DIR}/pkg/pkg.sh}"

log() {
    echo "[03-build-rootfs] $*"
}

# ------------------------------------------------------------------
# 1. Build Busybox
# ------------------------------------------------------------------
build_busybox() {
    local bb_src="${OUTPUT_DIR}/busybox-1.27.2"

    log "Building Busybox..."

    # Extract tarball if not already done
    if [ ! -d "${bb_src}" ]; then
        log "Extracting busybox tarball..."
        mkdir -p "${OUTPUT_DIR}"
        tar -xjf "${BUSYBOX_DIR}/busybox-1.27.2.tar.bz2" -C "${OUTPUT_DIR}"
    fi

    cd "${bb_src}"

    # Apply patches if not yet patched
    if [ ! -f "busybox_patched_done" ]; then
        log "Applying Busybox patches..."
        for patchfile in "${BUSYBOX_DIR}"/0001-*.patch \
                         "${BUSYBOX_DIR}"/0002-*.patch \
                         "${BUSYBOX_DIR}"/0003-*.patch \
                         "${BUSYBOX_DIR}"/0004-*.patch \
                         "${BUSYBOX_DIR}"/0005-*.patch \
                         "${BUSYBOX_DIR}"/0006-*.patch \
                         "${BUSYBOX_DIR}"/0007-*.patch \
                         "${BUSYBOX_DIR}"/0008-*.patch \
                         "${BUSYBOX_DIR}"/0009-*.patch \
                         "${BUSYBOX_DIR}"/0010-*.patch; do
            if [ -f "${patchfile}" ]; then
                patch -p1 < "${patchfile}"
            fi
        done
        touch busybox_patched_done
    else
        log "Busybox patches already applied, skipping."
    fi

    # Use the tiny ARM configuration
    cp "${BUSYBOX_DIR}/config_tiny_arm" .config

    # If CROSS_COMPILE is set, update the config
    if [ -n "${CROSS_COMPILE}" ]; then
        sed -i "s|CONFIG_CROSS_COMPILER_PREFIX=.*|CONFIG_CROSS_COMPILER_PREFIX=\"${CROSS_COMPILE}\"|" .config
    fi

    # Build Busybox
    make oldconfig
    make -j$(nproc)

    # Install to _install directory
    make install

    log "Busybox build complete."
    cd "${SCRIPT_DIR}"
}

# ------------------------------------------------------------------
# 2. Create rootfs staging area
# ------------------------------------------------------------------
prepare_rootfs() {
    log "Preparing rootfs staging at ${ROOTFS_DIR}..."

    # Clean and recreate staging directory
    rm -rf "${ROOTFS_DIR}"
    mkdir -p "${ROOTFS_DIR}"

    # Copy skeleton files
    log "Copying skeleton..."
    cp -a "${SKELETON_DIR}"/* "${ROOTFS_DIR}/"

    # Create base FHS mount points / runtime dirs.
    # These are kernel/tmpfs mount points (mounted by rcS+fstab) and must exist
    # in the image. Git cannot track empty directories, so create them here
    # instead of relying on empty dirs in the skeleton.
    log "Creating base directories (mount points)..."
    mkdir -p "${ROOTFS_DIR}"/{dev,proc,sys,run,mnt,opt,home}
    install -d -m 1777 "${ROOTFS_DIR}/tmp"
    install -d -m 1777 "${ROOTFS_DIR}/var"
    install -d -m 0700 "${ROOTFS_DIR}/root"

    # Copy busybox _install
    log "Copying Busybox _install..."
    cp -a "${OUTPUT_DIR}/busybox-1.27.2/_install"/* "${ROOTFS_DIR}/"

    # Copy linuxrc / init
    log "Copying init script..."
    cp "${INIT_SCRIPT}" "${ROOTFS_DIR}/init"
    chmod +x "${ROOTFS_DIR}/init"

    # Extract runtime libraries from toolchain
    local runtime_lib="${TOOLCHAIN_DIR}/runtime_lib/lib.tar.bz2"
    if [ -f "${runtime_lib}" ]; then
        log "Extracting runtime libraries..."
        tar -xjf "${runtime_lib}" -C "${ROOTFS_DIR}/"
    else
        log "WARNING: Runtime lib tarball not found at ${runtime_lib}"
    fi

    # Remove debug/sanitizer libraries
    log "Removing debug/sanitizer libraries..."
    for dbg_lib in libasan libtsan libubsan liblsan libhwasan libgcov; do
        find "${ROOTFS_DIR}/lib" -name "${dbg_lib}*" -type f -delete 2>/dev/null || true
    done

    log "Rootfs staging prepared."
}

# ------------------------------------------------------------------
# 3. Strip binaries
# ------------------------------------------------------------------
strip_rootfs() {
    log "Stripping binaries with ${CROSS_COMPILE}strip..."

    if command -v "${CROSS_COMPILE}strip" &>/dev/null; then
        find "${ROOTFS_DIR}" -type f \( -perm /111 -o -name '*.so*' \) | while read -r f; do
            if file "${f}" | grep -q "ELF"; then
                "${CROSS_COMPILE}strip" --strip-unneeded "${f}" 2>/dev/null || true
            fi
        done
        log "Stripping complete."
    else
        log "WARNING: ${CROSS_COMPILE}strip not found. Skipping strip."
    fi
}

# ------------------------------------------------------------------
# 4. Create rootfs ext4 image
# ------------------------------------------------------------------
create_image() {
    log "Creating rootfs image: ${ROOTFS_IMG}"

    mkdir -p "${IMAGES_DIR}"

    # Calculate size based on rootfs contents
    local dir_size_kb
    dir_size_kb=$(du -sk "${ROOTFS_DIR}" | cut -f1)
    local img_size_kb=$(( dir_size_kb + (dir_size_kb / 4) + 8192 ))   # contents + 25% + 8MB padding
    # Ensure minimum size
    if [ "${img_size_kb}" -lt $((IMAGE_SIZE_MB * 1024)) ]; then
        img_size_kb=$((IMAGE_SIZE_MB * 1024))
    fi

    log "Image size: ${img_size_kb} KB"

    # Create ext4 filesystem image, populated from rootfs staging directory
    mkfs.ext4 -F -L "rootfs" -d "${ROOTFS_DIR}" "${ROOTFS_IMG}" "${img_size_kb}k"

    # Create symlink
    ln -sf "rootfs_base.img" "${ROOTFS_SYMLINK}"

    log "Rootfs image created: ${ROOTFS_IMG}"
    ls -lh "${ROOTFS_IMG}"
}

# ------------------------------------------------------------------
# 5. Install kernel modules from staging
# ------------------------------------------------------------------
install_modules() {
    local modules_staging="${OUTPUT_DIR}/modules"
    if [ -d "${modules_staging}" ]; then
        log "Installing kernel modules from staging..."
        cp -a "${modules_staging}/"* "${ROOTFS_DIR}/"
        log "Kernel modules installed."
    else
        log "No modules staging found at ${modules_staging}, skipping."
    fi
}

# ------------------------------------------------------------------
# 6. Install packages
# ------------------------------------------------------------------
install_packages() {
    if [ -f "${PKG_SCRIPT}" ]; then
        log "Installing packages via ${PKG_SCRIPT} install-all..."
        bash "${PKG_SCRIPT}" install-all
    else
        log "WARNING: Package script not found at ${PKG_SCRIPT}. Skipping package installation."
    fi
}

# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------
main() {
    log "=== Starting rootfs build ==="

    build_busybox
    prepare_rootfs
    strip_rootfs
    install_modules
    install_packages
    create_image

    log "=== Rootfs build complete ==="
    log "Image: ${ROOTFS_IMG}"
    log "Symlink: ${ROOTFS_SYMLINK}"
}

main "$@"
