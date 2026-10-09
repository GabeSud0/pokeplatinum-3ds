# ---------------------------------------------------------------------------
# libnitro: Nintendo's NitroSDK, compiled with GCC for the 3DS (and for PC,
# so it can be unit tested).
#
#   make                 build for PC and run the tests     (TARGET=host)
#   make TARGET=3ds      build the library for the 3DS's ARM11 (needs devkitARM)
#   make clean
#
# The SDK sources come from build/NitroSDK (run tools/bootstrap.py first).
# Our changes to them are in patches/NitroSDK/; code that replaces parts of
# the SDK entirely is in src/ here.
# ---------------------------------------------------------------------------

TARGET   ?= host
ROOT     := $(abspath ../..)
SDK      := $(ROOT)/build/NitroSDK
OUT      := build/$(TARGET)
GEN      := $(OUT)/gen

ifeq ($(wildcard $(SDK)/include/nitro.h),)
$(error NitroSDK not found at $(SDK). Run: python tools/bootstrap.py)
endif

# --- Sources ---------------------------------------------------------------

# NitroSDK files compiled as they are (with our patches applied).
SDK_SRCS := \
	fx/src/fx.c \
	fx/src/fx_asinacosidx.c \
	fx/src/fx_atan.c \
	fx/src/fx_atanidx.c \
	fx/src/fx_cp.c \
	fx/src/fx_mtx22.c \
	fx/src/fx_mtx33.c \
	fx/src/fx_mtx43.c \
	fx/src/fx_mtx44.c \
	fx/src/fx_sincos.c \
	fx/src/fx_trig.c \
	fx/src/fx_vec.c \
	math/src/checksum.c \
	math/src/crc.c \
	math/src/dgt.c \
	math/src/fft.c \
	math/src/math.c

# Not built: math/src/qsort.c (assembly only, and the game never calls it).
# Replaced by our own C in src/:
#   mi/src/mi_memory.c, mi/src/mi_swap.c   (Metrowerks assembly only)
#   dgt/prebuilt/libdgt.a                  (MD5/SHA-1, shipped as a binary)
#   the divider and square-root hardware   (src/port_cp.c)

PORT_SRCS := $(wildcard src/*.c)

SDK_OBJS  := $(SDK_SRCS:%.c=$(OUT)/sdk/%.o)
PORT_OBJS := $(PORT_SRCS:src/%.c=$(OUT)/port/%.o)
LIB       := $(OUT)/libnitro.a

# --- Flags -----------------------------------------------------------------

# The same SDK configuration pokeplatinum builds with, minus the Metrowerks
# compiler (SDK_CW), plus SDK_PORT_3DS which turns on our patches.
DEFINES := -DSDK_ARM9 -DSDK_TS -DSDK_4M -DSDK_CODE_ARM -DSDK_FINALROM \
           -DNNS_FINALROM -DSDK_PORT_3DS

# gnu89 inline rules match how the SDK's .c files use `inline`.
# No strict aliasing: the SDK reinterprets memory through casts on purpose.
CFLAGS := -std=gnu99 -fgnu89-inline -fno-strict-aliasing -O2 -g \
          -Wall -Wno-unused-function -Wno-unused-variable -Wno-missing-braces \
          -Wno-unknown-pragmas -Wno-unused-but-set-variable -Wno-parentheses \
          $(DEFINES) -Iinclude -I$(GEN) -I$(SDK)/include

ifeq ($(TARGET),3ds)
  ifeq ($(strip $(DEVKITARM)),)
    $(error Set DEVKITARM (install devkitPro with 3ds-dev))
  endif
  PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
  CC     := $(PREFIX)gcc
  AR     := $(PREFIX)gcc-ar
  CFLAGS += -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
            -mword-relocations -ffunction-sections -D__3DS__
else ifeq ($(TARGET),host)
  CC     ?= cc
  AR     ?= ar
  # The SDK stores addresses in 32-bit integers. On a 64-bit PC that only
  # works if our data sits below 4 GB, which a non-PIE executable guarantees.
  CFLAGS += -fno-pie -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
  LDFLAGS += -no-pie
else
  $(error TARGET must be host or 3ds)
endif

# --- Rules -----------------------------------------------------------------

.PHONY: all lib test clean

ifeq ($(TARGET),host)
all: test
else
all: lib
endif

lib: $(LIB)

$(GEN)/nitro/fx/fx_const.h: $(SDK)/gen/nitro/fx/fx_const.csv $(SDK)/gen/nitro/fx/gen_fx_const.py
	@mkdir -p $(dir $@)
	python3 $(SDK)/gen/nitro/fx/gen_fx_const.py $< $@

$(OUT)/sdk/%.o: $(SDK)/libraries/%.c $(GEN)/nitro/fx/fx_const.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/port/%.o: src/%.c $(GEN)/nitro/fx/fx_const.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(SDK_OBJS) $(PORT_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "built $@"

# Unit tests run on the PC only.
TEST_BIN := $(OUT)/test_nitro

$(TEST_BIN): tests/test_nitro.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIB) -lm -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf build
