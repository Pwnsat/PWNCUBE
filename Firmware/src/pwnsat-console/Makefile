# PWNSAT console -- "Black Hat Arsenal Edition" banner/menu for the ttyGS0
# USB gadget console (see pwnsat_console.c header for the port rationale).
CC      ?= gcc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -static

pwnsat_console: pwnsat_console.c spacecan_sim.c spacecan_sim.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ pwnsat_console.c spacecan_sim.c

install: pwnsat_console
	install -d $(DESTDIR)/usr/bin
	install -m 755 pwnsat_console $(DESTDIR)/usr/bin/pwnsat_console

clean:
	rm -f pwnsat_console
