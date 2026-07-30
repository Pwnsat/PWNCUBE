CC      ?= arm-rockchip830-linux-uclibcgnueabihf-gcc
CFLAGS  ?= -Os -Wall -I.
LDFLAGS ?= -static

all: sx1262_cli

sx1262_cli: sx1262_cli.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f sx1262_cli

install:
	install -d $(DESTDIR)/usr/bin
	install -m 755 sx1262_cli $(DESTDIR)/usr/bin/

.PHONY: all clean install
