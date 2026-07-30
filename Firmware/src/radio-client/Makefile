# CubeSat — userspace client for the MCU RadioService (SX1262 over rpmsg).
# The radio hardware is owned by the RISC-V MCU (RT-Thread); this tool talks
# to it through /dev/rpmsg (see docs/migration/design/40-migration-design.md).
CC      ?= gcc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -static

# Shared CCSDS TC library (single source of truth — the same files the MCU
# firmware builds), used by the `tcsecsend` command to forge secured TCs.
# BASE_DIR is the repo root: the package manager exports it, and when building
# in-place it defaults to two levels up from this Makefile. Rooting MCU_APP at
# BASE_DIR keeps the build working after pkg.sh copies these sources into an
# isolated build dir, where a path relative to src/radio-client would not exist.
BASE_DIR ?= $(abspath $(CURDIR)/../..)
MCU_APP  ?= $(BASE_DIR)/src/mcu/rt-thread/bsp/rockchip/rv1106-mcu/applications
CCSDS_SRC := $(MCU_APP)/spp.c $(MCU_APP)/ccsds/ccsds_crc.c \
             $(MCU_APP)/ccsds/ccsds_aes.c $(MCU_APP)/ccsds/ccsds_tc.c
# override so the include dirs survive a command-line CFLAGS (the package
# manager passes its own CFLAGS, which would otherwise replace these).
override CFLAGS += -I$(MCU_APP) -I$(MCU_APP)/ccsds

radio_test: radio_test.c $(CCSDS_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ radio_test.c $(CCSDS_SRC)

install: radio_test
	install -d $(DESTDIR)/usr/bin
	install -m 755 radio_test $(DESTDIR)/usr/bin/radio_test

clean:
	rm -f radio_test
