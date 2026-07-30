PKG_NAME="pwnsat-console"
PKG_VERSION="1.0"
PKG_LICENSE="MIT"
PKG_DEPENDS=""
PKG_DESCRIPTION="PWNSAT console (Black Hat Arsenal Edition): banner/menu on ttyGS0, replaces the raw debug shell (option 3 in the menu drops to /bin/sh)"

pkg_build() {
    [ -d "${BASE_DIR}/src/pwnsat-console" ] || { echo "Source not found: src/pwnsat-console"; return 1; }
    cp -a "${BASE_DIR}"/src/pwnsat-console/* "${PKG_BUILD_DIR}/"
    make -C "${PKG_BUILD_DIR}" clean
    make -C "${PKG_BUILD_DIR}" \
        CC="${CROSS_COMPILE}gcc" \
        CFLAGS="-Os -Wall" \
        LDFLAGS="-static"
}

pkg_install() {
    make -C "${PKG_BUILD_DIR}" DESTDIR="${PKG_INSTALL_DIR}" install
}
