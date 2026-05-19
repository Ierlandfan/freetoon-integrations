# Cross-compile hello-solar for the Toon (ARMv7 hardfloat, glibc 2.21).
#
# Reuse the same linaro toolchain the freetoon-lvgl repo uses. If you
# have a different prefix, override CROSS:
#   make CROSS=/path/to/arm-linux-gnueabihf-

CROSS    := /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-
SYSROOT  := /tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc
CC       := $(CROSS)gcc
STRIP    := $(CROSS)strip

CFLAGS  := --sysroot=$(SYSROOT) -O2 -Wall -Wextra
LDFLAGS := --sysroot=$(SYSROOT)

OUT := hello-solar

all: $(OUT)

$(OUT): hello-solar.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(STRIP) $@

# Package for the marketplace tarball — binary + manifest + README.
tarball: $(OUT)
	tar czf hello-solar.tar.gz $(OUT) manifest.json README.md

clean:
	rm -f $(OUT) hello-solar.tar.gz

.PHONY: all clean tarball
