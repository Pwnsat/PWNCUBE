PKG_NAME="sx1262"
PKG_VERSION="1.0"
PKG_LICENSE="MIT"
PKG_DEPENDS=""
PKG_DESCRIPTION="SX1262 dual-LoRa kernel driver (sx1262.ko) + CLI (sx1262_cli)"

# Kernel version the module is built/installed for
SX1262_KVER="5.10.160"

pkg_build() {
    local ksrc="${BASE_DIR}/src/kernel"
    local kobj="${BASE_DIR}/output/objs_kernel"
    local kmod="${BASE_DIR}/src/sx1262-kmod"
    local cli="${BASE_DIR}/src/sx1262-cli"

    [ -d "${kmod}" ] || { echo "Source not found: src/sx1262-kmod"; return 1; }
    [ -d "${cli}" ]  || { echo "Source not found: src/sx1262-cli"; return 1; }
    [ -d "${kobj}" ] || { echo "Kernel not built (${kobj} missing) — build the kernel first"; return 1; }

    # --- kernel module (sx1262.ko) ---
    make -C "${ksrc}" O="${kobj}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
        modules_prepare 2>&1 | grep -v "Nothing to be done" || true
    make -C "${kmod}" KERNEL_SRC="${ksrc}" O="${kobj}" \
        ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" clean all

    # --- userspace CLI (sx1262_cli) ---
    cp -a "${cli}"/* "${PKG_BUILD_DIR}/"
    make -C "${PKG_BUILD_DIR}" clean
    make -C "${PKG_BUILD_DIR}" \
        CC="${CROSS_COMPILE}gcc" \
        CFLAGS="-Os -Wall -I." \
        LDFLAGS="-static"
}

pkg_install() {
    # kernel module
    install -d "${PKG_INSTALL_DIR}/lib/modules/${SX1262_KVER}"
    install -m 644 "${BASE_DIR}/src/sx1262-kmod/sx1262.ko" \
        "${PKG_INSTALL_DIR}/lib/modules/${SX1262_KVER}/"
    # CLI
    make -C "${PKG_BUILD_DIR}" DESTDIR="${PKG_INSTALL_DIR}" install
}
