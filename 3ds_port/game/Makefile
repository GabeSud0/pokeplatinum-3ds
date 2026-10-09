# Incremental GCC build of the original Pokémon Platinum game sources.
# Always fetch the pinned decompilation first: python tools/bootstrap.py --clean
#
# make                 Compile and smoke-test the first game-code slice on PC
# make TARGET=host32   Repeat with 32-bit pointers, as on the 3DS
# make TARGET=3ds      Cross-compile the same game code for devkitARM
#
# This is a relocatable object library, NOT a playable game executable.
# Expand GAME_SRCS after each subsystem compiles cleanly and is covered by CI.

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
             map_tile_behavior.c field_script_context.c
GAME_OBJS := $(GAME_SRCS:%.c=$(OUT)/src/%.o)

# metang normally creates this header during the DS Meson build. Generate it
# from the *pinned* manifest so GCC sees real, stable text-bank indices.
TEXT_BANK_MANIFEST := $(GAME)/generated/text_banks.txt
TEXT_BANK_HEADER := $(OUT)/gen/generated/text_banks.h

$(TEXT_BANK_HEADER): $(TEXT_BANK_MANIFEST) $(ROOT)/tools/generate_game_text_banks.py
	@python3 $(ROOT)/tools/generate_game_text_banks.py $< $@


# Force the original global precompiled header, as Meson/Metrowerks does.
# _MSL_RESTRICT is a Metrowerks qualifier and must be empty with GCC.
GAME_CFLAGS := $(PORT_CFLAGS) $(SDK_INCLUDES) $(NNS_INCLUDES) \
               -I$(GAME)/include -I$(GAME)/asm -I$(GAME) -I$(OUT)/gen \
               -DPOKEPLATINUM_REVISION=1 -DPOKEPLATINUM_GENERATED_ENUM \
               -D_MSL_RESTRICT= -DPM_KEEP_ASSERTS \
               -DGAME_VERSION=VERSION_PLATINUM \
               -DGAME_LANGUAGE=LANGUAGE_ENGLISH \
               -include $(GAME)/include/pch/global_pch.h \
               -ffunction-sections -fdata-sections

.PHONY: all lib test clean libnitro libnitrosystem

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
$(OUT)/src/%.o: $(GAME)/src/%.c $(NNS_INLINE_SYMS) $(TEXT_BANK_HEADER) | libnitrosystem
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

$(OUT)/tests/test_game.o: tests/test_game.c $(NNS_INLINE_SYMS) $(TEXT_BANK_HEADER) | libnitrosystem
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

clean:
	rm -rf build
