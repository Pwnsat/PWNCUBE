PKG_NAME="mcu-tool"
PKG_VERSION="1.0"
PKG_LICENSE="GPL-3.0"
PKG_DEPENDS=""
PKG_DESCRIPTION="mcutool: hot-load/reset/release the RISC-V MCU from Linux via /dev/mem (dev aid, needs CONFIG_DEVMEM)"

pkg_build() {
    [ -d "${BASE_DIR}/src/mcu-tool" ] || { echo "Source not found: src/mcu-tool"; return 1; }
    cp -a "${BASE_DIR}"/src/mcu-tool/* "${PKG_BUILD_DIR}/"
    make -C "${PKG_BUILD_DIR}" clean
    make -C "${PKG_BUILD_DIR}" \
        CC="${CROSS_COMPILE}gcc" \
        CFLAGS="-Os -Wall" \
        LDFLAGS="-static"
}

pkg_install() {
    make -C "${PKG_BUILD_DIR}" DESTDIR="${PKG_INSTALL_DIR}" install
}
