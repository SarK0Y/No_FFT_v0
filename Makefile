# no_fft - lossy WAV -> .no_fft variable-degree polynomial codec
#
#   make            build the host (linux) codec
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
LDLIBS   += -lm

WIN_CXX      ?= x86_64-w64-mingw32-g++
WIN_CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror -pedantic
WIN_LDLIBS   ?= -lm

SRC := main.cpp nofft.cpp
HDR := nofft.h third_party/dr_wav.h

OUT_DIR_LINUX := codecs/linux/ubuntu
OUT_DIR_WIN   := codecs/windows/10

.PHONY: all linux windows deb clean

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

DEBIAN_SRC := debian/control debian/copyright debian/changelog debian/nofft.1

$(STAGE)/DEBIAN/control: $(OUT_DIR_LINUX)/nofft Makefile $(DEBIAN_SRC)
	@rm -rf $(STAGE)
	@mkdir -p $(STAGE)/DEBIAN $(STAGE)/usr/bin $(STAGE)/usr/share/man/man1 $(STAGE)/usr/share/doc/$(PKG_NAME)
	strip -s --strip-unneeded $< -o $(STAGE)/usr/bin/$(PKG_NAME)
	install -m 644 debian/control $(STAGE)/DEBIAN/control
	install -m 644 debian/copyright $(STAGE)/usr/share/doc/$(PKG_NAME)/copyright
	install -m 644 debian/changelog $(STAGE)/usr/share/doc/$(PKG_NAME)/changelog
	gzip -9n $(STAGE)/usr/share/doc/$(PKG_NAME)/changelog
	install -m 644 debian/nofft.1 $(STAGE)/usr/share/man/man1/$(PKG_NAME).1
	gzip -9n $(STAGE)/usr/share/man/man1/$(PKG_NAME).1
	dpkg-deb --build --root-owner-group $(STAGE) $(DEB)
	LC_ALL=C lintian $(DEB)

clean:
	rm -rf build
	rm -f $(OUT_DIR_LINUX)/nofft $(OUT_DIR_WIN)/nofft.exe gmon.out

# fail if debian/control, debian/changelog and the man page disagree on the version
checkver:
	@v=$$(sed -n 's/^Version:[ \t]*//p' debian/control); \
	c=$$(sed -n 's/^nofft (\([^)]*\)).*/\1/p' debian/changelog | head -1); \
	m=$$(sed -n 's/^\.TH NOFFT 1 "[^"]*" "nofft \([^"]*\)".*/\1/p' debian/nofft.1); \
	if [ "$$v" != "$$c" ] || [ "$$v" != "$$m" ]; then \
	  echo "version mismatch: control=$$v changelog=$$c man=$$m" >&2; exit 1; fi; \
	echo "version consistent: $$v"
