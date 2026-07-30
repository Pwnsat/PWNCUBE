# CubeSat — userspace client for the MCU TelemetryService (periodic CCSDS telemetry
# via SX1262 over rpmsg). The service runs on the RISC-V MCU (RT-Thread); this
# tool talks to it through /dev/rpmsg (see docs/riscv-migration/).
CC      ?= gcc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -static

telemetry_test: telemetry_test.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: telemetry_test
	install -d $(DESTDIR)/usr/bin
	install -m 755 telemetry_test $(DESTDIR)/usr/bin/telemetry_test

clean:
	rm -f telemetry_test
