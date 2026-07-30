# PWNCUBE

**A hardware-based CubeSat platform for aerospace cybersecurity research and training**, developed by PWNSAT in partnership with [ElectronicCats](https://github.com/ElectronicCats). PWNCUBE is a satellite design and vulnerability set onto real dual-core flight-computer hardware (Rockchip RV1106 — Cortex-A7 Linux + RISC-V RT-Thread), demonstrating that the same class of satellite command-and-control vulnerabilities reproduces across genuinely different hardware, not just one reference board.

Full documentation, architecture diagrams, and step-by-step guides live in the **[Wiki](../../wiki)**.

## Repository layout

| Folder | Contents |
|---|---|
| [`Firmware/`](Firmware/) | The complete RV1106 SDK source: dual-core boot (Cortex-A7 Linux + RISC-V RT-Thread), CCSDS Space Packet Protocol stack, radio/sensor/GPS drivers, build scripts. Clone-and-build (see [Getting Started](../../wiki/Getting-Started)). |
| [`Attacks/`](Attacks/) | 8 standalone, documented attack scripts (00–07) exploiting the vulnerabilities below — both over the board's USB debug console and over real RF (HackRF uplink / RTL-SDR downlink). No dependency on any other repository. |
| [`Case/`](Case/) | 3D-printable enclosure (STL + slicer project files). |

## What PWNCUBE demonstrates

Same protocol (CCSDS Space Packet Protocol), same command APIDs, same vulnerability classes as FlatSat — reproduced on real dual-core flight hardware instead of a single-MCU reference board:

- **No authentication** on any telecommand (thruster control, reset, firmware disclosure, mode changes)
- **No anti-replay protection** — a captured command can be resent verbatim and re-executed
- **A ground-station "auth" handshake** trivially forgeable with a static, hardcoded XOR key
- **A GPS subsystem** (real u-blox NEO-6M receiver) that trusts any injected fix with zero plausibility or rate-of-change checking
- **Plaintext telemetry by default** — no encryption on the downlink at all, unlike FlatSat's static-key AES-128
- **A memory-safety bug** (integer underflow in message reassembly) that crashes the flight computer's real-time core with a single malformed packet, permanently killing the internal command channel until a physical power-cycle

See the **[Attack Vectors](../../wiki/Attack-Vectors)** wiki page for the full walkthrough of all 8 attacks, with real hardware output for each.

## Quick start

```bash
git clone <this-repo-url> PWNCUBE-FIRMWARE
cd PWNCUBE-FIRMWARE/Firmware
./build.sh deps      # install host build dependencies
./build.sh            # produces output/images/update.img
```

Flashing requires a Linux x86-64 host (the vendor `upgrade_tool` is not portable) with USB access to the board in maskrom mode — see **[Getting Started](../../wiki/Getting-Started)** for the full walkthrough, including the toolchain download step (excluded from this repo, see below).

## Note on repository size

This repository intentionally excludes two things present in the original SDK archive:

- **`toolchain/`** (ARM + RISC-V cross-compilers, ~2.6GB) — third-party, freely redistributable, but too large for a source repo. Download instructions: **[Getting Started](../../wiki/Getting-Started)**.
- **`src/rkbin/`** (Rockchip's proprietary boot-stage binaries) — third-party, closed-source, redistribution terms not confirmed. Download instructions: **[Getting Started](../../wiki/Getting-Started)**.

Everything else — kernel, U-Boot, the RT-Thread MCU firmware, all PWNSAT-authored application code — is included as source.

## License

Firmware source: see [`Firmware/README.md`](Firmware/README.md#license) for the per-component breakdown (this SDK integrates code under several different open-source licenses). Attack scripts and this repository's own documentation: MIT unless noted otherwise.

## Related projects

- **[FlatSat](https://github.com/Pwnsat/FlatSat)** — the original single-MCU (RP2040) satellite security training platform this project ports from.
- **PWNSAT-C3** — the shared ground-station dashboard used to operate both FlatSat and PWNCUBE (separate repository).
