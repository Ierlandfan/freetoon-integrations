CROSS    := /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-
SYSROOT  := /tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc
CC       := $(CROSS)gcc
STRIP    := $(CROSS)strip

CFLAGS  := --sysroot=$(SYSROOT) -O2 -Wall -Wextra
LDFLAGS := --sysroot=$(SYSROOT)

OUT := findmy

all: $(OUT)

$(OUT): findmy.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(STRIP) $@

tarball: $(OUT)
	tar czf findmy.tar.gz $(OUT) manifest.json README.md findmy.conf.example

clean:
	rm -f $(OUT) findmy.tar.gz

.PHONY: all clean tarball
