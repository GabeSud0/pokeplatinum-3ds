# ---------------------------------------------------------------------------
# libnitro: Nintendo's NitroSDK, compiled with GCC for the 3DS (and for PC,
# so it can be unit tested).
#
#   make                 build for PC and run the tests     (TARGET=host)
#   make TARGET=host32   same, as a 32-bit program (needs gcc-multilib)
#   make TARGET=3ds      build the library for the 3DS's ARM11 (needs devkitARM)
#   make clean
#
# The SDK sources come from build/NitroSDK (run tools/bootstrap.py first).
# Our changes to them are in patches/NitroSDK/; code that replaces parts of
# the SDK entirely is in src/ here.
# ---------------------------------------------------------------------------

include ../common.mk

OUT      := build/$(TARGET)
GEN      := $(OUT)/gen

# --- Sources ---------------------------------------------------------------

# NitroSDK files compiled as they are (with our patches applied): every
# ARM9 source of the libraries the game uses.
SDK_SRCS := \
	card/src/card_common.c \
	card/src/card_spi.c \
	card/src/card_backup.c \
	card/src/card_rom.c \
	card/src/card_request.c \
	card/src/card_pullOut.c \
	cp/src/cp_context.c \
	fs/src/fs_command.c \
	fs/src/fs_command_default.c \
	fs/src/fs_archive.c \
	fs/src/fs_file.c \
	fs/src/fs_rom.c \
	fs/src/fs_mem.c \
	fs/src/fs_overlay.c \
	fx/src/fx_sincos.c \
	fx/src/fx_mtx22.c \
	fx/src/fx_mtx33.c \
	fx/src/fx_mtx43.c \
	fx/src/fx_mtx44.c \
	fx/src/fx_cp.c \
	fx/src/fx_vec.c \
	fx/src/fx_trig.c \
	fx/src/fx_atan.c \
	fx/src/fx_atanidx.c \
	fx/src/fx.c \
	fx/src/fx_asinacosidx.c \
	gx/src/gx.c \
	gx/src/gxstate.c \
	gx/src/gx_vramcnt.c \
	gx/src/gx_bgcnt.c \
	gx/src/g2.c \
	gx/src/g3b.c \
	gx/src/g3c.c \
	gx/src/g3imm.c \
	gx/src/g3x.c \
	gx/src/g3_util.c \
	gx/src/gx_load2d.c \
	gx/src/gx_load3d.c \
	gx/src/g3.c \
	gx/src/gxasm.c \
	math/src/math.c \
	math/src/dgt.c \
	math/src/crc.c \
	math/src/checksum.c \
	math/src/fft.c \
	mi/src/mi_wram.c \
	mi/src/mi_dma.c \
	mi/src/mi_dma_hblank.c \
	mi/src/mi_dma_vblank.c \
	mi/src/mi_dma_mainmem.c \
	mi/src/mi_dma_gxcommand.c \
	mi/src/mi_byteAccess.c \
	mi/src/mi_stream.c \
	mi/src/mi_card.c \
	mi/src/mi_dma_card.c \
	mi/src/mi_compress.c \
	mi/src/mi_exMemory.c \
	mi/src/mi_init.c \
	mi/src/mi_cache.c \
	os/src/os_irqHandler.c \
	os/src/os_irqTable.c \
	os/src/os_interrupt.c \
	os/src/os_spinLock.c \
	os/src/os_printf.c \
	os/src/os_thread.c \
	os/src/os_context.c \
	os/src/os_emulator.c \
	os/src/os_message.c \
	os/src/os_mutex.c \
	os/src/os_init.c \
	os/src/os_arena.c \
	os/src/os_alloc.c \
	os/src/os_timer.c \
	os/src/os_tick.c \
	os/src/os_alarm.c \
	os/src/os_valarm.c \
	os/src/os_system.c \
	os/src/os_profile.c \
	os/src/os_ownerInfo.c \
	os/src/os_vramExclusive.c \
	os/src/os_argument.c \
	os/src/os_entropy.c \
	os/src/os_terminate_proc.c \
	rtc/src/external.c \
	rtc/src/internal.c \
	rtc/src/convert.c \
	snd/src/snd_interface.c \
	snd/src/snd_main.c \
	snd/src/snd_command.c \
	snd/src/snd_alarm.c \
	snd/src/snd_work.c \
	snd/src/snd_util.c \
	snd/src/snd_bank.c \
	snd/src/snd_data.c \
	spi/src/tp.c \
	spi/src/mic.c \
	spi/src/pm.c \
	std/src/std_string.c \
	std/src/std_sprintf.c \
	std/src/std_unicode.c \
	wbt/src/wbt_context.c \
	wbt/src/wbt_api.c \
	wfs/src/wfs_format.c \
	wfs/src/wfs_server.c \
	wfs/src/wfs_client.c \
	wfs/src/wfs_archive.c \
	wfs/src/wfs_thread.c \
	utest/src/utest_assert.c \
	utest/src/utest_cui.c \
	utest/src/utest_main.c \
	wm/src/wm_system.c \
	wm/src/wm_sync.c \
	wm/src/wm_standard.c \
	wm/src/wm_mp.c \
	wm/src/wm_dcf.c \
	wm/src/wm_ds.c \
	wm/src/wm_ks.c \
	wm/src/wm_etc.c

# Not built, because the game doesn't use them: mb (download play),
# ctrdg (GBA slot, answered by src/ctrdg.c instead), prc (gesture recognition),
# wvr, env, ext, os/src/os_china.c (China-edition boot screen) and
# math/src/qsort.c. pad and exi are ARM7-only. utest is built because the
# SDK's own self-tests (in os_thread.c) refer to it.
#
# Replaced by our own C in src/:
#   mi/src/mi_memory.c, mi_swap.c, mi_uncompress.c  (Metrowerks assembly only)
#   os/src/os_cache.c, os_tcm.c, os_protection*.c   (DS CPU setup: nothing to do) -> port_os.c
#   os/src/os_exception.c, os_reset.c               (mostly assembly)            -> port_os.c
#   pxi/src/pxi_fifo.c, pxi_init.c                  (the ARM7 link)              -> pxi.c
#   dgt/prebuilt/libdgt.a                           (MD5/SHA-1, a binary)        -> dgt.c
#   the divider/square-root hardware                                             -> port_cp.c
# Not built: mi/src/mi_uncomp_stream.c (streaming decompression; assembly only,
# and the game doesn't use it).

# Compile Nintendo's original ARM7 sequencer as a second, isolated SDK
# instance. Both CPUs expose the same SND_* names, so link the ARM7 objects
# together and rename ONLY symbols they define, not their external OS/libc
# dependencies. The resulting driver is not started until the audio bridge
# implements independent scheduling and register emulation.
ARM7_SND_SRCS := \
    snd/src/ARM7/snd_capture.c \
    snd/src/ARM7/snd_channel.c \
    snd/src/ARM7/snd_exchannel.c \
    snd/src/ARM7/snd_global.c \
    snd/src/ARM7/snd_midiplayer.c \
    snd/src/ARM7/snd_seq.c \
    snd/src/snd_alarm.c \
    snd/src/snd_bank.c \
    snd/src/snd_command.c \
    snd/src/snd_data.c \
    snd/src/snd_main.c \
    snd/src/snd_util.c \
    snd/src/snd_work.c
ARM7_SND_OBJS := $(ARM7_SND_SRCS:%.c=$(OUT)/arm7/sdk/%.o)
ARM7_SND_RAW := $(OUT)/arm7/sound_driver.raw.o
ARM7_SND_MAP := $(OUT)/arm7/sound_driver.map
ARM7_SND_PREFIXED := $(OUT)/arm7/sound_driver.prefixed.o

PORT_SRCS := $(wildcard src/*.c)

# Threads, signals and the clock come from the machine (src/port_platform.h).
ifeq ($(TARGET),3ds)
PORT_SRCS += src/platform/3ds.c
else
PORT_SRCS += src/platform/host.c
endif

SDK_OBJS  := $(SDK_SRCS:%.c=$(OUT)/sdk/%.o)
PORT_OBJS := $(PORT_SRCS:src/%.c=$(OUT)/port/%.o)
# Embed the *same* portable libpak implementation in libnitro.a. NitroSDK
# callers already link libnitro, so we don't force a new linker dependency on
# every native game/smoke-test target while still using the C libpak API.
PAK_READER_OBJ := $(OUT)/port/pak_reader.o
PORT_OBJS += $(PAK_READER_OBJ)
LIB       := $(OUT)/libnitro.a

# --- Flags -----------------------------------------------------------------

# Compiler, target and SDK configuration: see ../common.mk.
CFLAGS := $(PORT_CFLAGS) $(SDK_INCLUDES) -I../pak/include

# --- Rules -----------------------------------------------------------------

.PHONY: all lib test clean

ifeq ($(IS_HOST),yes)
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
	@echo "  CC  $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/port/%.o: src/%.c $(GEN)/nitro/fx/fx_const.h
	@mkdir -p $(dir $@)
	@echo "  CC  $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

# Build exactly the ARM7 branches of NitroSDK's sound driver. CFLAGS
# contains -DSDK_ARM9 by default; explicitly undefine it after those flags.
$(OUT)/arm7/sdk/%.o: $(SDK)/libraries/%.c $(GEN)/nitro/fx/fx_const.h
	@mkdir -p $(dir $@)
	@echo "  CC  ARM7 $(notdir $<)"
	@$(CC) $(CFLAGS) -USDK_ARM9 -DSDK_ARM7 -include $(SDK)/include/nitro/misc.h -c $< -o $@

$(ARM7_SND_RAW): $(ARM7_SND_OBJS)
	@mkdir -p $(dir $@)
	@$(CC) $(PORT_CFLAGS) -r -nostdlib $^ -o $@

$(ARM7_SND_MAP): $(ARM7_SND_RAW)
	@$(NM) -g --defined-only $< | python3 -c 'import sys; [print(v[-1], "Arm7_" + v[-1]) for line in sys.stdin for v in [line.split()] if len(v) == 3]' > $@

$(ARM7_SND_PREFIXED): $(ARM7_SND_RAW) $(ARM7_SND_MAP)
	@cp $(ARM7_SND_RAW) $@
	@$(OBJCOPY) --redefine-syms=$(ARM7_SND_MAP) $@

# The 3DS platform layer uses libctru (and none of the SDK's headers).
DEVKITPRO ?= /opt/devkitpro
$(OUT)/port/platform/3ds.o: src/platform/3ds.c src/port_platform.h
	@mkdir -p $(dir $@)
	@echo "  CC  platform/3ds.c"
	@$(CC) $(PORT_CFLAGS) -I$(DEVKITPRO)/libctru/include -c $< -o $@

$(PAK_READER_OBJ): ../pak/src/pak.c ../pak/include/pak.h
	@mkdir -p $(dir $@)
	@echo "  CC  libpak reader"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(SDK_OBJS) $(PORT_OBJS) $(ARM7_SND_PREFIXED)
	@rm -f $@
	@$(AR) rcs $@ $^
	@echo "built $@"

# Unit tests run on the PC only.
TEST_BIN := $(OUT)/test_nitro

$(TEST_BIN): tests/test_nitro.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIB) -lm -o $@

# An independent FS_* integration test uses only synthetic bytes produced by
# the project's real Python pack writer (no ROM contents are checked in).
FS_TEST_BIN := $(OUT)/test_fs_pak
FS_FIXTURE_DIR := $(OUT)/fs_fixture
FS_FIXTURE_STAMP := $(FS_FIXTURE_DIR)/.generated

$(FS_TEST_BIN): tests/test_fs_pak.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIB) -lm -o $@

$(FS_FIXTURE_STAMP): tests/make_fs_fixture.py ../../builder/pack.py
	@mkdir -p $(FS_FIXTURE_DIR)
	python3 tests/make_fs_fixture.py $(FS_FIXTURE_DIR)
	@touch $@

# Threads, interrupts with threads, timers: a program of their own, since
# once the SDK's threads start the program's thread is theirs for good.
THREAD_TEST_BIN := $(OUT)/test_threads

$(THREAD_TEST_BIN): tests/test_threads.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIB) -lm -o $@

# Call the actual ARM9 CARD/RTC/PM/TP APIs against virtual ARM7 handlers.
# The integration executable creates its own temporary .sav and removes it.
ARM7_TEST_BIN := $(OUT)/test_arm7

$(ARM7_TEST_BIN): tests/test_arm7.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIB) -lm -o $@

# Synthetic-only PC test for the portable sound-chip engine.
# This is independent from the ARM9/ARM7 command-protocol integration.
SOUND_TEST_BIN := $(OUT)/test_sound_chip

$(SOUND_TEST_BIN): tests/test_sound_chip.c src/sound_chip.h $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIB) -lm -o $@

test: $(TEST_BIN) $(FS_TEST_BIN) $(FS_FIXTURE_STAMP) $(THREAD_TEST_BIN) $(ARM7_TEST_BIN) $(SOUND_TEST_BIN)
	./$(TEST_BIN)
	./$(FS_TEST_BIN) $(FS_FIXTURE_DIR)
	./$(THREAD_TEST_BIN)
	./$(ARM7_TEST_BIN)
	./$(SOUND_TEST_BIN)

clean:
	rm -rf build
