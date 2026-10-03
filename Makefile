# ISDS205 Wine bridge
#
#   make                  build everything into build/
#   make install          copy the bridge into the program directory and patch
#   make check            show which program files are patched
#   make run              start a program with the bridge enabled
#   make uninstall        restore the patched files and remove the bridge
#   make clean            remove build/
#
# Variables (override on the command line, e.g. `make install WINEPREFIX=~/osc`):
#   WINEPREFIX  Wine prefix the vendor software is installed in (default ~/.wine)
#   APPDIR      program directory inside the prefix
#   PATCH       files to patch on install (default: usbwave.dll)
#   APP         program for `make run` (default: ProfessionalVersion.exe)

MINGW_CC   ?= i686-w64-mingw32-gcc
HOST_CC    ?= gcc
PYTHON     ?= python3
PKG_CONFIG ?= pkg-config

WINEPREFIX ?= $(HOME)/.wine
APPDIR     ?= $(WINEPREFIX)/drive_c/Program Files/Vimu Electronic/Multi VirAnalyzer
PATCH      ?= usbwave.dll
APP        ?= ProfessionalVersion.exe

# Every file `make install` knows how to patch; `make uninstall` restores
# whichever of them has a backup.
PATCHABLE  := usbwave.dll Recorder.exe Logic.exe Sweep.exe
BRIDGE     := oscwus.dll oscsetup.dll oscusb.dll oscusb.so

BUILD      := build
PE_CFLAGS  := -O2 -Wall -Wextra
PE_LDFLAGS := -shared -static-libgcc -Wl,--enable-stdcall-fixup
SO_CFLAGS  := -O2 -Wall -Wextra -shared -fPIC
LIBUSB_CFLAGS = $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS   = $(shell $(PKG_CONFIG) --libs libusb-1.0)

PATCH_TOOL := $(CURDIR)/tools/patch_imports.py

.PHONY: all install check run uninstall clean appdir

all: $(addprefix $(BUILD)/,$(BRIDGE))

$(BUILD):
	mkdir -p $@

$(BUILD)/oscwus.dll: src/oscwus.c src/oscwus.def src/oscusb_unixlib.h src/debuglog.h | $(BUILD)
	$(MINGW_CC) $(PE_CFLAGS) -o $@ src/oscwus.c src/oscwus.def $(PE_LDFLAGS) -lm

$(BUILD)/oscsetup.dll: src/oscsetup.c src/oscsetup.def src/debuglog.h | $(BUILD)
	$(MINGW_CC) $(PE_CFLAGS) -o $@ src/oscsetup.c src/oscsetup.def $(PE_LDFLAGS)

$(BUILD)/oscusb.dll: src/oscusb_pe.c tools/stamp_builtin.py | $(BUILD)
	$(MINGW_CC) $(PE_CFLAGS) -o $@ src/oscusb_pe.c $(PE_LDFLAGS)
	$(PYTHON) tools/stamp_builtin.py $@

$(BUILD)/oscusb.so: src/oscusb_unix.c src/oscusb_unixlib.h | $(BUILD)
	$(HOST_CC) $(SO_CFLAGS) $(LIBUSB_CFLAGS) -o $@ src/oscusb_unix.c $(LIBUSB_LIBS) -lpthread

appdir:
	@test -f "$(APPDIR)/usbwave.dll" || { \
	    echo "error: '$(APPDIR)' is not the Multi VirAnalyzer program directory" >&2; \
	    echo "       set WINEPREFIX (or APPDIR), e.g. make install WINEPREFIX=\$$HOME/.wine-osc" >&2; \
	    exit 1; }

install: all appdir
	for f in $(BRIDGE); do cp $(BUILD)/$$f "$(APPDIR)/$$f"; done
	cd "$(APPDIR)" && $(PYTHON) "$(PATCH_TOOL)" $(PATCH)

check: appdir
	cd "$(APPDIR)" && $(PYTHON) "$(PATCH_TOOL)" --check $(PATCHABLE)

run: appdir
	WINEPREFIX="$(WINEPREFIX)" APPDIR="$(APPDIR)" scripts/run.sh "$(APP)"

uninstall: appdir
	cd "$(APPDIR)" && for f in $(PATCHABLE); do \
	    if [ -f "$$f.orig" ]; then $(PYTHON) "$(PATCH_TOOL)" --restore "$$f"; fi; \
	done
	cd "$(APPDIR)" && rm -f $(BRIDGE)

clean:
	rm -rf $(BUILD)
