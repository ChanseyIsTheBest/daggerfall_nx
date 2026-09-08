#---------------------------------------------------------------------------------
# papapear_nx -- Papa Pear Saga wrapper for Nintendo Switch
#
#   dkp-pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-zlib \
#                switch-libpng switch-sdl2
#
# Ships no game code and no game assets. See tools/prepare_game.sh.
#
# Audio is OpenSL ES (opensles.c) over SDL2, which is the path both reference
# ports use and the only one with a working precedent. The engine does not link
# OpenSL: it dlopen()s libOpenSLES.so and resolves slCreateEngine and five
# SL_IID_* ids through dlsym, so those are answered from the import table via
# dynlib_find_export rather than by the loader.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Source $$DEVKITPRO/switchvars.sh or install devkitPro.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := papapear_nx
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := source

APP_TITLE   := Papa Pear Saga
APP_AUTHOR  := ChanseyIsTheBest
APP_VERSION := 1.0.0

# 256x256 JPEG, as the NRO format requires. Generated from a 512x512 PNG with
# tools/make_icon.py -- JPEG has no alpha, so the transparent surround is
# composited onto black first, which is both the game's own background and
# what the home menu shows behind a tile.
ICON        := icon.jpg

#---------------------------------------------------------------------------------
# -mtp=soft is not optional. The loaded module is built for bionic and reads its
# stack canary from TPIDR_EL0+0x28; there are 15,198 such sites in libpapapearsaga.so's
# .text. If the compiler is allowed to emit hardware TLS accesses for our own
# code they fight over the same register. See util.c / install_bionic_tls.
#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

DEFINES := -D__SWITCH__

CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 \
           -ffunction-sections -fdata-sections $(ARCH) $(DEFINES)

# On aarch64 an implicit declaration is not a style problem: the compiler
# assumes an int return and guesses the arguments, which can corrupt registers
# rather than merely warn. Make it an error on every toolchain version.
CFLAGS  += -Werror=implicit-function-declaration -Werror=implicit-int
CFLAGS  += -Werror=incompatible-pointer-types
CFLAGS  += -Wcomment
CFLAGS  += -Wno-unused-but-set-variable

CFLAGS  += $(INCLUDE)

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS  := $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections \
            -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# Link with $(CXX) even though this tree is pure C. switch-mesa's libEGL.a
# contains the nouveau shader compiler (nv50_ir), which is C++; the stock
# devkitPro template picks $(CC) when there are no .cpp files, and the link then
# fails with hundreds of undefined operator new / std::__detail references.
#---------------------------------------------------------------------------------
# GNU ld resolves left to right, so each library must come before the ones it
# depends on: libGLESv2 needs libglapi, libEGL needs libglapi and
# libdrm_nouveau. This is the order the Killer Bean port links in.
# -lpng for nx_pointer's optional cursor.png; it must precede -lz.
# SDL2 backs opensles.c's output device. -lpng is for nx_pointer's optional
# cursor.png and MUST precede -lz: GNU ld resolves left to right and would
# otherwise finish with zlib before learning libpng needed it.
LIBS    := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin)))

export LD := $(CXX)

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(PORTLIBS)/include \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
  icons := $(wildcard *.jpg)
  ifneq (,$(findstring $(TARGET).jpg,$(icons)))
    export APP_ICON := $(TOPDIR)/$(TARGET).jpg
  endif
else
  ifneq (,$(wildcard $(TOPDIR)/$(ICON)))
    export APP_ICON := $(TOPDIR)/$(ICON)
  endif
endif

#---------------------------------------------------------------------------------
# The title, author, version and icon only reach the console through these.
#
# Setting APP_TITLE and friends is not enough on its own. The NRO carries them
# in an asset section, and elf2nro only writes that section when it is passed
# --nacp and --icon; without NROFLAGS it emits a bare NRO and the home menu
# falls back to the filename with no author, no version and no icon. This
# Makefile set APP_ICON and never built NROFLAGS, which is exactly what that
# looks like on hardware.
#
# The three APP_* variables also have to be exported: the .nacp is generated by
# the recursive make running inside build/, which does not inherit them
# otherwise.
#---------------------------------------------------------------------------------
export APP_TITLE APP_AUTHOR APP_VERSION

ifeq ($(strip $(NO_NACP)),)
  export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(strip $(APP_ICON)),)
  export NROFLAGS += --icon=$(APP_ICON)
endif

.PHONY: all clean check

all: check $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#   check_stale     the Makefile globs source/*.c and updates ship as a zip;
#                   extracting over an existing tree never deletes removed
#                   files, so an orphan from an older version keeps compiling.
#                   Fatal, and checked first: a stale file breaks the build
#                   anyway, and the error it produces points at a file that is
#                   no longer part of the port.
#
# Three host-side checks before every build, each catching a class of failure
# that is otherwise invisible until much later:
#
#   check_includes  a glibc-only header compiles fine on a Linux host and dies
#                   on newlib
#   check_links     every file can compile and the tree still not link -- a
#                   missing definition or a duplicate one is only visible
#                   across the whole tree. Both happened here; see the header
#                   comment in check_links.py
#   verify_imports  an unresolved import is a load-time abort on hardware with
#                   no other symptom, because libpapapearsaga.so is BIND_NOW
#   check_jni_      the outbound JNI surface is small and fully enumerable, so
#     coverage      it can be checked against the binary rather than assumed.
#                   This is what caught a dispatcher arm matching on invented
#                   method names that appear nowhere in libpapapearsaga.so.
#   check_stub_     several functions libc_shim calls are predicates -- "did I
#     polarity      handle this?" -- and a stub returning -1 there is TRUE, so
#                   the caller reads back uninitialised outputs and reports
#                   success. That produced garbage from every fstat in the game.
#   check_        this port drives a GLSurfaceView game, so every entry point
#     entrypoints the Java layer used to call, main.c must now call itself.
#                 Missing one is silent. nativeRecreateFBO was bound and only
#                 called on a dock, so on a normal boot the render FBO was
#                 never created and a fullscreen quad of texture 0 covered
#                 the screen for several test cycles.
#   check_jni_      the JNIEnv/JavaVM vtable indices can be read out of the
#     slots         binary too. This caught DetachCurrentThread and
#                   AttachCurrentThread being swapped -- a wild store on every
#                   bridge call -- and an empty CallStaticBooleanMethodV.
# NOCHECK=1 skips all of them. They are advisory: none is required to produce
# a working build, and a check that can stop a build is worse than no check.
# check_links.py already exits 0 when it cannot find a compiler (MSYS2 ships
# no host gcc), but `-` here means even a broken Python cannot halt `make`.
ifeq ($(strip $(NOCHECK)),)
check:
	-@python3 tools/check_includes.py source/
	-@python3 tools/check_links.py
ifneq ($(strip $(SO)),)
	@python3 tools/verify_imports.py $(SO) source/imports.c
	-@python3 tools/check_entrypoints.py $(SO)
endif
else
check:
	@echo "checks skipped (NOCHECK=1)"
endif

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

ifeq ($(strip $(APP_JSON)),)
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).json
endif

$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
