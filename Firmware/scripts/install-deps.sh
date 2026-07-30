#!/bin/bash
# ---------------------------------------------------------------------------
# install-deps.sh — install ALL host build dependencies in one command.
#
#   ./scripts/install-deps.sh        (or:  ./build.sh deps)
#
# Targets Debian/Ubuntu (apt). The toolchains are vendored in the repo, so this
# only installs the system packages needed to build the kernel, U-Boot, the
# rootfs and the RISC-V MCU firmware. Safe to re-run.
# ---------------------------------------------------------------------------
set -euo pipefail

# One list, kept in sync with docs/getting-started and the README.
PKGS="git make gcc g++ bc cpio rsync fakeroot bison flex \
libssl-dev device-tree-compiler scons \
gawk texinfo cmake unzip gperf autoconf \
libncurses5-dev pkg-config python3"

if ! command -v apt-get >/dev/null 2>&1; then
    echo "[deps] apt-get not found — this installer targets Debian/Ubuntu." >&2
    echo "[deps] Install these packages with your distro's package manager:" >&2
    echo "       ${PKGS}" >&2
    exit 1
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

echo "[deps] Updating package lists..."
if ! ${SUDO} apt-get update; then
    echo "[deps] WARNING: 'apt-get update' failed — check network / apt sources." >&2
fi

# Preflight: on a machine with no working repositories the apt cache is empty and
# EVERY package "cannot be located" (git, gcc, make included). Detect that up
# front with a core package that lives in Debian/Ubuntu 'main', and explain what
# to fix — instead of dumping a wall of "unable to locate" errors.
if ! apt-cache show git >/dev/null 2>&1; then
    echo ""                                                                            >&2
    echo "[deps] ERROR: apt has no packages available — the repositories are not set up." >&2
    echo "[deps] On a fresh Debian/Ubuntu this usually means one of:"                   >&2
    echo "         - no network access (apt cannot reach the mirrors)"                  >&2
    echo "         - /etc/apt/sources.list is empty or points only to the install CD"   >&2
    echo "         - Ubuntu: the 'universe' component is not enabled"                    >&2
    echo "       Fix the repos + network, run '${SUDO} apt-get update' until it pulls a" >&2
    echo "       package index, then re-run ./build.sh deps."                           >&2
    exit 1
fi

echo "[deps] Installing host build dependencies..."
# shellcheck disable=SC2086
${SUDO} apt-get install -y ${PKGS}

echo "[deps] Done. Next:"
echo "       source scripts/00-setup-toolchain.sh"
echo "       ./build.sh"
