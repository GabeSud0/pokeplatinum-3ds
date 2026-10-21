# GCC build of the original Pokémon Platinum game sources, and the programs
# that run them. Fetch the pinned decompilation first: python tools/bootstrap.py
#
# make                         compile the game for the PC and run its tests
# make TARGET=host32           the same with 32-bit pointers, as on the 3DS
# make TARGET=3ds              compile the game for the 3DS (devkitARM)
# make TARGET=3ds program      build/3ds/pokeplatinum.3dsx: the game on a 3DS
# make TARGET=host32 program   build/host32/pokeplatinum-pc: the game on a PC,
#                              without a screen (for finding where it stops)
# make TARGET=... link-check   link the program, reporting what's missing
#
# The programs need the player's data pack to run (README.md). Builds that
# contain game code are for your own use: never share them.

.DEFAULT_GOAL := all
comma := ,

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
# The sources the original build compiles (src/meson.build's lists).
GAME_ALL_SRCS := $(shell python3 $(ROOT)/tools/game_sources.py $(GAME))
GAME_SRCS := $(filter-out $(GAME_BLOCKED),$(GAME_ALL_SRCS))
GAME_OBJS := $(GAME_SRCS:%.c=$(OUT)/src/%.o)

# The game's own libraries that the original links into the same program
# (meson.build: libcrypto, libgds, libspl). libdwcppwlobby/libdwcenc are
# left out with the rest of online play.
GAME_LIBS := crypto gds spl
GAME_LIB_SRCS := $(sort $(foreach l,$(GAME_LIBS),$(wildcard $(GAME)/lib/$(l)/src/*.c)))
GAME_OBJS += $(GAME_LIB_SRCS:$(GAME)/%.c=$(OUT)/%.o)

# The trainer AI script: data written with assembler macros, which the
# original assembles with GNU as (meson.build: trainer_ai_o). It is only
# data, so any target's assembler builds it the same way.
TRAINER_AI_O := $(OUT)/src/battle/trainer_ai/script.o
GAME_OBJS += $(TRAINER_AI_O)

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

# Headers the decompilation's own data tools write (tools/dataproc:
# speciesproc, itemproc, frontierproc, trainerproc, resdatproc), built with
# the PC's C compiler as their meson.build files say, and the two images the
# game embeds (tools/png_embed.py, nitrogfx -embed's conversion in Python).
DATAPROC_DIR := $(ROOT)/build/game-tools
DATAPROC_STAMP := $(RESOURCE_DIR)/.dataproc-ready
DATAPROC_INPUTS := $(shell find $(GAME)/tools/dataproc $(GAME)/tools/enumproc \
        $(GAME)/tools/nitroarc/lib -type f) \
    $(shell find $(GAME)/res/pokemon $(GAME)/res/items $(GAME)/res/trainers \
        $(GAME)/res/graphics/sprite_templates -name '*.json') \
    $(GAME)/res/fonts/arrow_cursor.png $(GAME)/res/graphics/battle/healthbox/healthbox_parts.png

$(DATAPROC_STAMP): $(DATAPROC_INPUTS) $(CONSTANTS_STAMP) $(ARCHIVE_STAMP) $(ORDERGEN_STAMP) \
                   $(ROOT)/tools/prepare_game_dataproc.py $(ROOT)/tools/png_embed.py
	@python3 $(ROOT)/tools/prepare_game_dataproc.py --upstream "$(GAME)" --constants "$(CONSTANTS_DIR)" \
	    --resources "$(RESOURCE_DIR)" --work "$(DATAPROC_DIR)"
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

# Meson adds the folder of every generated header it compiles with to the
# include path, so the game can include some by bare name ("ending.naix").
# Same here: every res/ folder whose meson.build generates a .naix or .h.
GENERATED_HEADER_DIRS := $(addprefix -I$(RESOURCE_DIR)/,$(sort $(patsubst $(GAME)/%/,%,$(dir \
    $(shell grep -rlE "\.(naix|h)'" --include=meson.build $(GAME)/res)))))

# Overlay IDs (FS_OVERLAY_ID): each overlay's position in the linker spec,
# as constants in a header every game file is compiled with, and as the
# symbols Nintendo's linker made (an ld script). See the tool.
OVERLAY_IDS   := $(ROOT)/build/game-link/overlay_ids.ld
OVERLAY_IDS_H := $(ROOT)/build/game-link/overlay_ids.h

$(OVERLAY_IDS) $(OVERLAY_IDS_H) &: $(GAME)/platinum.us/main.lsf $(ROOT)/tools/prepare_game_overlay_ids.py
	@python3 $(ROOT)/tools/prepare_game_overlay_ids.py --lsf $< --output $(OVERLAY_IDS) --header $(OVERLAY_IDS_H)
	@touch $(OVERLAY_IDS) $(OVERLAY_IDS_H)

# Every generated header the game's sources may include.
GAME_GENERATED := $(CONSTANTS_STAMP) $(RESOURCE_HEADER) $(TEXT_BANK_STAMP) $(MOVE_TEXT_STAMP) \
                  $(SPECIES_TEXT_STAMP) $(SOUND_HEADER) $(ARCHIVE_STAMP) $(ORDERGEN_STAMP) \
                  $(DATAPROC_STAMP) $(NETWORK_STAMP) $(OVERLAY_IDS_H)

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
               -I$(CONSTANTS_DIR)/generated $(GENERATED_HEADER_DIRS) \
               -DPOKEPLATINUM_REVISION=1 -DPOKEPLATINUM_GENERATED_ENUM \
               -D_MSL_RESTRICT= -DPM_KEEP_ASSERTS \
               -DGAME_VERSION=VERSION_PLATINUM \
               -DGAME_LANGUAGE=LANGUAGE_ENGLISH \
               -D_NITRO -DLINK_PPWLOBBY \
               -include $(GAME)/include/pch/global_pch.h -include $(OVERLAY_IDS_H) \
               -ffunction-sections -fdata-sections \
               $(GCC14_ERRORS)


.PHONY: all lib test probe program cia link-check clean libnitro libnitrosystem FORCE

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

# The same pass gives each overlay's objects their own data sections, for
# resetting an overlay's data when it's loaded (tools/game_overlays.py,
# nitro/src/port_overlay.c), and writes the overlay table programs link.
# Both change the objects in place, so they share this one step.
OVERLAY_TABLE_C := $(OUT)/gen/overlay_table.c

$(GAME_HEADER_DEFS): $(CENSUS_OBJS) $(GAME)/platinum.us/main.lsf $(ROOT)/tools/game_overlays.py
	@mkdir -p $(dir $@)
	@# Objects already made weak by an earlier run no longer show up as
	@# duplicates, so keep the names found before.
	$(file >$(OUT)/census_objs.rsp,$(CENSUS_OBJS))@{ cat $@ 2>/dev/null; $(NM) -g --defined-only @$(OUT)/census_objs.rsp | awk 'NF == 3 && $$2 ~ /^[TDBR]$$/ { print $$3 }' | sort | uniq -d; } | sort -u > $@.new
	@mv $@.new $@
	@for o in $(CENSUS_OBJS); do $(OBJCOPY) --weaken-symbols=$@ $$o; done
	@echo "  $$(wc -l < $@) game header functions defined in several objects, made weak"
	@python3 $(ROOT)/tools/game_overlays.py --lsf $(GAME)/platinum.us/main.lsf --upstream $(GAME) \
	    --objdir $(OUT) --objcopy $(OBJCOPY) --output $(OVERLAY_TABLE_C)
	@touch $@

$(OVERLAY_TABLE_C): $(GAME_HEADER_DEFS)
	@test -f $@

$(TRAINER_AI_O): $(GAME)/src/battle/trainer_ai/script.s $(GAME_GENERATED)
	@mkdir -p $(dir $@)
	@echo "  GAME AS battle/trainer_ai/script.s"
	@$(CC) $(filter -m%,$(PORT_CFLAGS)) $(if $(IS_HOST),-Wa$(comma)--noexecstack) \
	    -I$(GAME)/include -I$(GAME)/asm -I$(CONSTANTS_DIR) -x assembler-with-cpp -c $< -o $@

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
	    --headers $(NETWORK_DIR) $(GAME)/lib/ppwlobby $(SDK)/include/nitro \
	    --sources $(NETWORK_DIR)/NitroDWC $(NETWORK_DIR)/NitroWiFi $(GAME)/lib/ppwlobby -- $(GAME_OBJS)
	@touch $@

$(WIFI_STUBS_O): $(WIFI_STUBS_C)
	@$(CC) $(PORT_CFLAGS) $(SDK_INCLUDES) -c $< -o $@

# The ~1,100 object files are passed to ar and the linker in a response file
# (@file): listed on the command line they'd pass Windows' 32 KB limit on a
# command line (building in MSYS2). Relative paths, which every native
# Windows tool reads as they are.
GAME_OBJS_RSP := $(OUT)/game_objs.rsp
write_game_objs_rsp = $(file >$(GAME_OBJS_RSP),$(GAME_OBJS) $(WIFI_STUBS_O))

$(LIB): $(GAME_HEADER_DEFS) $(WIFI_STUBS_O)
	@rm -f $@
	$(write_game_objs_rsp)@$(AR) rcs $@ @$(GAME_OBJS_RSP)
	@echo "built $@ ($(words $(GAME_SRCS)) of $(words $(GAME_ALL_SRCS)) game C files, and lib/$(subst $(eval) ,+,$(GAME_LIBS)))"

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

# --- The programs -----------------------------------------------------------
# The game itself: the original NitroMain with every game file, the overlay
# table, the port's start (app/platinum.c) and a main for the target:
#   TARGET=3ds          app/main_3ds.c -> build/3ds/pokeplatinum.3dsx (.elf)
#   TARGET=host/host32  app/main_pc.c  -> build/<target>/pokeplatinum-pc,
#                       which runs the game on a PC without a screen
# The game objects go in themselves (not through the library), so every
# overlay's data sections are there for the overlay table. Programs built
# from the game are for your own use: never share them (CONTRIBUTING.md).
DEVKITPRO ?= /opt/devkitpro
APP_OBJS := $(OUT)/app/platinum.o
# Linked after the game's objects: see tools/game_overlays.py (render_table).
OVERLAY_TABLE_O := $(OUT)/gen/overlay_table.o

ifeq ($(TARGET),3ds)
APP_OBJS += $(OUT)/app/main_3ds.o $(OUT)/app/port_gx3d_pica.o $(OUT)/app/port_compose_pica.o \
            $(OUT)/app/port_pica_abi.o $(OUT)/app/gx3d_passthrough_shbin.o
PROGRAM_ELF := $(OUT)/pokeplatinum.elf
PROGRAM := $(OUT)/pokeplatinum.3dsx
# The architecture flags pick devkitARM's hard-float libraries and the 3DS
# startup code (3dsx_crt0.o).
PROGRAM_LDFLAGS := $(filter -march=% -mtune=% -mfloat-abi=% -mfpu=% -mtp=%,$(PORT_CFLAGS)) \
                   -specs=3dsx.specs -L$(DEVKITPRO)/libctru/lib -Wl,-Map,$(OUT)/pokeplatinum.map
PROGRAM_LIBS := -lcitro3d -lctru -lm
else
APP_OBJS += $(OUT)/app/main_pc.o
PROGRAM_ELF := $(OUT)/pokeplatinum-pc
PROGRAM := $(PROGRAM_ELF)
# -rdynamic: function names in the crash backtrace.
PROGRAM_LDFLAGS := $(LDFLAGS) -rdynamic
PROGRAM_LIBS := -lm
endif

# The wide art built into the program (the repository's wideart/: the
# pictures without the game's art, docs/WIDE_ART.md), as C, for both
# programs. tools/embed_wideart.py runs as make reads this file (quick) and
# writes the C again only when the pictures changed (added, changed or
# taken out), so the object is made again only then.
WIDE_ART_FILES := $(sort $(wildcard $(ROOT)/wideart/*.png))
WIDE_ART_C := $(OUT)/gen/wideart_builtin.c
WIDE_ART_O := $(OUT)/gen/wideart_builtin.o
APP_OBJS += $(WIDE_ART_O)
$(shell mkdir -p $(OUT)/gen && python3 $(ROOT)/tools/embed_wideart.py --output $(WIDE_ART_C) $(WIDE_ART_FILES) >/dev/null)

$(WIDE_ART_O): $(WIDE_ART_C) $(NITRO)/include/nitro_port_wideart.h
	@echo "  APP CC wideart_builtin.c"
	@$(CC) $(PORT_CFLAGS) -I$(NITRO)/include -c $< -o $@

PROGRAM_LINK = $(write_game_objs_rsp)$(CC) $(PROGRAM_LDFLAGS) -Wl,--gc-sections -Wl,--wrap=ErrorHandling_AssertFail \
    $(APP_OBJS) @$(GAME_OBJS_RSP) $(OVERLAY_TABLE_O) \
    -Wl,--start-group $(LIBNNS) $(LIBNITRO) -Wl,--end-group $(PROGRAM_LIBS) $(OVERLAY_IDS) -o $(PROGRAM_ELF)

$(OUT)/app/platinum.o: app/platinum.c app/platinum.h | libnitrosystem
	@mkdir -p $(dir $@)
	@echo "  APP CC platinum.c"
	@$(CC) $(PORT_CFLAGS) $(SDK_INCLUDES) -I$(ROOT)/3ds_port/pak/include -I$(GAME)/include -c $< -o $@

$(OUT)/app/main_pc.o: app/main_pc.c app/platinum.h | libnitrosystem
	@mkdir -p $(dir $@)
	@echo "  APP CC main_pc.c"
	@$(CC) $(PORT_CFLAGS) $(SDK_INCLUDES) -c $< -o $@

# main_3ds.c uses libctru, and none of the SDK's headers (the port's
# portable geometry header only).
$(OUT)/app/main_3ds.o: app/main_3ds.c app/platinum.h app/port_pica.h $(NITRO)/include/nitro_port_gx3d.h \
                       $(NITRO)/include/nitro_port_render2d.h
	@mkdir -p $(dir $@)
	@echo "  APP CC main_3ds.c"
	@$(CC) $(PORT_CFLAGS) -I$(DEVKITPRO)/libctru/include -I$(NITRO)/include -MMD -MP -c $< -o $@

# Thin 3DS-only PICA backend; includes only portable GX3D headers + libctru.
$(OUT)/app/port_gx3d_pica.o: app/port_gx3d_pica.c app/port_pica.h $(NITRO)/include/nitro_port_gx3d_vertex.h
	@mkdir -p $(dir $@)
	@echo "  APP CC port_gx3d_pica.c"
	@$(CC) $(PORT_CFLAGS) -I$(DEVKITPRO)/libctru/include -I$(NITRO)/include -MMD -MP -c $< -o $@

# libctru's structs as libctru lays them out: without -fno-short-enums, as
# devkitARM's libraries are built (port_pica_abi.c).
$(OUT)/app/port_pica_abi.o: app/port_pica_abi.c
	@mkdir -p $(dir $@)
	@echo "  APP CC port_pica_abi.c"
	@$(CC) $(filter-out -fno-short-enums,$(PORT_CFLAGS)) -I$(DEVKITPRO)/libctru/include -MMD -MP -c $< -o $@

# (the 3DS program's own files rebuilt when a header they include changes)
-include $(wildcard $(OUT)/app/*.d)

# The screens put together on the GPU (the remaster display).
$(OUT)/app/port_compose_pica.o: app/port_compose_pica.c app/port_pica.h $(NITRO)/include/nitro_port_upscale.h
	@mkdir -p $(dir $@)
	@echo "  APP CC port_compose_pica.c"
	@$(CC) $(PORT_CFLAGS) -I$(DEVKITPRO)/libctru/include -I$(NITRO)/include -MMD -MP -c $< -o $@

# Assemble the PICA200 vertex programs like hello3d, into one blob (one
# DVLE each, in this order: the polygons, the sun shadows' map, the sun
# shadows' scene). The generated blob symbol is gx3d_passthrough_shbin,
# consumed by port_gx3d_pica.c.
GX3D_SHADERS := app/gx3d_passthrough.v.pica app/gx3d_shadow_cast.v.pica app/gx3d_shadow_recv.v.pica
$(OUT)/app/gx3d_passthrough.shbin: $(GX3D_SHADERS)
	@mkdir -p $(dir $@)
	picasso -o $@ $(GX3D_SHADERS)

$(OUT)/app/gx3d_passthrough_shbin.o: $(OUT)/app/gx3d_passthrough.shbin
	@cd $(OUT)/app && bin2s gx3d_passthrough.shbin | $(DEVKITARM)/bin/arm-none-eabi-as -o gx3d_passthrough_shbin.o

$(OVERLAY_TABLE_O): $(OVERLAY_TABLE_C) | libnitrosystem
	@$(CC) $(PORT_CFLAGS) $(SDK_INCLUDES) -c $< -o $@

$(PROGRAM_ELF): $(APP_OBJS) $(OVERLAY_TABLE_O) $(GAME_HEADER_DEFS) $(WIFI_STUBS_O) $(LIBNNS) $(LIBNITRO) $(OVERLAY_IDS)
	@echo "  LINK $(notdir $@)"
	@$(PROGRAM_LINK)

ifeq ($(TARGET),3ds)
# The HOME Menu's icon: the port's own (cia/icon.png, 48x48, original art),
# else the one made from your ROM's icon (tools/home_menu_art.py: made at
# build time, never committed), else libctru's.
HOME_ART ?=
ifneq ($(wildcard cia/icon.png),)
ICON := cia/icon.png
else ifneq ($(wildcard $(HOME_ART)/icon.png),)
ICON := $(HOME_ART)/icon.png
else
ICON := $(DEVKITPRO)/libctru/default_icon.png
endif

# (made every time: quick, and the icon may have come or gone)
$(OUT)/pokeplatinum.smdh: FORCE
	@mkdir -p $(dir $@)
	@$(DEVKITPRO)/tools/bin/smdhtool --create "Pokemon Platinum" "3DS port (needs your data pack)" \
	    "pokeplatinum-3ds" "$(ICON)" $@

$(PROGRAM): $(PROGRAM_ELF) $(OUT)/pokeplatinum.smdh
	@$(DEVKITPRO)/tools/bin/3dsxtool $< $@ --smdh=$(OUT)/pokeplatinum.smdh
	@echo "built $@ (contains game code: for your own use, never share it)"

# The HOME Menu version: the same program installed as a title of its own
# (opens and closes from the HOME Menu). Needs makerom and bannertool
# (tools/build_cia_tools.sh: CIA_TOOLS is where it put them) and the art.
CIA_TOOLS ?= $(ROOT)/build/cia-tools
MAKEROM ?= $(CIA_TOOLS)/bin/makerom
BANNERTOOL ?= $(CIA_TOOLS)/bin/bannertool
PROGRAM_CIA := $(OUT)/pokeplatinum.cia

# The banner's tune: the port's own chime (cia/chime.wav, original music,
# under the HOME Menu's 3-second limit), or HOME_ART's moment of silence
# without it. CHIME=path/to/other.wav chooses another one.
CHIME ?= $(wildcard cia/chime.wav)
BANNER_WAV := $(if $(CHIME),$(CHIME),$(HOME_ART)/banner.wav)

$(OUT)/pokeplatinum.bnr: $(HOME_ART)/banner.png $(BANNER_WAV)
	@test -n "$(HOME_ART)" || { echo "Set HOME_ART (tools/home_menu_art.py)"; exit 1; }
	@$(BANNERTOOL) makebanner -i $(HOME_ART)/banner.png -a $(BANNER_WAV) -o $@ >/dev/null

$(PROGRAM_CIA): $(PROGRAM_ELF) $(OUT)/pokeplatinum.smdh $(OUT)/pokeplatinum.bnr cia/pokeplatinum.rsf
	@$(MAKEROM) -f cia -o $@ -target t -exefslogo -rsf cia/pokeplatinum.rsf -elf $(PROGRAM_ELF) \
	    -icon $(OUT)/pokeplatinum.smdh -banner $(OUT)/pokeplatinum.bnr
	@echo "built $@ (contains game code: for your own use, never share it)"

cia: $(PROGRAM_CIA)
endif

program: $(PROGRAM)

FORCE:

# The same link, reporting every symbol still missing instead of failing
# (tools/game_link_report.py): a to-do list while the port is incomplete.
LINK_REPORT := $(ROOT)/build/game-link-report/$(TARGET).md

link-check: $(APP_OBJS) $(OVERLAY_TABLE_O) $(GAME_HEADER_DEFS) $(WIFI_STUBS_O) $(LIBNNS) $(LIBNITRO) $(OVERLAY_IDS)
	@mkdir -p $(dir $(LINK_REPORT)) $(OUT)/link
	@$(PROGRAM_LINK) > $(OUT)/link/linker.txt 2>&1 || true
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
