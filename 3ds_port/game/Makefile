# Incremental GCC build of the original Pokémon Platinum game sources.
# Always fetch the pinned decompilation first: python tools/bootstrap.py --clean
#
# make                 Compile and smoke-test the first game-code slice on PC
# make TARGET=host32   Repeat with 32-bit pointers, as on the 3DS
# make TARGET=3ds      Cross-compile the same game code for devkitARM
#
# This is a relocatable object library, NOT a playable game executable.
# Expand GAME_SRCS after each subsystem compiles cleanly and is covered by CI.

.DEFAULT_GOAL := all

include ../common.mk

GAME := $(ROOT)/build/upstream
OUT  := build/$(TARGET)
LIB  := $(OUT)/libplatinum_game.a
NNS_PORT := $(ROOT)/3ds_port/nitrosystem
LIBNITRO := $(NITRO)/build/$(TARGET)/libnitro.a
LIBNNS   := $(NNS_PORT)/build/$(TARGET)/libnitrosystem.a

ifeq ($(wildcard $(GAME)/include/pch/global_pch.h),)
$(error Game decompilation not found at $(GAME). Run: python tools/bootstrap.py --clean)
endif

# These are actual unmodified pokeplatinum C files, not port rewrites.
# Start with code that does not require the generated Meson constants, ROM
# contents, overlay linker scripts, or unimplemented DS hardware backends.
GAME_SRCS := coins.c charcode.c charcode_util.c ascii_util.c game_version.c \
             play_time.c int_distance.c math_util.c fx_util.c \
             map_tile_behavior.c field_script_context.c \
             play_time_manager.c poffin.c game_options.c \
             berry_data.c poffin_types.c \
             grid_menu_cursor_position.c overworld_map_history.c \
             pokedex_language.c persisted_map_features.c \
             berry_tag_data.c pokedex_memory.c \
             pokedex_data_index.c ribbon_save_data.c \
             battle_regulation.c record_mixed_rng.c
GAME_OBJS := $(GAME_SRCS:%.c=$(OUT)/src/%.o)

# Use the actual Meson/Metang rules from the pinned decompilation for ALL
# 107 generated enum/mask headers, including TextBank. Do not vendor these
# headers or hand-code game IDs. A stamp prevents rebuilding the entire set
# per C source, while manifest prerequisites preserve incremental correctness.
CONSTANTS_DIR := $(ROOT)/build/game-constants
CONSTANTS_STAMP := $(CONSTANTS_DIR)/.game-headers-ready
GAME_MANIFESTS := $(wildcard $(GAME)/generated/*.txt)

$(CONSTANTS_STAMP): $(GAME)/generated/meson.build $(GAME)/subprojects/metang.wrap $(GAME_MANIFESTS) $(ROOT)/tools/prepare_game_constants.py
	@python3 $(ROOT)/tools/prepare_game_constants.py --upstream $(GAME) --output $(CONSTANTS_DIR)
	@touch $@

# Generate only metadata the original build's dexproc would emit.
# Other resource/NAIX headers require their real native generators and
# must not be replaced with fake definitions or ROM/game content.
RESOURCE_DIR := $(ROOT)/build/game-resource-headers
RESOURCE_HEADER := $(RESOURCE_DIR)/res/pokemon/regional_pokedex_size.h
RESOURCE_MANIFEST := $(GAME)/res/pokemon/sinnoh_pokedex.json

$(RESOURCE_HEADER): $(RESOURCE_MANIFEST) $(ROOT)/tools/prepare_game_resource_headers.py
	@python3 $(ROOT)/tools/prepare_game_resource_headers.py --source="$<" --output="$@"

# Mirror original msgenc's *identifier-only* text-bank headers directly
# from the pinned JSON message list. No dialogue, NARC, or data compiled.
TEXT_BANK_JSON := $(wildcard $(GAME)/res/text/*.json)
TEXT_BANK_STAMP := $(RESOURCE_DIR)/.text-bank-ids-ready

$(TEXT_BANK_STAMP): $(TEXT_BANK_JSON) $(ROOT)/tools/prepare_game_text_bank_headers.py
	@python3 $(ROOT)/tools/prepare_game_text_bank_headers.py --upstream="$(GAME)" --output="$(RESOURCE_DIR)"
	@touch $@

# Original moveproc creates four additional text banks from the pinned
# sequential moves enum and one data.json per move. Emit only message IDs
# and counts, not names, descriptions, battle text or NARC contents.
MOVE_TEXT_DATA := $(wildcard $(GAME)/res/moves/*/data.json)
MOVE_TEXT_STAMP := $(RESOURCE_DIR)/.move-text-ids-ready

$(MOVE_TEXT_STAMP): $(GAME)/generated/moves.txt $(MOVE_TEXT_DATA) $(ROOT)/tools/prepare_game_move_text_headers.py
	@python3 $(ROOT)/tools/prepare_game_move_text_headers.py --upstream="$(GAME)" --output="$(RESOURCE_DIR)"
	@touch $@

# The original speciesproc emits species_name and species_name_with_articles
# in the enum registry order, ending at SPECIES_BAD_EGG. Prepare only those
# original message IDs/counts, not name strings, text or graphics resources.
SPECIES_TEXT_DATA := $(wildcard $(GAME)/res/pokemon/*/data.json)
SPECIES_TEXT_STAMP := $(RESOURCE_DIR)/.species-text-ids-ready

$(SPECIES_TEXT_STAMP): $(GAME)/generated/species.txt $(SPECIES_TEXT_DATA) $(ROOT)/tools/prepare_game_species_text_headers.py
	@python3 $(ROOT)/tools/prepare_game_species_text_headers.py --upstream="$(GAME)" --output="$(RESOURCE_DIR)"
	@touch $@

# Reproduce NitroSFX's -naix symbol indices from the upstream SDAT manifest,
# with no sound/sample generation and no ROM/resource extraction.
SOUND_MANIFEST := $(GAME)/res/sound/pl_sound_data.json
SOUND_HEADER := $(RESOURCE_DIR)/res/sound/pl_sound_data.naix

$(SOUND_HEADER): $(SOUND_MANIFEST) $(ROOT)/tools/prepare_game_sound_naix.py
	@python3 $(ROOT)/tools/prepare_game_sound_naix.py --upstream="$(GAME)" --output="$(RESOURCE_DIR)"

# NitroArc --files-from uses pinned .order manifests for graphics and
# two additional archives (encounter effects and battle subscripts). The
# Pokédex output is zukan.naix even though its input is pokedex.order.
# Reproduce only explicit indices, never assets or implicit members.
ARCHIVE_ORDERS := $(shell find $(GAME)/res/graphics -type f -name '*.order') \
                  $(GAME)/res/trainers/classes/field_encounteffect.order \
                  $(GAME)/res/battle/scripts/subscripts/sub_seq.order
ARCHIVE_STAMP := $(RESOURCE_DIR)/.archive-naix-ready

$(ARCHIVE_STAMP): $(ARCHIVE_ORDERS) $(ROOT)/tools/prepare_game_graphics_naix.py
	@python3 $(ROOT)/tools/prepare_game_graphics_naix.py --upstream="$(GAME)" --output="$(RESOURCE_DIR)"
	@touch $@

# Network headers are available from the exact upstream pinned NitroDWC
# and NitroWiFi revisions. Fetch them only for broad compiler diagnostics,
# not for the already-working 16-file game/SDK compile regression.
NETWORK_DIR := $(ROOT)/build/game-network-sdks
NETWORK_STAMP := $(NETWORK_DIR)/.headers-ready
NETWORK_WRAPS := $(GAME)/subprojects/NitroDWC.wrap $(GAME)/subprojects/NitroWiFi.wrap

$(NETWORK_STAMP): $(NETWORK_WRAPS) $(ROOT)/tools/prepare_game_network_headers.py
	@python3 $(ROOT)/tools/prepare_game_network_headers.py --upstream="$(GAME)" --output="$(NETWORK_DIR)"
	@touch $@

# Force the original global precompiled header, as Meson/Metrowerks does.
# _MSL_RESTRICT is a Metrowerks qualifier and must be empty with GCC.
GAME_CFLAGS := $(PORT_CFLAGS) $(SDK_INCLUDES) $(NNS_INCLUDES) \
               -I$(GAME)/include -I$(GAME)/asm -I$(GAME) -I$(CONSTANTS_DIR) \
               -I$(ROOT)/3ds_port/game/include \
               -I$(RESOURCE_DIR) -I$(GAME)/lib/spl/include \
               -I$(NETWORK_DIR)/NitroDWC/include \
               -I$(NETWORK_DIR)/NitroDWC/include/gs \
               -I$(NETWORK_DIR)/NitroDWC/include/base \
               -I$(NETWORK_DIR)/NitroWiFi/include \
               -I$(GAME)/lib/gds/include \
               -DPOKEPLATINUM_REVISION=1 -DPOKEPLATINUM_GENERATED_ENUM \
               -D_MSL_RESTRICT= -DPM_KEEP_ASSERTS \
               -DGAME_VERSION=VERSION_PLATINUM \
               -DGAME_LANGUAGE=LANGUAGE_ENGLISH \
               -include $(GAME)/include/pch/global_pch.h \
               -ffunction-sections -fdata-sections

.PHONY: all lib test probe clean libnitro libnitrosystem

ifeq ($(IS_HOST),yes)
all: test
else
all: lib
endif

lib: $(LIB)

libnitro:
	@$(MAKE) --no-print-directory -C $(NITRO) TARGET=$(TARGET) lib

libnitrosystem:
	@$(MAKE) --no-print-directory -C $(NNS_PORT) TARGET=$(TARGET) lib

$(LIBNITRO): libnitro
$(LIBNNS): libnitrosystem

# nitrosystem's make generates the weak-inline symbol list together with
# libnitrosystem, but Make must know how to satisfy this prerequisite here.
$(NNS_INLINE_SYMS): | libnitrosystem
	@test -f $@

# The NitroSystem inline functions in included game headers have external
# definitions under GCC's gnu89 semantics; match the SDK's weak-symbol fix.
$(OUT)/src/%.o: $(GAME)/src/%.c $(NNS_INLINE_SYMS) $(CONSTANTS_STAMP) $(RESOURCE_HEADER) $(TEXT_BANK_STAMP) $(MOVE_TEXT_STAMP) $(SPECIES_TEXT_STAMP) $(SOUND_HEADER) $(ARCHIVE_STAMP) | libnitrosystem
	@mkdir -p $(dir $@)
	@echo "  GAME CC $(notdir $<)"
	@$(CC) $(GAME_CFLAGS) -c $< -o $@
	@$(WEAKEN_NNS_INLINE)

$(LIB): $(GAME_OBJS)
	@rm -f $@
	@$(AR) rcs $@ $^
	@echo "built $@ (game-source slice: $(words $(GAME_SRCS)) C files)"

# Independent host tests exercise the ORIGINAL game functions linked against
# NitroSystem and NitroSDK, without any ROM or copyrighted game data.
TEST_BIN := $(OUT)/test_game

$(OUT)/tests/test_game.o: tests/test_game.c $(NNS_INLINE_SYMS) $(CONSTANTS_STAMP) $(RESOURCE_HEADER) $(TEXT_BANK_STAMP) $(MOVE_TEXT_STAMP) $(SPECIES_TEXT_STAMP) $(SOUND_HEADER) $(ARCHIVE_STAMP) | libnitrosystem
	@mkdir -p $(dir $@)
	@$(CC) $(GAME_CFLAGS) -c $< -o $@
	@$(WEAKEN_NNS_INLINE)

$(TEST_BIN): $(OUT)/tests/test_game.o $(LIB) $(LIBNNS) $(LIBNITRO)
	@$(CC) $(LDFLAGS) -Wl,--gc-sections $(OUT)/tests/test_game.o \
	    -Wl,--start-group $(LIB) $(LIBNNS) $(LIBNITRO) -Wl,--end-group -lm -o $@

ifeq ($(IS_HOST),yes)
test: $(TEST_BIN)
	./$(TEST_BIN)
else
test:
	@echo "Host-only tests; run make TARGET=host or TARGET=host32"
	@exit 1
endif

# The diagnostic survey exercises original game C files *without linking*.
# This intentionally does not force every game source into libplatinum_game:
# errors are reported as blockers, not masked by blanket compiler stubs.
# Set PROBE_LIMIT=0 to check every file; use a bounded sample in CI.
PROBE_LIMIT ?= 64
PROBE_TIMEOUT ?= 10
PROBE_REPORT := $(ROOT)/build/game-compile-survey/$(TARGET).json

# GameSpy nonport.h supports _UNIX via _LINUX, and _NITRO for the
# original DS target. These are DIAGNOSTIC-ONLY platform selectors:
# building game logic does not imply networking support or link its APIs.
# Never force _LINUX on devkitARM (POSIX sockets are not the DS interface).
ifeq ($(IS_HOST),yes)
GAME_PROBE_PLATFORM_FLAGS := -D_LINUX
else
GAME_PROBE_PLATFORM_FLAGS := -D_NITRO
endif
GAME_PROBE_CFLAGS := $(GAME_CFLAGS) $(GAME_PROBE_PLATFORM_FLAGS)

probe: $(CONSTANTS_STAMP) $(RESOURCE_HEADER) $(TEXT_BANK_STAMP) $(MOVE_TEXT_STAMP) $(SPECIES_TEXT_STAMP) $(SOUND_HEADER) $(ARCHIVE_STAMP) $(NETWORK_STAMP) | libnitrosystem
	@python3 $(ROOT)/tools/probe_game_compile.py \
	    --source-dir="$(GAME)/src" \
	    --compiler="$(CC)" \
	    --flags="$(GAME_PROBE_CFLAGS)" \
	    --target="$(TARGET)" \
	    --limit="$(PROBE_LIMIT)" \
	    --timeout="$(PROBE_TIMEOUT)" \
	    --output="$(PROBE_REPORT)"

clean:
	rm -rf build
