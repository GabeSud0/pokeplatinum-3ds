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
             berry_data.c poffin_types.c
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
               -I$(RESOURCE_DIR) -I$(GAME)/lib/spl/include \
               -I$(NETWORK_DIR)/NitroDWC/include \
               -I$(NETWORK_DIR)/NitroDWC/include/gs \
               -I$(NETWORK_DIR)/NitroDWC/include/base \
               -I$(NETWORK_DIR)/NitroWiFi/include \
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
$(OUT)/src/%.o: $(GAME)/src/%.c $(NNS_INLINE_SYMS) $(CONSTANTS_STAMP) $(RESOURCE_HEADER) | libnitrosystem
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

$(OUT)/tests/test_game.o: tests/test_game.c $(NNS_INLINE_SYMS) $(CONSTANTS_STAMP) $(RESOURCE_HEADER) | libnitrosystem
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

probe: $(CONSTANTS_STAMP) $(RESOURCE_HEADER) $(NETWORK_STAMP) | libnitrosystem
	@python3 $(ROOT)/tools/probe_game_compile.py \
	    --source-dir="$(GAME)/src" \
	    --compiler="$(CC)" \
	    --flags="$(GAME_CFLAGS)" \
	    --target="$(TARGET)" \
	    --limit="$(PROBE_LIMIT)" \
	    --timeout="$(PROBE_TIMEOUT)" \
	    --output="$(PROBE_REPORT)"

clean:
	rm -rf build
