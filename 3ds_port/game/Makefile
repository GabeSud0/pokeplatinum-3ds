# Incremental GCC build of the original Pokémon Platinum game sources.
# Always fetch the pinned decompilation first: python tools/bootstrap.py --clean
#
# make                 Compile and smoke-test the first game-code slice on PC
# make TARGET=host32   Repeat with 32-bit pointers, as on the 3DS
# make TARGET=3ds      Cross-compile the same game code for devkitARM
#
# This is a relocatable object library, NOT a playable game executable.
# Builds every game source that compiles; see blocked_sources.txt for the rest.

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

# Every original pokeplatinum C file, unmodified (apart from the patches in
# patches/pokeplatinum), except the ones listed in blocked_sources.txt with
# the reason each doesn't compile yet. On a 64-bit PC a few more are left out
# (blocked_sources_64bit.txt). Fix a cause, delete its lines, and those files
# join the library; `make probe PROBE_LIMIT=0` reports what compiles.
blocklist = $(shell sed -e 's/\#.*//' -e '/^[[:space:]]*$$/d' $(1))
GAME_BLOCKED := $(call blocklist,blocked_sources.txt)
ifeq ($(TARGET),host)
GAME_BLOCKED += $(call blocklist,blocked_sources_64bit.txt)
endif
GAME_ALL_SRCS := $(sort $(patsubst $(GAME)/src/%,%,$(shell find $(GAME)/src -name '*.c')))
GAME_SRCS := $(filter-out $(GAME_BLOCKED),$(GAME_ALL_SRCS))
GAME_OBJS := $(GAME_SRCS:%.c=$(OUT)/src/%.o)

# The game's own libraries that the original links into the same program
# (meson.build: libcrypto, libgds, libspl). libdwcppwlobby/libdwcenc are
# left out with the rest of online play.
GAME_LIBS := crypto gds spl
GAME_LIB_SRCS := $(sort $(foreach l,$(GAME_LIBS),$(wildcard $(GAME)/lib/$(l)/src/*.c)))
GAME_OBJS += $(GAME_LIB_SRCS:$(GAME)/%.c=$(OUT)/%.o)

# Game headers (inlines.h, pokemon_sprite.h, NitroDWC's dwc_friend.h ...)
# define functions with plain `inline`. Metrowerks kept one copy; GCC
# exports one from every file that includes the header, and those files then
# can't be linked together. The original game linked as one program, so a
# symbol defined in more than one object can only be such a header copy:
# after compiling, list those and mark them weak in every object (as for
# NitroSystem's headers). The linker then keeps one copy, as Metrowerks did.
# The host test object is counted too, since it includes the same headers.
GAME_HEADER_DEFS := $(OUT)/gen/game_header_defs.syms
TEST_OBJ := $(OUT)/tests/test_game.o
CENSUS_OBJS := $(GAME_OBJS) $(if $(IS_HOST),$(TEST_OBJ))

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

# NitroArc --files-from uses pinned .order manifests: all graphics archives,
# and every archive whose res/**/meson.build rule names its .order list and
# .naix output literally (fonts, field props and scripts ...). The
# Pokédex output is zukan.naix even though its input is pokedex.order.
# Reproduce only explicit indices, never assets or implicit members.
ARCHIVE_ORDERS := $(shell find $(GAME)/res -type f \( -name '*.order' -o -name meson.build \))
ARCHIVE_STAMP := $(RESOURCE_DIR)/.archive-naix-ready

$(ARCHIVE_STAMP): $(ARCHIVE_ORDERS) $(ROOT)/tools/prepare_game_graphics_naix.py
	@python3 $(ROOT)/tools/prepare_game_graphics_naix.py --upstream="$(GAME)" --output="$(RESOURCE_DIR)"
	@touch $@

# Archives whose member list a tools/ordergen/ script writes (trainer
# sprites, footprints): run the decompilation's own script with the folder
# list its meson.build derives, then index the result.
ORDERGEN_STAMP := $(RESOURCE_DIR)/.ordergen-naix-ready

$(ORDERGEN_STAMP): $(wildcard $(GAME)/tools/ordergen/*.py) $(GAME)/generated/trainer_classes.txt \
                   $(GAME)/generated/species.txt $(ROOT)/tools/prepare_game_ordergen_naix.py \
                   $(ROOT)/tools/prepare_game_graphics_naix.py
	@python3 $(ROOT)/tools/prepare_game_ordergen_naix.py --upstream="$(GAME)" --output="$(RESOURCE_DIR)"
	@touch $@

# Network headers come from exact pinned NitroDWC and NitroWiFi revisions.
# The game save structs now need NitroDWC's genuine account declarations,
# so fetch these headers before compiling original game translation units.
# This does NOT enable GameSpy networking or link an online-play runtime.
NETWORK_DIR := $(ROOT)/build/game-network-sdks
NETWORK_STAMP := $(NETWORK_DIR)/.headers-ready
NETWORK_WRAPS := $(GAME)/subprojects/NitroDWC.wrap $(GAME)/subprojects/NitroWiFi.wrap \
                 $(GAME)/subprojects/libvct.wrap

$(NETWORK_STAMP): $(NETWORK_WRAPS) $(ROOT)/tools/prepare_game_network_headers.py
	@python3 $(ROOT)/tools/prepare_game_network_headers.py --upstream="$(GAME)" --output="$(NETWORK_DIR)"
	@touch $@

# GCC 14 (devkitARM) turned some old-C warnings into errors; GCC 13 (a
# typical PC) didn't. Set them the same way on every compiler, so a file
# that builds on the PC builds for the 3DS:
#  - errors: implicit function declarations (calls with no declaration in
#    scope). A call through an implicit declaration can silently pass or
#    return the wrong types; patches/pokeplatinum/0003 declares the ones the
#    game makes.
#  - warnings: integer/pointer conversions (mostly NULL used as 0) and
#    implicit int. Metrowerks accepted these, and on the 3DS pointers and int
#    are both 32 bits, so the result is the same as on the DS.
GCC14_ERRORS := -Werror=implicit-function-declaration \
                -Wno-error=implicit-int -Wno-error=int-conversion \
                -Wno-error=incompatible-pointer-types

# Include paths as in the original meson.build: the game's own libraries
# (lib/*/include) and libvct, plus the folder of the generated constant
# headers, which Meson adds by itself for generated headers (a few files
# include "items.h" rather than "generated/items.h").
# -D_NITRO -DLINK_PPWLOBBY: what NitroDWC's build passes on to the game in
# the original (its dependency's compile_args). NitroDWC's GameSpy headers
# (included by about 90 game files,
# main.c among them) pick their platform from this; _NITRO is the DS, as the
# original game was built. Same on every target, so PC and 3DS compile the
# same code. include/extras.h stands in for the Metrowerks header it pulls in.
# This compiles the declarations only; networking is not ported.
# Every generated header the game's sources may include.
GAME_GENERATED := $(CONSTANTS_STAMP) $(RESOURCE_HEADER) $(TEXT_BANK_STAMP) $(MOVE_TEXT_STAMP) \
                  $(SPECIES_TEXT_STAMP) $(SOUND_HEADER) $(ARCHIVE_STAMP) $(ORDERGEN_STAMP) $(NETWORK_STAMP)

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
               -I$(GAME)/lib/ppwlobby/include -I$(GAME)/lib/crypto/include \
               -I$(NETWORK_DIR)/libvct/include \
               -I$(CONSTANTS_DIR)/generated \
               -DPOKEPLATINUM_REVISION=1 -DPOKEPLATINUM_GENERATED_ENUM \
               -D_MSL_RESTRICT= -DPM_KEEP_ASSERTS \
               -DGAME_VERSION=VERSION_PLATINUM \
               -DGAME_LANGUAGE=LANGUAGE_ENGLISH \
               -D_NITRO -DLINK_PPWLOBBY \
               -include $(GAME)/include/pch/global_pch.h \
               -ffunction-sections -fdata-sections \
               $(GCC14_ERRORS)


.PHONY: all lib test probe link-check clean libnitro libnitrosystem

ifeq ($(IS_HOST),yes)
all: test
else
all: lib
endif

lib: $(LIB)

# libnitrosystem's own make builds libnitro first. Run that one first, so
# that with make -j two makes never build libnitro at the same time.
libnitro: libnitrosystem
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
$(OUT)/src/%.o: $(GAME)/src/%.c $(NNS_INLINE_SYMS) $(GAME_GENERATED) | libnitrosystem
	@mkdir -p $(dir $@)
	@echo "  GAME CC $(notdir $<)"
	@$(CC) $(GAME_CFLAGS) -c $< -o $@
	@$(WEAKEN_NNS_INLINE)

$(GAME_HEADER_DEFS): $(CENSUS_OBJS)
	@mkdir -p $(dir $@)
	@# Objects already made weak by an earlier run no longer show up as
	@# duplicates, so keep the names found before.
	@{ cat $@ 2>/dev/null; $(NM) -g --defined-only $^ | awk 'NF == 3 && $$2 ~ /^[TDBR]$$/ { print $$3 }' | sort | uniq -d; } | sort -u > $@.new
	@mv $@.new $@
	@for o in $^; do $(OBJCOPY) --weaken-symbols=$@ $$o; done
	@touch $@
	@echo "  $$(wc -l < $@) game header functions defined in several objects, made weak"

# The game's libraries: same flags, plus their own private headers (src/).
$(OUT)/lib/%.o: $(GAME)/lib/%.c $(NNS_INLINE_SYMS) $(GAME_GENERATED) | libnitrosystem
	@mkdir -p $(dir $@)
	@echo "  GAME CC lib/$*.c"
	@$(CC) $(GAME_CFLAGS) -I$(dir $<) -c $< -o $@
	@$(WEAKEN_NNS_INLINE)

# Stand-ins for the Wi-Fi SDK functions the game calls (online play isn't
# ported): every DWC_, PPW_, VCT_, WVR_ ... symbol the game objects use and
# none defines, made from `nm` after compiling (tools/make_game_wifi_stubs.py).
WIFI_STUBS_C := $(OUT)/gen/wifi_stubs.c
WIFI_STUBS_O := $(OUT)/gen/wifi_stubs.o

# After the weak-symbol step, which touches every object (so depending on
# the objects themselves would redo this on every make).
$(WIFI_STUBS_C): $(GAME_HEADER_DEFS) $(ROOT)/tools/make_game_wifi_stubs.py
	@python3 $(ROOT)/tools/make_game_wifi_stubs.py --nm $(NM) --output $@ \
	    --headers $(NETWORK_DIR) $(GAME)/lib/ppwlobby $(SDK)/include/nitro -- $(GAME_OBJS)
	@touch $@

$(WIFI_STUBS_O): $(WIFI_STUBS_C)
	@$(CC) $(PORT_CFLAGS) $(SDK_INCLUDES) -c $< -o $@

$(LIB): $(GAME_HEADER_DEFS) $(WIFI_STUBS_O)
	@rm -f $@
	@$(AR) rcs $@ $(GAME_OBJS) $(WIFI_STUBS_O)
	@echo "built $@ ($(words $(GAME_SRCS)) of $(words $(GAME_ALL_SRCS)) game C files, and lib/$(subst $(eval) ,+,$(GAME_LIBS)))"

# The overlay ID symbols Nintendo's linker made from the linker spec
# (SDK_OVERLAY_<name>_ID at the overlay's number). Every program that links
# the game passes this file to the linker.
OVERLAY_IDS := $(ROOT)/build/game-link/overlay_ids.ld

$(OVERLAY_IDS): $(GAME)/platinum.us/main.lsf $(ROOT)/tools/prepare_game_overlay_ids.py
	@python3 $(ROOT)/tools/prepare_game_overlay_ids.py --lsf $< --output $@

# Independent host tests exercise the ORIGINAL game functions linked against
# NitroSystem and NitroSDK, without any ROM or copyrighted game data.
TEST_BIN := $(OUT)/test_game

$(TEST_OBJ): tests/test_game.c $(NNS_INLINE_SYMS) $(GAME_GENERATED) | libnitrosystem
	@mkdir -p $(dir $@)
	@$(CC) $(GAME_CFLAGS) -c $< -o $@
	@$(WEAKEN_NNS_INLINE)

$(TEST_BIN): $(TEST_OBJ) $(LIB) $(LIBNNS) $(LIBNITRO) $(OVERLAY_IDS)
	@$(CC) $(LDFLAGS) -Wl,--gc-sections -Wl,--wrap=ErrorHandling_AssertFail $(OUT)/tests/test_game.o \
	    -Wl,--start-group $(LIB) $(LIBNNS) $(LIBNITRO) -Wl,--end-group -lm $(OVERLAY_IDS) -o $@

ifeq ($(IS_HOST),yes)
test: $(TEST_BIN)
	./$(TEST_BIN)
else
test:
	@echo "Host-only tests; run make TARGET=host or TARGET=host32"
	@exit 1
endif

# Link the whole game from NitroMain and report every symbol still missing
# (tools/game_link_report.py). A to-do list, not a gate: always succeeds.
LINK_CHECK_OBJ := $(OUT)/link/link_check.o

LINK_REPORT := $(ROOT)/build/game-link-report/$(TARGET).md
DEVKITPRO ?= /opt/devkitpro
ifeq ($(TARGET),3ds)
# The architecture flags pick devkitARM's hard-float libraries and the 3DS
# startup code (3dsx_crt0.o), as the smoke app's link does.
LINK_CHECK_LDFLAGS := $(filter -march=% -mtune=% -mfloat-abi=% -mfpu=% -mtp=%,$(PORT_CFLAGS)) \
                      -specs=3dsx.specs -L$(DEVKITPRO)/libctru/lib
LINK_CHECK_LIBS := -lctru
endif

$(LINK_CHECK_OBJ): link/link_check.c
	@mkdir -p $(dir $@)
	@$(CC) $(PORT_CFLAGS) -c $< -o $@

link-check: $(LINK_CHECK_OBJ) $(LIB) $(LIBNNS) $(LIBNITRO) $(OVERLAY_IDS)
	@mkdir -p $(dir $(LINK_REPORT))
	@$(CC) $(LDFLAGS) $(LINK_CHECK_LDFLAGS) -Wl,--gc-sections $(LINK_CHECK_OBJ) \
	    -Wl,--start-group $(LIB) $(LIBNNS) $(LIBNITRO) -Wl,--end-group $(LINK_CHECK_LIBS) -lm \
	    $(OVERLAY_IDS) -o $(OUT)/link/platinum.elf > $(OUT)/link/linker.txt 2>&1 || true
	@python3 $(ROOT)/tools/game_link_report.py $(OUT)/link/linker.txt --target $(TARGET) --output $(LINK_REPORT)
	@echo "Report: $(LINK_REPORT)"

# The diagnostic survey exercises original game C files *without linking*.
# This intentionally does not force every game source into libplatinum_game:
# errors are reported as blockers, not masked by blanket compiler stubs.
# Set PROBE_LIMIT=0 to check every file; use a bounded sample in CI.
PROBE_LIMIT ?= 64
PROBE_TIMEOUT ?= 10
PROBE_REPORT := $(ROOT)/build/game-compile-survey/$(TARGET).json

GAME_PROBE_CFLAGS := $(GAME_CFLAGS)

probe: $(GAME_GENERATED) | libnitrosystem
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
