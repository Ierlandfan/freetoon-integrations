CROSS    := /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-
SYSROOT  := /tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc
CC       := $(CROSS)gcc
STRIP    := $(CROSS)strip

CFLAGS  := --sysroot=$(SYSROOT) -O2 -Wall -Wextra
LDFLAGS := --sysroot=$(SYSROOT)

OUT := crypto

all: $(OUT)

$(OUT): crypto.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(STRIP) $@

# Package binary + manifest + README + config-template for the marketplace
tarball: $(OUT)
	tar czf crypto.tar.gz $(OUT) manifest.json README.md crypto.conf.example

clean:
	rm -f $(OUT) crypto.tar.gz

.PHONY: all clean tarball
