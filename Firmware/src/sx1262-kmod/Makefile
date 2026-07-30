# SX1262 LoRa transceiver kernel module
obj-m += sx1262.o
sx1262-objs := sx1262_core.o sx1262_hal.o sx1262_cmd.o sx1262_chardev.o

KERNEL_SRC ?= $(CURDIR)/../../src/kernel
ARCH ?= arm
CROSS_COMPILE ?= arm-rockchip830-linux-uclibcgnueabihf-
O ?=
KBUILD_ARGS = M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
ifdef O
KBUILD_ARGS += O=$(O)
endif

all:
	$(MAKE) -C $(KERNEL_SRC) $(KBUILD_ARGS) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) $(KBUILD_ARGS) clean

install:
	$(MAKE) -C $(KERNEL_SRC) $(KBUILD_ARGS) modules_install INSTALL_MOD_PATH=$(DESTDIR)
