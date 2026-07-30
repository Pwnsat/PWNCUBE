PKG_NAME="radio-client"
PKG_VERSION="1.0"
PKG_LICENSE="MIT"
PKG_DEPENDS=""
PKG_DESCRIPTION="RadioService client (radio_test): drives the SX1262 owned by the RISC-V MCU over /dev/rpmsg"

pkg_build() {
    [ -d "${BASE_DIR}/src/radio-client" ] || { echo "Source not found: src/radio-client"; return 1; }
    cp -a "${BASE_DIR}"/src/radio-client/* "${PKG_BUILD_DIR}/"
    make -C "${PKG_BUILD_DIR}" clean
    make -C "${PKG_BUILD_DIR}" \
        CC="${CROSS_COMPILE}gcc" \
        CFLAGS="-Os -Wall" \
        LDFLAGS="-static"
}

pkg_install() {
    make -C "${PKG_BUILD_DIR}" DESTDIR="${PKG_INSTALL_DIR}" install
}
