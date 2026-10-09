# Standalone portable C99 reader for docs/PAK_FORMAT.md.
#   make                Build host library and run synthetic tests
#   make TARGET=host32  Build and run 32-bit tests (gcc-multilib required)
#   make TARGET=3ds     Build ARM11 libpak.a (devkitARM required)
#   make clean

TARGET ?= host
OUT := build/$(TARGET)
INCLUDES := -Iinclude
CFLAGS := -std=c99 -O2 -g -Wall -Wextra -Werror -pedantic $(INCLUDES)

ifeq ($(TARGET),3ds)
  ifeq ($(strip $(DEVKITARM)),)
    $(error Set DEVKITARM to your devkitPro devkitARM installation)
  endif
  CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
  AR := $(DEVKITARM)/bin/arm-none-eabi-ar
  # Match 3ds_port/common.mk: 4-byte enums (ARM GCC shrinks them otherwise,
  # so PakResult would disagree with the game's code) and the same ABI flags.
  CFLAGS += -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard -mtp=soft \
            -fno-short-enums -mword-relocations -ffunction-sections
  DEFAULT_TARGET := lib
else ifeq ($(TARGET),host32)
  CC := gcc
  AR := ar
  CFLAGS += -m32
  LDFLAGS += -m32
  DEFAULT_TARGET := test
else ifeq ($(TARGET),host)
  CC := gcc
  AR := ar
  DEFAULT_TARGET := test
else
  $(error TARGET must be host, host32 or 3ds)
endif

LIB := $(OUT)/libpak.a
TEST_BIN := $(OUT)/test_pak
PYTHON_FIXTURE := $(OUT)/python-fixture.pak

.PHONY: all lib test clean
all: $(DEFAULT_TARGET)
lib: $(LIB)

$(OUT)/pak.o: src/pak.c include/pak.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OUT)/pak.o
	$(AR) rcs $@ $^

ifneq ($(TARGET),3ds)
$(TEST_BIN): tests/test_pak.c include/pak.h $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIB) -o $@

# Generated locally using only synthetic bytes and builder/pack.py.
$(PYTHON_FIXTURE): tests/make_python_fixture.py ../../builder/pack.py
	@mkdir -p $(OUT)
	python3 tests/make_python_fixture.py $@

test: $(TEST_BIN) $(PYTHON_FIXTURE)
	./$(TEST_BIN) $(OUT) $(PYTHON_FIXTURE)
else
test:
	@echo "Unit tests require TARGET=host or TARGET=host32" >&2; exit 1
endif

clean:
	rm -rf build
