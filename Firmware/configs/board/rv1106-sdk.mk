#!/bin/bash
# Board: RV1106 SDK (SPI NAND)
export RK_CHIP="rv1106"
export RK_ARCH="arm"
export RK_TOOLCHAIN_CROSS="arm-rockchip830-linux-uclibcgnueabihf"
export RK_KERNEL_DEFCONFIG="rv1106_minimal_defconfig"
export RK_KERNEL_DTS="rv1106g-sdk"
export RK_UBOOT_DEFCONFIG="rv1106_sdk_defconfig"
export RK_BOOT_MEDIUM="spi_nand"
# A 'meta' partition is REQUIRED for the HP_MCU: the SPL's rk_meta_process()
# (which sets CORE_GRF MCU_CACHE_MISC=0x00080008, without which the SCR1 crashes
# ~400ms after boot) only runs after it finds+reads a partition named "meta".
# Layout mirrors the Luckfox fastboot flow (meta inserted after uboot).
export RK_PARTITION_CMD_IN_ENV="256K(env),512K@256K(idblock),256K@768K(uboot),2M@1024K(meta),32M@3072K(boot),-(rootfs)"
export LF_TARGET_ROOTFS="busybox"
