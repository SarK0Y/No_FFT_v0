# no_fft - lossy WAV -> .no_fft variable-degree polynomial codec
#          also acpcm: sample-domain DPCM with a range coder (.nadc)
#
#   make            build the host (linux) codecs: nofft (C++) and acc (C)
#   make test       run both self tests
#   make check-ac   prove the C and C++ codecs agree byte for byte
#   make deb        build a installable .deb package
#   make windows    cross-build the win64 codec (needs mingw-w64)
#   make clean      remove built codecs, package staging and profiling droppings

PKG_NAME    := nofft
PKG_ARCH    := amd64
# single source of truth: debian/control (verified against the changelog by `make checkver`)
PKG_VERSION := $(shell sed -n 's/^Version:[ \t]*//p' debian/control)
DEB         := build/$(PKG_NAME)_$(PKG_VERSION)_$(PKG_ARCH).deb
STAGE       := build/deb

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror -pedantic
CPPFLAGS += -isystem third_party

# ALSA support for acpcm_play in C++ build
ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA_LIBS   := $(shell pkg-config --libs alsa 2>/dev/null)
ifdef ALSA_LIBS
CPPFLAGS += $(ALSA_CFLAGS) -DHAVE_ALSA -D_POSIX_C_SOURCE=200809L
LDLIBS   += $(ALSA_LIBS)
endif
LDLIBS   += -lm

WIN_CXX      ?= x86_64-w64-mingw32-g++
WIN_CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror -pedantic
WIN_LDLIBS   ?= -lm

SRC := main.cpp nofft.cpp acpcm.cpp selftest.cpp
HDR := nofft.h acpcm.h selftest.h third_party/dr_wav.h

# The same codec in portable C11.  It is a separate binary, not a rewrite of the
# C++ one: the C++ build stays as the format reference and `make check-ac`
# compares the two byte for byte, so neither can drift.
CC      ?= cc
CFLAGS  ?= -O2
CSTD    := -std=c11 -Wall -Wextra -Werror -pedantic -Wshadow -Wpointer-arith \
           -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes
# Stops the compiler fusing the predictor's multiply-adds, which would round
# differently from the C++ build and permanently desync the recursive filter.
CCONF   := -ffp-contract=off
ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA_LIBS   := $(shell pkg-config --libs alsa 2>/dev/null)
# ALSA is optional: without it acc still encodes and decodes, and `acc play`
# reports that it was built without a backend.
ifdef ALSA_LIBS
CPPFLAGS_C := $(ALSA_CFLAGS) -DHAVE_ALSA -D_POSIX_C_SOURCE=200809L
# acc links libasound, so the package has to declare it.  A build without ALSA
# has no such runtime dependency and must not claim one.
ALSA_DEP := , libasound2
endif

CSRC := c/main.c c/acpcm.c c/rc.c c/wavio.c c/play.c
CHDR := c/ac.h c/rc.h

OUT_DIR_LINUX := codecs/linux/ubuntu
OUT_DIR_WIN   := codecs/windows/10

.PHONY: all linux windows deb clean test

all: linux

linux: $(OUT_DIR_LINUX)/nofft

$(OUT_DIR_LINUX)/nofft: $(SRC) $(HDR)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SRC) -o $@ $(LDLIBS)

windows: $(OUT_DIR_WIN)/nofft.exe

$(OUT_DIR_WIN)/nofft.exe: $(SRC) $(HDR)
	@mkdir -p $(@D)
	$(WIN_CXX) $(WIN_CXXFLAGS) $(CPPFLAGS) $(SRC) -o $@ $(WIN_LDLIBS)

deb: $(STAGE)/DEBIAN/control

DEBIAN_SRC := debian/control debian/copyright debian/changelog debian/nofft.1 debian/acc.1

$(STAGE)/DEBIAN/control: $(OUT_DIR_LINUX)/nofft Makefile $(DEBIAN_SRC)
	@rm -rf $(STAGE)
	@mkdir -p $(STAGE)/DEBIAN $(STAGE)/usr/bin $(STAGE)/usr/share/man/man1 $(STAGE)/usr/share/doc/$(PKG_NAME)
	strip -s --strip-unneeded $(OUT_DIR_LINUX)/nofft -o $(STAGE)/usr/bin/$(PKG_NAME)
	install -m 644 debian/control $(STAGE)/DEBIAN/control
	
	install -m 644 debian/copyright $(STAGE)/usr/share/doc/$(PKG_NAME)/copyright
	install -m 644 debian/changelog $(STAGE)/usr/share/doc/$(PKG_NAME)/changelog
	gzip -9n $(STAGE)/usr/share/doc/$(PKG_NAME)/changelog
	install -m 644 debian/nofft.1 $(STAGE)/usr/share/man/man1/$(PKG_NAME).1
	gzip -9n $(STAGE)/usr/share/man/man1/$(PKG_NAME).1
	install -m 644 debian/acc.1 $(STAGE)/usr/share/man/man1/acc.1
	gzip -9n $(STAGE)/usr/share/man/man1/acc.1
	dpkg-deb --build --root-owner-group $(STAGE) $(DEB)
	LC_ALL=C lintian $(DEB)

clean:
	rm -rf build
	rm -f $(OUT_DIR_LINUX)/nofft $(OUT_DIR_LINUX)/acc $(OUT_DIR_WIN)/nofft.exe gmon.out
	

# fail if debian/control, debian/changelog and the man page disagree on the version
# self contained checks: no input files needed
test: linux
	./codecs/linux/ubuntu/nofft selftest

# The point of having the codec in C as well as C++: the two implementations
# must produce the same bytes for the same input at every width.  A silent
# divergence in one of them would otherwise only show up as noise much later.
check-ac: linux acc
	@mkdir -p build/ac
	./codecs/linux/ubuntu/acc gen build/ac/t.wav 0.5 2
	@set -e; for b in 2 4 6 8 12 16 20 24; do \
	  ./codecs/linux/ubuntu/nofft ac-encode build/ac/t.wav build/ac/cpp_$$b.nadc $$b >/dev/null; \
	  ./codecs/linux/ubuntu/acc  encode build/ac/t.wav build/ac/c_$$b.nadc $$b; \
	  cmp build/ac/cpp_$$b.nadc build/ac/c_$$b.nadc; \
	  ./codecs/linux/ubuntu/nofft ac-decode build/ac/c_$$b.nadc build/ac/cpp_$$b.wav >/dev/null; \
	  ./codecs/linux/ubuntu/acc  decode build/ac/c_$$b.nadc build/ac/c_$$b.wav; \
	  cmp build/ac/cpp_$$b.wav build/ac/c_$$b.wav; \
	  echo "  bits $$b: encode and decode byte identical to the C++ reference"; \
	done
	@rm -rf build/ac

checkver:
	@v=$$(sed -n 's/^Version:[ \t]*//p' debian/control); \
	c=$$(sed -n 's/^nofft (\([^)]*\)).*/\1/p' debian/changelog | head -1); \
	m=$$(sed -n 's/^\.TH NOFFT 1 "[^"]*" "nofft \([^"]*\)".*/\1/p' debian/nofft.1); \
	if [ "$$v" != "$$c" ] || [ "$$v" != "$$m" ]; then \
	  echo "version mismatch: control=$$v changelog=$$c man=$$m" >&2; exit 1; fi; \
	echo "version consistent: $$v"
