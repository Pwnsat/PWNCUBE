PKG_NAME="hello-world"
PKG_VERSION="1.0"
PKG_LICENSE="MIT"
PKG_DEPENDS=""
PKG_DESCRIPTION="Example package: prints Hello, RV1106!"

pkg_build() {
    [ -d "${BASE_DIR}/src/hello-world" ] || { echo "Source not found"; return 1; }
    cp -a "${BASE_DIR}"/src/hello-world/* "${PKG_BUILD_DIR}/"
    make -C "${PKG_BUILD_DIR}" \
        CC="${CROSS_COMPILE}gcc" \
        CFLAGS="-Os -Wall" \
        LDFLAGS="-static"
}

pkg_install() {
    make -C "${PKG_BUILD_DIR}" \
        DESTDIR="${PKG_INSTALL_DIR}" \
        install
}
