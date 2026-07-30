# SDK PwnCube — Documentación

Target CubeSat sobre el **Rockchip RV1106** (Cortex-A7 + SCR1 RISC-V). Empieza por la
arquitectura, luego build, y después los docs por dispositivo y de seguridad.

Los docs son **bilingües (EN/ES)**. Cada tema es `name.en.md` + `name.es.md` con `name.md` un
symlink al idioma activo. Cambia con `./docs/switch.sh en|es` (o sin argumento para alternar).

## Empieza aquí
- [`getting-started.md`](getting-started.md) — **¿primera vez?** desde cero: instalar dependencias, clonar, compilar la imagen y flashear la placa

## Arquitectura
- [`architecture/overview.md`](architecture/overview.md) — los dos núcleos, roles, boot, misión
- [`architecture/ipc-rpmsg.md`](architecture/ipc-rpmsg.md) — servicios y endpoints rpmsg Linux↔MCU
- [`architecture/peripheral-ownership.md`](architecture/peripheral-ownership.md) — qué periféricos posee el MCU, cómo, el mapa de pines

## Build
- [`build/toolchain.md`](build/toolchain.md) · [`build/uboot.md`](build/uboot.md) · [`build/kernel.md`](build/kernel.md) · [`build/rootfs.md`](build/rootfs.md)
- [`build/packaging.md`](build/packaging.md) — ensamblado de imagen y flasheo · [`build/pkg-system.md`](build/pkg-system.md) — el sistema de paquetes `pkg/`

## Periféricos (los posee el MCU, manejados por rpmsg)
- [`peripherals/sx1262-radio.md`](peripherals/sx1262-radio.md) — SX1262 LoRa dual
- [`peripherals/bme280.md`](peripherals/bme280.md) — temperatura / presión / humedad
- [`peripherals/icm42670.md`](peripherals/icm42670.md) — IMU de 6 ejes

## Seguridad
- [`security/exploitation-guide.md`](security/exploitation-guide.md) — superficie de ataque de telecomandos CCSDS, paso a paso

## Migración (registro)
Cómo el MCU llegó a poseer los periféricos — decisiones de diseño y bring-up en placa:
- [`migration/README.md`](migration/README.md) — índice de la serie de migración
