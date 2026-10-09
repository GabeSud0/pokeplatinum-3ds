# ---------------------------------------------------------------------------
# Settings shared by everything the port compiles from Nintendo's and the
# decompilation's C: libnitro (NitroSDK), libnitrosystem (NitroSystem) and,
# later, the game. Included by those Makefiles; not run on its own.
#
# Defines, for the including Makefile:
#   TARGET       host (PC, for tests), host32 (32-bit PC, for tests that need
#                the 3DS's 4-byte pointers) or 3ds (default: host)
#   IS_HOST      yes for host and host32
#   ROOT         repository root
#   SDK, NNS     build/NitroSDK, build/NitroSystem (fetched by tools/bootstrap.py)
#   CC, AR, NM, OBJCOPY  the toolchain for TARGET
#   PORT_CFLAGS  flags every file is compiled with
#   SDK_INCLUDES include paths for NitroSDK (and the libnitro port headers)
#   NNS_INCLUDES include paths for NitroSystem
#   LDFLAGS      link flags for test programs (host only)
#   NNS_INLINE_SYMS  NitroSystem's header inline functions (built by
#                3ds_port/nitrosystem); every object that includes NitroSystem
#                headers must be passed through $(WEAKEN_NNS_INLINE)
# ---------------------------------------------------------------------------

TARGET ?= host
ROOT   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
SDK    := $(ROOT)/build/NitroSDK
NNS    := $(ROOT)/build/NitroSystem
NITRO  := $(ROOT)/3ds_port/nitro

ifeq ($(wildcard $(SDK)/include/nitro.h),)
$(error NitroSDK not found at $(SDK). Run: python tools/bootstrap.py)
endif

# The same SDK configuration pokeplatinum builds with, minus the Metrowerks
# compiler (SDK_CW), plus SDK_PORT_3DS which turns on our patches.
PORT_DEFINES := -DSDK_ARM9 -DSDK_TS -DSDK_4M -DSDK_CODE_ARM -DSDK_FINALROM \
                -DNNS_FINALROM -DSDK_PORT_3DS

# Matching how Metrowerks compiled the originals:
#   gnu89 inline rules match how the SDK's .c files use `inline`.
#   No strict aliasing: the SDK reinterprets memory through casts on purpose.
#   Enums are always 4 bytes ("-enum int"); ARM GCC would otherwise shrink
#   them and break structure layouts.
#   char is signed ("-char signed"); on ARM, GCC's char is unsigned by
#   default, which silently changes comparisons like `c < 0`.
PORT_CFLAGS := -std=gnu99 -fgnu89-inline -fno-strict-aliasing -fno-short-enums \
               -fsigned-char -O2 -g \
               -Wall -Wno-unused-function -Wno-unused-variable -Wno-missing-braces \
               -Wno-unknown-pragmas -Wno-unused-but-set-variable -Wno-parentheses \
               -Wno-switch -Wno-multichar \
               $(PORT_DEFINES)

SDK_INCLUDES := -I$(NITRO)/include -I$(NITRO)/build/$(TARGET)/gen -I$(SDK)/include \
                $(foreach d,$(wildcard $(SDK)/libraries/*/include),-I$(d))
NNS_INCLUDES := -I$(NNS)/include \
                $(foreach d,$(wildcard $(NNS)/libraries/*/include),-I$(d))

ifeq ($(TARGET),3ds)
  ifeq ($(strip $(DEVKITARM)),)
    $(error Set DEVKITARM (install devkitPro with 3ds-dev))
  endif
  PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
  CC     := $(PREFIX)gcc
  AR     := $(PREFIX)gcc-ar
  NM     := $(PREFIX)nm
  OBJCOPY := $(PREFIX)objcopy
  # The 3DS's ARM11 MPCore, with its VFP floating-point unit.
  PORT_CFLAGS += -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard -mtp=soft \
                 -mword-relocations -ffunction-sections -D__3DS__
else ifneq ($(filter host host32,$(TARGET)),)
  IS_HOST := yes
  CC     ?= cc
  AR     ?= ar
  NM     ?= nm
  OBJCOPY ?= objcopy
  # The SDK stores addresses in 32-bit integers. On a 64-bit PC that only
  # works if our data sits below 4 GB, which a non-PIE executable guarantees.
  PORT_CFLAGS += -fno-pie -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
  LDFLAGS += -no-pie
  ifeq ($(TARGET),host32)
    # Nintendo's file formats hold 4-byte pointers that the libraries fix up
    # in place, so those parts only work with 32-bit pointers, as on the 3DS.
    # -malign-double aligns 64-bit values to 8 bytes like ARM (x86 uses 4).
    # Needs 32-bit C libraries (Ubuntu: gcc-multilib).
    PORT_CFLAGS += -m32 -malign-double
    LDFLAGS += -m32
  endif
else
  $(error TARGET must be host, host32 or 3ds)
endif

# NitroSystem's headers define small functions as plain `inline`. Metrowerks
# kept one copy of each; GCC exports one from every file that includes the
# header, and those files then can't be linked together. So after compiling,
# mark them weak: the linker keeps one copy, as Metrowerks did. The list is
# made by compiling every NitroSystem header (tools/nns_inline_symbols.py).
NNS_INLINE_SYMS   := $(ROOT)/3ds_port/nitrosystem/build/$(TARGET)/gen/nns_inline.syms
WEAKEN_NNS_INLINE  = $(OBJCOPY) --weaken-symbols=$(NNS_INLINE_SYMS) $@
