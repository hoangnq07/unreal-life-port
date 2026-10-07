# Cross-compile for ArkOS (Ubuntu 19.10 / glibc 2.30, aarch64) using zig as the C toolchain.
ZIG      ?= /opt/zigenv/bin/python -m ziglang
CC       := $(ZIG) cc -target aarch64-linux-gnu.2.30
CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fno-strict-aliasing -Ithird_party/zlib -Isrc
LDFLAGS  := -ldl -lpthread -lm -rdynamic

SRC := src/main.c src/loader.c src/shim.c src/fakejni.c src/android_native.c src/sdl_gl.c src/androidfw.c src/looper.c src/opensles.c src/shim_asm.S
ZLIB := $(wildcard third_party/zlib/*.c)
OUT := build/unityhost

all: $(OUT)

src/jni_slots.inc: tools/gen_jni_slots.py
	python3 tools/gen_jni_slots.py

build/zlib/%.o: third_party/zlib/%.c
	@mkdir -p build/zlib
	$(CC) -O2 -w -DHAVE_UNISTD_H -DHAVE_STDARG_H -c $< -o $@

ZOBJ := $(patsubst third_party/zlib/%.c,build/zlib/%.o,$(ZLIB))

$(OUT): $(SRC) src/jni_slots.inc $(wildcard src/*.h) $(ZOBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(ZOBJ) -o $@ $(LDFLAGS)

clean:
	rm -rf build

.PHONY: all clean
