CROSS    := /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-
SYSROOT  := /tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc
CC       := $(CROSS)gcc
STRIP    := $(CROSS)strip

CFLAGS  := --sysroot=$(SYSROOT) -O2 -Wall -Wextra
LDFLAGS := --sysroot=$(SYSROOT)

OUT := openweather

all: $(OUT)

$(OUT): openweather.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(STRIP) $@

# Package binary + manifest + README + config-template for the marketplace
tarball: $(OUT)
	tar czf openweather.tar.gz $(OUT) manifest.json README.md openweather.conf.example

clean:
	rm -f $(OUT) openweather.tar.gz

.PHONY: all clean tarball
