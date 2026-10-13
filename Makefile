# no_fft - lossy WAV -> .no_fft variable-degree polynomial codec
#          also acpcm: sample-domain DPCM with a range coder (.nadc)
#
#   make            build the host (linux) codec and the loadable codec modules
#   make test       run the self test
#   make test-modules  build the modules and exercise them through the C ABI
#   make modules    just the core library and the per codec shared objects
#   make install    install the library, headers and modules under $(PREFIX)
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

.PHONY: all linux windows windows-modules deb clean test test-modules modules install

all: linux modules

linux: $(OUT_DIR_LINUX)/nofft

$(OUT_DIR_LINUX)/nofft: $(SRC) $(HDR)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SRC) -o $@ $(LDLIBS)

windows: $(OUT_DIR_WIN)/nofft.exe

$(OUT_DIR_WIN)/nofft.exe: $(SRC) $(HDR)
	@mkdir -p $(@D)
	$(WIN_CXX) $(WIN_CXXFLAGS) $(CPPFLAGS) $(SRC) -o $@ $(WIN_LDLIBS)

# ---- loadable codec modules ------------------------------------------------
#
# The core library carries the codecs and the registry; the per codec modules
# are tiny shared objects that register one codec each through the ABI in
# codec.h.  A host can link the library directly (build/lib/libnofft.so, or the
# static libnofft.a) and use the builtins, or discover the modules at run time
# with nofft_codec_autoload().  See codec.h and tools/codec_tool.cpp.

LIB_DIR := build/lib
OBJ_DIR := build/obj
MOD_DIR := build/modules
BIN_DIR := build/bin

CODEC_LIB_SRC := nofft.cpp acpcm.cpp codec.cpp codec_builtin.cpp
CODEC_HDR     := nofft.h acpcm.h codec.h
CODEC_OBJS    := $(OBJ_DIR)/nofft.o $(OBJ_DIR)/acpcm.o \
                 $(OBJ_DIR)/codec.o $(OBJ_DIR)/codec_builtin.o
CODEC_NAMES   := poly nfa1 nfa2 nfa3 nfa4 nfa5 nfa6

LIB_SO  := $(LIB_DIR)/libnofft.so
LIB_A   := $(LIB_DIR)/libnofft.a
MODULES := $(patsubst %,$(MOD_DIR)/nofft_codec_%.so,$(CODEC_NAMES))
TOOL    := $(BIN_DIR)/nofft_codec

# $ORIGIN/.. covers the installed layout (modules under <lib>/nofft, the library
# in <lib>); the absolute path lets the modules load straight from build/.
LIB_RPATH := -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,$(abspath $(LIB_DIR))

modules: $(LIB_SO) $(LIB_A) $(MODULES) $(TOOL)

$(OBJ_DIR)/%.o: %.cpp $(CODEC_HDR)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fPIC $(CPPFLAGS) -c $< -o $@

$(LIB_A): $(CODEC_OBJS)
	@mkdir -p $(@D)
	ar rcs $@ $^

$(LIB_SO): $(CODEC_OBJS)
	@mkdir -p $(@D)
	$(CXX) -shared $^ -o $@ $(LDLIBS)

# The module sources are plain C on purpose: it shows the ABI really is a C ABI
# and keeps a plugin free of the C++ runtime.
$(MOD_DIR)/nofft_codec_%.so: plugins/%.c $(LIB_SO) $(CODEC_HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fPIC -I. -shared $< -o $@ -L$(LIB_DIR) -lnofft $(LIB_RPATH)

$(TOOL): tools/codec_tool.cpp $(LIB_SO) $(CODEC_HDR)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I. $< -o $@ -L$(LIB_DIR) -lnofft \
	    -Wl,-rpath,$(abspath $(LIB_DIR)) $(LDLIBS)

# The same modules cross built for win64 (needs mingw-w64, like `make windows`).
WIN_CC      ?= x86_64-w64-mingw32-gcc
WIN_MOD_DIR := $(OUT_DIR_WIN)/modules
WIN_MODULES := $(patsubst %,$(WIN_MOD_DIR)/nofft_codec_%.dll,$(CODEC_NAMES))

windows-modules: $(OUT_DIR_WIN)/nofft.dll $(WIN_MODULES) $(OUT_DIR_WIN)/nofft_codec.exe

$(OUT_DIR_WIN)/nofft.dll: $(CODEC_LIB_SRC) $(CODEC_HDR)
	@mkdir -p $(@D)
	$(WIN_CXX) $(WIN_CXXFLAGS) $(CPPFLAGS) -shared $(CODEC_LIB_SRC) -o $@ \
	    -Wl,--out-implib,$(OUT_DIR_WIN)/libnofft.dll.a $(WIN_LDLIBS)

$(WIN_MOD_DIR)/nofft_codec_%.dll: plugins/%.c $(OUT_DIR_WIN)/nofft.dll
	@mkdir -p $(@D)
	$(WIN_CC) $(CFLAGS) -I. -shared $< -o $@ -L$(OUT_DIR_WIN) -lnofft

$(OUT_DIR_WIN)/nofft_codec.exe: tools/codec_tool.cpp $(OUT_DIR_WIN)/nofft.dll
	$(WIN_CXX) $(WIN_CXXFLAGS) $(CPPFLAGS) $< -o $@ -L$(OUT_DIR_WIN) -lnofft $(WIN_LDLIBS)

PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
MODULEDIR  ?= $(LIBDIR)/nofft

install: modules
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR)/nofft \
	           $(DESTDIR)$(MODULEDIR) $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(LIB_SO) $(DESTDIR)$(LIBDIR)/libnofft.so
	install -m 644 $(LIB_A)  $(DESTDIR)$(LIBDIR)/libnofft.a
	install -m 644 codec.h acpcm.h nofft.h $(DESTDIR)$(INCLUDEDIR)/nofft/
	install -m 755 $(MODULES) $(DESTDIR)$(MODULEDIR)/
	install -m 755 $(TOOL) $(DESTDIR)$(PREFIX)/bin/nofft_codec

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
	rm -f $(OUT_DIR_WIN)/nofft.dll $(OUT_DIR_WIN)/nofft_codec.exe $(OUT_DIR_WIN)/libnofft.dll.a
	rm -rf $(OUT_DIR_WIN)/modules

# fail if debian/control, debian/changelog and the man page disagree on the version
# self contained checks: no input files needed
test: linux
	./codecs/linux/ubuntu/nofft selftest

# Exercises the module layer end to end: the tool links only the core library
# and reaches every codec through the vtable, with the .so modules loaded from
# build/modules via NOFFT_MODULE_PATH.
test-modules: linux modules
	@mkdir -p build/mtest
	./codecs/linux/ubuntu/nofft ac-gen build/mtest/t.wav 0.25 2
	@echo "-- builtins reached through the registry --"
	$(TOOL) list
	@echo "-- driver codec roundtrip --"
	$(TOOL) roundtrip nfa3 build/mtest/t.wav bits=6
	$(TOOL) roundtrip poly build/mtest/t.wav frame=20 degree=3 coef=f16 fit=least-sq
	@echo "-- loadable modules --"
	NOFFT_MODULE_PATH=$(abspath $(MOD_DIR)) $(TOOL) list
	NOFFT_MODULE_PATH=$(abspath $(MOD_DIR)) $(TOOL) roundtrip nfa1 build/mtest/t.wav bits=6
	$(TOOL) load $(MOD_DIR)/nofft_codec_nfa3.so && echo "  dlopen of nofft_codec_nfa3.so ok"
	@echo "-- module only: .so discovery fills the registry --"
	@n=$$(NOFFT_NO_BUILTINS=1 $(TOOL) count); \
	 test "$$n" = 0 || { echo "expected 0 codecs without modules, got $$n" >&2; exit 1; }
	@n=$$(NOFFT_NO_BUILTINS=1 NOFFT_MODULE_PATH=$(abspath $(MOD_DIR)) $(TOOL) count); \
	 test "$$n" = 7 || { echo "expected 7 codecs from modules, got $$n" >&2; exit 1; }
	@echo "  0 builtins + 7 modules = registry of 7"
	@rm -rf build/mtest

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
