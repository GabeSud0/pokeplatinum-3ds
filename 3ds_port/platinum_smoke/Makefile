# Native 3DS smoke executable linked against ORIGINAL pret/pokeplatinum C.
# This is an interactive diagnostics app, NOT the full NitroMain/game.
#
# Requirements: devkitARM + libctru, python3, git, pinned upstream checkout.
# From repo root:
#   python3 tools/bootstrap.py --clean
#   make -C 3ds_port/platinum_smoke
#
# The original game library (and both SDKs) are built automatically.

.SUFFIXES:
.DEFAULT_GOAL := all

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB := $(DEVKITPRO)/libctru
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
SMOKE := platinum-smoke
BUILD := build
GAME_LIB := ../game/build/3ds/libplatinum_game.a
NNS_LIB := ../nitrosystem/build/3ds/libnitrosystem.a
NITRO_LIB := ../nitro/build/3ds/libnitro.a

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := $(ARCH) -std=gnu11 -O2 -g -Wall -Wextra -Werror \
          -mword-relocations -ffunction-sections -fdata-sections \
          -I$(CTRULIB)/include -D__3DS__
LDFLAGS := $(ARCH) -specs=3dsx.specs \
           -Wl,--gc-sections -Wl,-Map,$(SMOKE).map

.PHONY: all clean original-game-libs
all: $(SMOKE).3dsx

# Do not fork/stub original game functions. Build the SAME 33+ original
# objects that the game library host/3DS CI already compiles and tests.
original-game-libs:
	@$(MAKE) --no-print-directory -C ../game TARGET=3ds lib

$(BUILD)/main.o: source/main.c source/platinum_api.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# original-game-libs is intentionally phony so incremental game-library
# changes are always seen by the native linker.
$(SMOKE).elf: $(BUILD)/main.o original-game-libs
	$(CC) $(LDFLAGS) $(BUILD)/main.o \
	    -Wl,--start-group $(GAME_LIB) $(NNS_LIB) $(NITRO_LIB) -Wl,--end-group \
	    -L$(CTRULIB)/lib -lctru -lm -o $@

$(SMOKE).smdh:
	$(DEVKITPRO)/tools/bin/smdhtool --create \
	    "Platinum smoke" "Original Platinum code on 3DS" "pokeplatinum-3ds" \
	    "$(CTRULIB)/default_icon.png" $@

$(SMOKE).3dsx: $(SMOKE).elf $(SMOKE).smdh
	$(DEVKITPRO)/tools/bin/3dsxtool $< $@ --smdh=$(SMOKE).smdh
	@echo "Built $@ (original Platinum logic linked; NOT a playable game)"

clean:
	rm -rf $(BUILD) $(SMOKE).3dsx $(SMOKE).smdh $(SMOKE).elf $(SMOKE).map
