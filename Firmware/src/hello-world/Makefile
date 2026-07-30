CC      ?= arm-rockchip830-linux-uclibcgnueabihf-gcc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -static

all: hello

hello: hello.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f hello

install:
	install -d $(DESTDIR)/usr/bin
	install -m 755 hello $(DESTDIR)/usr/bin/

.PHONY: all clean install
