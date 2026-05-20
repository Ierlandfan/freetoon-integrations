CROSS    := /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-
SYSROOT  := /tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc
CC       := $(CROSS)gcc
STRIP    := $(CROSS)strip

CFLAGS  := --sysroot=$(SYSROOT) -O2 -Wall -Wextra
LDFLAGS := --sysroot=$(SYSROOT)

OUT := oepi-watermeter

all: $(OUT)

$(OUT): oepi-watermeter.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(STRIP) $@

# Package binary + manifest + README + config-template for the marketplace
tarball: $(OUT)
	tar czf oepi-watermeter.tar.gz $(OUT) manifest.json README.md oepi-watermeter.conf.example

clean:
	rm -f $(OUT) oepi-watermeter.tar.gz

.PHONY: all clean tarball
