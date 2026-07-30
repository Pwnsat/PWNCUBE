PKG_NAME="icm42670"
PKG_VERSION="1.0"
PKG_LICENSE="GPL-2.0"
PKG_DEPENDS=""
PKG_DESCRIPTION="ICM-42670-P 6-axis IMU kernel driver (icm42670.ko)"

# Kernel version the module is built/installed for
ICM42670_KVER="5.10.160"

pkg_build() {
    local ksrc="${BASE_DIR}/src/kernel"
    local kobj="${BASE_DIR}/output/objs_kernel"
    local kmod="${BASE_DIR}/src/icm42670-kmod"

    [ -d "${kmod}" ] || { echo "Source not found: src/icm42670-kmod"; return 1; }
    [ -d "${kobj}" ] || { echo "Kernel not built (${kobj} missing) — build the kernel first"; return 1; }

    # --- kernel module (icm42670.ko) ---
    make -C "${ksrc}" O="${kobj}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
        modules_prepare 2>&1 | grep -v "Nothing to be done" || true
    make -C "${kmod}" KERNEL_SRC="${ksrc}" O="${kobj}" \
        ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" clean all
}

pkg_install() {
    install -d "${PKG_INSTALL_DIR}/lib/modules/${ICM42670_KVER}"
    install -m 644 "${BASE_DIR}/src/icm42670-kmod/icm42670.ko" \
        "${PKG_INSTALL_DIR}/lib/modules/${ICM42670_KVER}/"
}
