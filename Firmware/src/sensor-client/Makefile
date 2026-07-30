# CubeSat — userspace client for the MCU SensorService (BME280 + ICM-42670
# over rpmsg). The I2C sensors are owned by the RISC-V MCU (RT-Thread); this
# tool talks to them through /dev/rpmsg (see docs/migration/).
CC      ?= gcc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -static

sensor_test: sensor_test.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: sensor_test
	install -d $(DESTDIR)/usr/bin
	install -m 755 sensor_test $(DESTDIR)/usr/bin/sensor_test

clean:
	rm -f sensor_test
