# RV1106 SDK - Package System

[Español](pkg-system.es.md)

## Architecture

```
pkg/
├── pkg.sh              # Package system entry point / CLI
├── package-config      # Plain-text package selection
└── available/
    └── <name>/
        └── package.mk  # Package metadata and build rules
```

## Usage

```bash
cd pkg
bash pkg.sh list               # list packages (✓=enabled ○=disabled)
bash pkg.sh enable <name>      # enable a package
bash pkg.sh disable <name>     # disable a package
bash pkg.sh build-all          # build all enabled packages
bash pkg.sh install-all        # copy built packages to rootfs staging
```

## `package.mk` Example (`hello-world`)

```bash
# Metadata
PKG_NAME="hello-world"
PKG_VERSION="1.0"
PKG_LICENSE="MIT"
PKG_DEPENDS=""
PKG_DESCRIPTION="Example package: prints Hello, RV1106!"

# Build: copy source from src/hello-world/ and compile
pkg_build() {
    cp -a "${BASE_DIR}"/src/hello-world/* "${PKG_BUILD_DIR}/"
    make -C "${PKG_BUILD_DIR}" \
        CC="${CROSS_COMPILE}gcc" \
        CFLAGS="-Os -Wall" \
        LDFLAGS="-static"
}

# Install: make install into PKG_INSTALL_DIR
pkg_install() {
    make -C "${PKG_BUILD_DIR}" \
        DESTDIR="${PKG_INSTALL_DIR}" \
        install
}
```

## How It Works

1. `build-one` sources `package.mk` and calls `pkg_build` (compile source)
2. Then calls `pkg_install` (copy artifacts to `PKG_INSTALL_DIR`)
3. `install-all` copies `PKG_INSTALL_DIR/*` → `${ROOTFS_DIR}/`
4. `03-build-rootfs.sh` creates the ext4 image from `${ROOTFS_DIR}`

Variables available in `package.mk`:
- `BASE_DIR` — SDK root (e.g. `/home/.../pwncube-sdk`)
- `CROSS_COMPILE` — toolchain prefix
- `PKG_BUILD_DIR` — temp build directory
- `PKG_INSTALL_DIR` — temp install directory
- `ROOTFS_DIR` — rootfs staging area

## MCU client packages

The userspace tools that talk to the RISC-V coprocessor over `/dev/rpmsg` ship
as packages (enabled in `package-config`):

| Package | Binary | Talks to |
|---------|--------|----------|
| `radio-client` | `radio_test` | RadioService (SX1262) |
| `sensor-client` | `sensor_test` | SensorService (BME280 + ICM-42670) |
| `telemetry-client` | `telemetry_test` | TelemetryService (CCSDS) |
| `mcu-tool` | `mcutool` | MCU load/reset via `/dev/mem` |

## Rules When Writing a New Package

```
src/<name>/       # source code (Makefile + sources)
pkg/available/<name>/package.mk   # package descriptor
```

Two non-obvious points that break the build silently:

- **Root paths at `BASE_DIR`, never relative.** `pkg_build` compiles in an
  isolated `PKG_BUILD_DIR`, so a `../other-dir` will not resolve. If your package
  needs sources from another tree (e.g. `radio_test` reuses the MCU firmware's
  CCSDS library), root them at `${BASE_DIR}/...`. The package manager exports
  `BASE_DIR` to every package.
- **A failing package aborts the whole phase** (`build-all` runs under
  `set -e`): packages after it never build. Verify yours compiles in isolation
  before trusting it.

Enable and build:

```bash
bash pkg.sh enable <name>
bash pkg.sh build <name>
bash pkg.sh install <name>
```

> Source lives under `src/`, which is `.gitignore`d with a whitelist: add
> `!src/<name>` (and a rule to ignore the compiled binary) or a fresh clone will
> not have your package.

---

[Español](pkg-system.es.md)
