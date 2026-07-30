#!/bin/bash
# RV1106 SDK - Package Manager
set -euo pipefail

PKG_DIR="$(cd "$(dirname "$0")" && pwd)"
AVAILABLE="${PKG_DIR}/available"
CONFIG="${PKG_DIR}/package-config"
BASE_DIR="$(cd "$PKG_DIR/.." && pwd)"
OUTPUT="${BASE_DIR}/output"
SRC_CACHE="${OUTPUT}/src/packages"
BUILD_BASE="${OUTPUT}/pkg"
ROOTFS_DIR="${ROOTFS_DIR:-${BASE_DIR}/output/rootfs}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
ARCH="${ARCH:-arm}"

mkdir -p "$SRC_CACHE" "$BUILD_BASE"

usage() {
    echo "Usage: pkg.sh <command> [args]"
    echo ""
    echo "Commands:"
    echo "  list              List all packages ✓=enabled ○=disabled"
    echo "  info <name>       Show info for a package"
    echo "  enable <name>     Enable a package"
    echo "  disable <name>    Disable a package"
    echo "  register <name> [url]  Register a new package from template"
    echo "  build <name>      Build a package and its dependencies"
    echo "  build-all         Build all enabled packages"
    echo "  install <name>    Install a built package to rootfs"
    echo "  install-all       Install all built packages to rootfs"
    echo "  clean <name>      Clean a package's build artifacts"
    echo "  clean-all         Clean all package artifacts"
    echo "  menuconfig        Enable/disable packages via whiptail TUI"
exit 1; }

list_enabled() { while IFS='=' read -r k v; do [[ "$k" == PKG_ENABLE_* && "$v" == "y" ]] && echo "${k#PKG_ENABLE_}"; done < "$CONFIG"; }
list_all() { for d in "$AVAILABLE"/*/; do basename "$d"; done; }
is_enabled() { grep -qs "^PKG_ENABLE_$1=y" "$CONFIG"; }

cmd_list() { for pkg in $(list_all); do is_enabled "$pkg" && echo "  ✓ $pkg" || echo "  ○ $pkg"; done; }

cmd_info() {
    local name="$1" mk="${AVAILABLE}/${name}/package.mk"; [[ -f "$mk" ]] || exit 1
    source "$mk"
    echo "Name:$PKG_NAME Version:$PKG_VERSION License:${PKG_LICENSE:-?} Depends:${PKG_DEPENDS:--} Desc:${PKG_DESCRIPTION:-}"
}

cmd_enable() {
    local name="$1"; [[ -d "${AVAILABLE}/${name}" ]] || { echo "Not found: $name"; exit 1; }
    if grep -qs "^# PKG_ENABLE_${name}=y" "$CONFIG"; then sed -i "s/^# //" "$CONFIG" && echo "Enabled: $name"
    elif grep -qs "^PKG_ENABLE_${name}=y" "$CONFIG"; then echo "Already enabled."
    else echo "PKG_ENABLE_${name}=y" >> "$CONFIG" && echo "Enabled: $name"; fi
}

cmd_disable() {
    local name="$1"
    if grep -qs "^PKG_ENABLE_${name}=y" "$CONFIG"; then sed -i "s/^PKG_ENABLE_${name}=y/# PKG_ENABLE_${name}=y/" "$CONFIG" && echo "Disabled: $name"
    else echo "Already disabled."; fi
}

cmd_register() {
    local name="$1" url="${2:-}" dir="${AVAILABLE}/${name}"
    mkdir -p "$dir"
    cat > "${dir}/package.mk" <<EOF
PKG_NAME="$name"
PKG_VERSION="1.0"
PKG_SOURCE="$url"
PKG_LICENSE=""
PKG_DEPENDS=""
PKG_DESCRIPTION=""
pkg_build() {
    tar xf "\${PKG_SOURCE_MIRROR}" -C "\${PKG_BUILD_DIR}"
    cd "\${PKG_BUILD_DIR}/${name}-\${PKG_VERSION}"
    ./configure --host=\${CROSS_COMPILE%-} --prefix=/usr && make -j\$(nproc)
}
pkg_install() {
    cd "\${PKG_BUILD_DIR}/${name}-\${PKG_VERSION}"
    make install DESTDIR="\${PKG_INSTALL_DIR}"
}
EOF
    echo "Registered: $name"
}

resolve_deps() {
    local name="$1" mk="${AVAILABLE}/${name}/package.mk"
    unset PKG_DEPENDS; source "$mk"
    for dep in ${PKG_DEPENDS:-}; do resolve_deps "$dep"; echo "$dep"; done
}

build_one() {
    local name="$1" mk="${AVAILABLE}/${name}/package.mk" bdir="${BUILD_BASE}/${name}/build" idir="${BUILD_BASE}/${name}/install"
    rm -rf "$bdir" "$idir"; mkdir -p "$bdir" "$idir"
    export BASE_DIR CROSS_COMPILE ARCH ROOTFS_DIR SYSROOT="${SYSROOT:-}" PKG_SOURCE_DIR="${SRC_CACHE}" PKG_BUILD_DIR="$bdir" PKG_INSTALL_DIR="$idir"
    (source "$mk"; for dep in ${PKG_DEPENDS:-}; do build_one "$dep"; done; echo "[pkg] Build: $name"; pkg_build; pkg_install)
}

cmd_build() {
    local name="$1" order=""
    while IFS= read -r dep; do order="$dep $order"; done < <(resolve_deps "$name" | tac)
    for pkg in $order $name; do [[ -d "${BUILD_BASE}/${pkg}/install" ]] || build_one "$pkg"; done
}
cmd_build_all() { for pkg in $(list_enabled); do cmd_build "$pkg"; done; }
cmd_install_all() {
    local dir="${1:-$ROOTFS_DIR}"
    for pkg in $(list_enabled); do
        local idir="${BUILD_BASE}/${pkg}/install"
        [[ -d "$idir" ]] && { echo "[pkg] Install: $pkg"; cp -af "$idir"/* "$dir"/ 2>/dev/null || true; } || echo "[pkg] Skip: $pkg (not built)"
    done
}
cmd_clean() { rm -rf "${BUILD_BASE:?}/$1"; echo "Cleaned: $1"; }
cmd_clean_all() { rm -rf "${BUILD_BASE:?}"/*; echo "Cleaned all."; }
cmd_menuconfig() {
    local pkgs=(); for pkg in $(list_all); do is_enabled "$pkg" && s=ON || s=OFF; pkgs+=("$pkg" "" "$s"); done
    if command -v whiptail &>/dev/null; then
        local sel; sel=$(whiptail --title "Packages" --checklist "Enable/disable" 20 60 10 "${pkgs[@]}" 3>&1 1>&2 2>&3) || sel=""
        for pkg in $(list_all); do cmd_disable "$pkg" &>/dev/null || true; done
        for s in $sel; do cmd_enable "$(echo "$s" | tr -d '"')" &>/dev/null || true; done
    else echo "whiptail not found. Edit pkg/package-config manually."; fi
}

[[ $# -ge 1 ]] || usage
case "$1" in
    list|info|enable|disable|register|build|build-all|clean|clean-all|menuconfig) "cmd_${1//-/_}" "${@:2}" ;;
    install) shift; if [[ $# -ge 1 ]]; then idir="${BUILD_BASE}/$1/install"; [[ -d "$idir" ]] && cp -af "$idir"/* "$ROOTFS_DIR"/; else cmd_install_all; fi ;;
    install-all) cmd_install_all "${2:-}" ;;
    *) usage ;;
esac
