#!/bin/bash
# ---------------------------------------------------------------------------
# 00-setup-toolchain.sh -- source-able environment setup for
#   arm-rockchip830-linux-uclibcgnueabihf cross-compiler toolchain
#
# Usage:
#   source scripts/00-setup-toolchain.sh
#
# This script exports all common cross-compilation variables and validates
# that the toolchain is present and functional.
# ---------------------------------------------------------------------------

# Guard against direct execution (must be sourced)
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "ERROR: This script must be sourced, not executed." >&2
    echo "Usage: source ${0}" >&2
    exit 1
fi

# ---- Resolve toolchain directory relative to this script ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_TOOLCHAIN_DIR="${SDK_DIR}/toolchain/arm-rockchip830-linux-uclibcgnueabihf"

# Allow override via TOOLCHAIN_DIR environment variable
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-${DEFAULT_TOOLCHAIN_DIR}}"

# ---- Cross-compilation triple ----
CROSS_COMPILE="arm-rockchip830-linux-uclibcgnueabihf-"
ARCH="arm"

# ---- Tool paths ----
TOOLCHAIN_BIN="${TOOLCHAIN_DIR}/bin"
SYSROOT="${TOOLCHAIN_DIR}/${CROSS_COMPILE%-}/sysroot"

CC="${TOOLCHAIN_BIN}/${CROSS_COMPILE}gcc"
CXX="${TOOLCHAIN_BIN}/${CROSS_COMPILE}g++"
AR="${TOOLCHAIN_BIN}/${CROSS_COMPILE}ar"
AS="${TOOLCHAIN_BIN}/${CROSS_COMPILE}as"
LD="${TOOLCHAIN_BIN}/${CROSS_COMPILE}ld"
NM="${TOOLCHAIN_BIN}/${CROSS_COMPILE}nm"
OBJCOPY="${TOOLCHAIN_BIN}/${CROSS_COMPILE}objcopy"
OBJDUMP="${TOOLCHAIN_BIN}/${CROSS_COMPILE}objdump"
RANLIB="${TOOLCHAIN_BIN}/${CROSS_COMPILE}ranlib"
READELF="${TOOLCHAIN_BIN}/${CROSS_COMPILE}readelf"
SIZE="${TOOLCHAIN_BIN}/${CROSS_COMPILE}size"
STRINGS="${TOOLCHAIN_BIN}/${CROSS_COMPILE}strings"
STRIP="${TOOLCHAIN_BIN}/${CROSS_COMPILE}strip"
GDB="${TOOLCHAIN_BIN}/${CROSS_COMPILE}gdb"
GCOV="${TOOLCHAIN_BIN}/${CROSS_COMPILE}gcov"
CPP="${TOOLCHAIN_BIN}/${CROSS_COMPILE}cpp"
ELFEDIT="${TOOLCHAIN_BIN}/${CROSS_COMPILE}elfedit"
DWP="${TOOLCHAIN_BIN}/${CROSS_COMPILE}dwp"
PROF="${TOOLCHAIN_BIN}/${CROSS_COMPILE}gprof"

# ---- Common compiler flags ----
CFLAGS="-march=armv7-a -mfloat-abi=hard -mfpu=neon --sysroot=${SYSROOT}"
CXXFLAGS="${CFLAGS}"
LDFLAGS="--sysroot=${SYSROOT}"

# ---- Export everything ----
export TOOLCHAIN_DIR
export CROSS_COMPILE
export ARCH
export SYSROOT
export CC
export CXX
export AR
export AS
export LD
export NM
export OBJCOPY
export OBJDUMP
export RANLIB
export READELF
export SIZE
export STRINGS
export STRIP
export GDB
export GCOV
export CPP
export ELFEDIT
export DWP
export PROF
export CFLAGS
export CXXFLAGS
export LDFLAGS

# Prepend toolchain bin to PATH if not already there
if [[ ":$PATH:" != *":${TOOLCHAIN_BIN}:"* ]]; then
    export PATH="${TOOLCHAIN_BIN}:${PATH}"
fi

# ---- Validation ---------------------------------------------------------

errors=0

# 1. Toolchain directory exists
if [[ ! -d "${TOOLCHAIN_DIR}" ]]; then
    echo "ERROR: Toolchain directory not found: ${TOOLCHAIN_DIR}" >&2
    errors=$((errors + 1))
fi

# 2. GCC is present and executable
GCC_PATH="${TOOLCHAIN_BIN}/${CROSS_COMPILE}gcc"
if [[ ! -x "${GCC_PATH}" ]]; then
    echo "ERROR: Cross-compiler not found or not executable: ${GCC_PATH}" >&2
    errors=$((errors + 1))
fi

# 3. GCC version check
if [[ -x "${GCC_PATH}" ]]; then
    GCC_VERSION=$("${GCC_PATH}" --version 2>/dev/null | head -1)
    echo "Toolchain GCC: ${GCC_VERSION}"
    if ! "${GCC_PATH}" --version &>/dev/null; then
        echo "ERROR: Cross-compiler failed to execute." >&2
        errors=$((errors + 1))
    fi
fi

# 4. Sysroot exists
if [[ ! -d "${SYSROOT}" ]]; then
    echo "ERROR: sysroot not found: ${SYSROOT}" >&2
    errors=$((errors + 1))
fi

# 5. Key host packages
REQUIRED_HOST_PKGS=(
    git make gcc g++ bc cpio rsync fakeroot bison flex
    libssl-dev device-tree-compiler
)
missing_pkgs=()
for pkg in "${REQUIRED_HOST_PKGS[@]}"; do
    if ! dpkg -s "${pkg}" &>/dev/null 2>&1; then
        # For packages not named exactly like tools, try which
        if ! command -v "${pkg}" &>/dev/null 2>&1; then
            missing_pkgs+=("${pkg}")
        fi
    fi
done

if [[ ${#missing_pkgs[@]} -gt 0 ]]; then
    echo "WARNING: The following host packages may be missing:" >&2
    printf '  - %s\n' "${missing_pkgs[@]}" >&2
    echo "Install them with: sudo apt install ${missing_pkgs[*]}" >&2
fi

# ---- Summary ------------------------------------------------------------
if [[ ${errors} -eq 0 ]]; then
    echo "=========================================="
    echo "Toolchain setup complete."
    echo "  TOOLCHAIN_DIR : ${TOOLCHAIN_DIR}"
    echo "  CROSS_COMPILE : ${CROSS_COMPILE}"
    echo "  ARCH          : ${ARCH}"
    echo "  SYSROOT       : ${SYSROOT}"
    echo "  CC            : ${CC}"
    echo "  CXX           : ${CXX}"
    echo "  PATH prefix   : ${TOOLCHAIN_BIN}"
    echo "=========================================="
else
    echo "ERROR: ${errors} validation error(s) found. Please fix before building." >&2
    return 1
fi
