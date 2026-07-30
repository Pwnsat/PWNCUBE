// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * mcutool — hot-load / reset / release the RV1106 SCR1 RISC-V MCU from Linux.
 *
 * Adapted for pwncube from luyi1888/rv1106-mcu (GPL-3.0). The register
 * sequence is identical to our own U-Boot SPL release path
 * (src/u-boot/arch/arm/mach-rockchip/rv1106/rv1106.c:557 spl_fit_standalone_release,
 * "mcu0"): set the cache peripheral (uncached) window, reset the core, set the
 * boot address, release the core. Moving it to userspace lets us reload MCU
 * firmware WITHOUT a full reflash cycle (build → uboot → pack → maskrom → UF).
 *
 * pwncube specifics:
 *   - Our MCU firmware runs from DDR 0x40000 (link.lds ORIGIN), NOT the SRAM
 *     0xff6c0000 the upstream demo uses. Default ADDR here is 0x40000.
 *   - Firmware image: src/mcu/output/image/rtthread.bin (copy it to the device).
 *   - Requires CONFIG_DEVMEM=y and root (mmaps /dev/mem).
 *
 * CAVEAT (dual-boot / rpmsg): our MCU is normally released by the SPL with a
 * live rpmsg link (virtio0 + reserved DDR + bound /dev/rpmsg*). Resetting and
 * hot-reloading tears the MCU side down; the Linux rpmsg endpoints will be
 * stale until re-established. This is a DEV tool — for a firmware that owns the
 * rpmsg services you may need to re-bind the channels (see the README) or, if it
 * desyncs, reboot. It shines for fast iteration and for recovering a wedged MCU.
 *
 * Usage:
 *   mcutool execute FILE [ADDR]   load firmware + release (run). ADDR def 0x40000
 *   mcutool load    FILE [ADDR]   load firmware only (leave in reset)
 *   mcutool run                   release (start) the MCU
 *   mcutool stop                  reset (halt) the MCU
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* Registers — must match src/u-boot/.../rv1106.c spl_fit_standalone_release. */
#define CORE_SGRF_HPMCU_BOOT_ADDR       0xff076044UL  /* MCU entry / boot address */
#define CORECRU_CORESOFTRST_CON01       0xff3b8a04UL  /* MCU core soft reset       */
#define CORE_GRF_CACHE_PERI_ADDR_START  0xff040024UL  /* uncached (MMIO) window lo */
#define CORE_GRF_CACHE_PERI_ADDR_END    0xff040028UL  /* uncached (MMIO) window hi */
#define RESET_MCU                       0x1e001eUL    /* write-masked: assert reset  */
#define RELEASE_MCU                     0x1e0000UL    /* write-masked: deassert reset*/
#define CACHE_PERI_WINDOW_START         0xff000UL     /* covers all 0xffxxxxxx MMIO  */
#define CACHE_PERI_WINDOW_END           0xffc00UL

#define PWNCUBE_MCU_DDR_ENTRY           0x40000UL     /* our firmware ORIGIN (link.lds) */

#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)

static unsigned long set_reg(unsigned long addr, unsigned long value)
{
	int devmem;
	void *mapping, *virt_addr;
	unsigned long read_result;

	devmem = open("/dev/mem", O_RDWR | O_SYNC);
	if (devmem == -1) {
		perror("open /dev/mem (need root + CONFIG_DEVMEM)");
		return (unsigned long)-1;
	}

	mapping = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
	               devmem, addr & ~MAP_MASK);
	if (mapping == MAP_FAILED) {
		perror("mmap register");
		close(devmem);
		return (unsigned long)-1;
	}
	virt_addr = (char *)mapping + (addr & MAP_MASK);
	*((volatile unsigned long *)virt_addr) = value;
	read_result = *((volatile unsigned long *)virt_addr);

	munmap(mapping, MAP_SIZE);
	close(devmem);
	return read_result;
}

static unsigned long parse_int(const char *str)
{
	char *endptr;
	long long result = strtoll(str, &endptr, 0);

	if (str[0] == '\0' || *endptr != '\0') {
		fprintf(stderr, "\"%s\" is not a valid number\n", str);
		exit(EXIT_FAILURE);
	}
	return (unsigned long)result;
}

static void mcu_reset(void)
{
	set_reg(CORECRU_CORESOFTRST_CON01, RESET_MCU);
	printf("MCU reset (halted).\n");
}

static void mcu_release(void)
{
	set_reg(CORECRU_CORESOFTRST_CON01, RELEASE_MCU);
	printf("MCU released (running).\n");
}

static int mcu_load_fw(unsigned long addr, const char *filename)
{
	struct stat sb;
	FILE *fd;
	unsigned char *mcufw;
	size_t length;
	int devmem;
	void *mapping;
	off_t map_base, extra_bytes;

	fd = fopen(filename, "rb");
	if (!fd) {
		fprintf(stderr, "Could not open %s\n", filename);
		return -1;
	}
	if (stat(filename, &sb) != 0) {
		fprintf(stderr, "stat %s failed\n", filename);
		fclose(fd);
		return -1;
	}
	length = (size_t)sb.st_size;
	mcufw = malloc(length);
	if (!mcufw) {
		fclose(fd);
		return -1;
	}
	if (fread(mcufw, 1, length, fd) != length) {
		fprintf(stderr, "short read on %s\n", filename);
		free(mcufw);
		fclose(fd);
		return -1;
	}
	fclose(fd);

	/* Mark all MMIO (0xffxxxxxx) as uncached for the MCU, then hold it in
	 * reset while we stage the image (same as the SPL release path). */
	set_reg(CORE_GRF_CACHE_PERI_ADDR_START, CACHE_PERI_WINDOW_START);
	set_reg(CORE_GRF_CACHE_PERI_ADDR_END,   CACHE_PERI_WINDOW_END);
	mcu_reset();

	devmem = open("/dev/mem", O_RDWR | O_SYNC);
	if (devmem == -1) {
		perror("open /dev/mem (need root + CONFIG_DEVMEM)");
		free(mcufw);
		return -1;
	}
	map_base = addr & ~MAP_MASK;
	extra_bytes = addr - map_base;

	mapping = mmap(NULL, length + extra_bytes, PROT_READ | PROT_WRITE,
	               MAP_SHARED, devmem, map_base);
	if (mapping == MAP_FAILED) {
		perror("mmap MCU load region (reserved DDR? check STRICT_DEVMEM)");
		close(devmem);
		free(mcufw);
		return -1;
	}
	memcpy((char *)mapping + extra_bytes, mcufw, length);
	munmap(mapping, length + extra_bytes);
	close(devmem);
	free(mcufw);

	printf("MCU firmware (%zu bytes) loaded to 0x%08lx.\n", length, addr);
	set_reg(CORE_SGRF_HPMCU_BOOT_ADDR, addr);
	printf("MCU boot addr set to 0x%08lx.\n", addr);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"mcutool — hot-load/reset/release the RV1106 RISC-V MCU (pwncube)\n"
		"Usage:\n"
		"  mcutool execute FILE [ADDR]   load firmware + run (ADDR default 0x40000)\n"
		"  mcutool load    FILE [ADDR]   load firmware only (stays in reset)\n"
		"  mcutool run                   release (start) the MCU\n"
		"  mcutool stop                  reset (halt) the MCU\n"
		"Needs root + CONFIG_DEVMEM. Firmware: src/mcu/output/image/rtthread.bin\n");
}

int main(int argc, char *argv[])
{
	unsigned long addr;

	if (argc < 2) {
		usage();
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[1], "execute") || !strcmp(argv[1], "load")) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}
		addr = (argc >= 4) ? parse_int(argv[3]) : PWNCUBE_MCU_DDR_ENTRY;
		if (mcu_load_fw(addr, argv[2]) != 0)
			return EXIT_FAILURE;
		if (!strcmp(argv[1], "execute")) {
			mcu_release();
			printf("\nNote: if this firmware owns rpmsg, re-establish the link\n"
			       "(re-bind /dev/rpmsg* per the README) or reboot if it desyncs.\n");
		}
	} else if (!strcmp(argv[1], "run")) {
		mcu_release();
	} else if (!strcmp(argv[1], "stop")) {
		mcu_reset();
	} else {
		usage();
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
