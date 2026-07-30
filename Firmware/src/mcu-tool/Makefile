# CubeSat — userspace tool to hot-load / reset / release the RISC-V MCU from
# Linux (via /dev/mem), mirroring the U-Boot SPL release sequence. Dev aid to
# iterate MCU firmware without a full reflash. See mcutool.c header.
CC      ?= gcc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -static

mcutool: mcutool.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: mcutool
	install -d $(DESTDIR)/usr/bin
	install -m 755 mcutool $(DESTDIR)/usr/bin/mcutool

clean:
	rm -f mcutool
