# ---------------------------------------------------------------------------
# libnitrosystem: Nintendo's NitroSystem (3D models, 2D sprites, VRAM
# managers, the sound archive player, heaps), compiled with GCC for the 3DS,
# and for PC so it can be unit tested. It sits on top of libnitro.
#
#   make                 build for PC and run the tests     (TARGET=host)
#   make TARGET=host32   same, as a 32-bit program (needs gcc-multilib)
#   make TARGET=3ds      build the library for the 3DS's ARM11 (needs devkitARM)
#   make clean
#
# The sources come from build/NitroSystem (run tools/bootstrap.py first).
# Our changes to them are in patches/NitroSystem/.
# ---------------------------------------------------------------------------

include ../common.mk

OUT := build/$(TARGET)

ifeq ($(wildcard $(NNS)/include/nnsys.h),)
$(error NitroSystem not found at $(NNS). Run: python tools/bootstrap.py)
endif

# --- Sources ---------------------------------------------------------------

# Every NitroSystem library the game uses, compiled as it is (with our patch
# applied). The lists match NitroSystem's own meson.build files.

FND_SRCS := list.c heapcommon.c expheap.c frameheap.c unitheap.c archive.c \
            allocator.c

G2D_SRCS := \
	load/g2d_NAN_load.c load/g2d_NOB_load.c load/g2d_NMC_load.c \
	load/g2d_NEN_load.c load/g2d_NCG_load.c load/g2d_NCL_load.c \
	load/g2d_NSC_load.c load/g2d_Load.c load/g2d_NFT_load.c \
	g2d_Softsprite.c g2d_Animation.c g2d_OAM.c g2d_OAMEx.c \
	g2d_PaletteTable.c g2d_Image.c g2d_SRTControl.c g2d_Node.c \
	g2d_CullingUtility.c g2d_CellAnimation.c g2d_MultiCellAnimation.c \
	g2d_Entity.c g2d_Renderer.c g2d_CellTransferManager.c \
	g2d_OamSoftwareSpriteDraw.c g2d_Screen.c g2d_RendererCore.c g2d_Font.c \
	g2d_CharCanvas.c g2d_TextCanvas.c \
	internal/g2di_Mtx32.c internal/g2di_debug.c internal/g2di_BitReader.c \
	internal/g2di_SplitChar.c internal/g2di_BGManipulator.c

G3D_SRCS := \
	kernel.c glbstate.c anm.c cgtool.c sbc.c 1mat1shp.c gecom.c util.c mem.c \
	model.c \
	binres/res_print.c binres/res_print_nsbca.c binres/res_print_nsbma.c \
	binres/res_print_nsbta.c binres/res_print_nsbtp.c binres/res_print_nsbva.c \
	binres/res_struct.c binres/res_struct_accessor.c \
	binres/res_struct_accessor_anm.c \
	anm/nsbca.c anm/nsbma.c anm/nsbta.c anm/nsbtp.c anm/nsbva.c \
	cgtool/basic.c cgtool/maya.c cgtool/si3d.c cgtool/3dsmax.c cgtool/xsi.c

GFD_SRCS := \
	VramManager/gfd_TexVramMan.c VramManager/gfd_PlttVramMan.c \
	VramManager/gfd_FrameTexVramMan.c VramManager/gfd_FramePlttVramMan.c \
	VramManager/gfdi_LinkedListVramMan_Common.c \
	VramManager/gfd_LinkedListTexVramMan.c VramManager/gfd_LinkedListPlttVramMan.c \
	VramTransferMan/gfd_VramTransferManager.c

SND_SRCS := main.c resource_mgr.c waveout.c player.c stream.c capture.c \
            sndarc.c heap.c sndarc_loader.c sndarc_player.c sndarc_stream.c \
            output_effect.c seqdata.c fader.c

# Not built: mcs, the link to Nintendo's PC debugging tools (the game
# doesn't use it).

NNS_SRCS := $(FND_SRCS:%=fnd/src/%) $(G2D_SRCS:%=g2d/src/%) \
            $(G3D_SRCS:%=g3d/src/%) $(GFD_SRCS:%=gfd/src/%) \
            $(SND_SRCS:%=snd/src/%)

NNS_OBJS := $(NNS_SRCS:%.c=$(OUT)/nns/%.o)
GEN      := $(OUT)/gen
LIB      := $(OUT)/libnitrosystem.a
LIBNITRO := $(NITRO)/build/$(TARGET)/libnitro.a

# --- Flags -----------------------------------------------------------------

# Compiler, target and SDK configuration: see ../common.mk.
CFLAGS := $(PORT_CFLAGS) $(NNS_INCLUDES) $(SDK_INCLUDES)

# --- Rules -----------------------------------------------------------------

.PHONY: all lib test clean libnitro

ifeq ($(IS_HOST),yes)
all: test
else
all: lib
endif

lib: $(LIB)

# libnitro is built by its own Makefile (it also generates a header the
# SDK includes, so it has to come first).
libnitro:
	@$(MAKE) --no-print-directory -C $(NITRO) TARGET=$(TARGET) lib

$(LIBNITRO): libnitro

# The list of NitroSystem's header inline functions (see ../common.mk):
# compile a file that includes every header and keep the functions it exports.
NNS_HEADERS := $(shell find $(NNS)/include $(NNS)/libraries -name '*.h')

$(GEN)/nns_all_headers.c: $(NNS_HEADERS) $(ROOT)/tools/nns_inline_symbols.py
	@python3 $(ROOT)/tools/nns_inline_symbols.py $@

$(GEN)/nns_all_headers.o: $(GEN)/nns_all_headers.c | libnitro
	@$(CC) $(CFLAGS) -c $< -o $@

# Replaced only when the list changes: every object of NitroSystem and the
# game depends on it, so a rewrite with the same names would rebuild them all.
$(NNS_INLINE_SYMS): $(GEN)/nns_all_headers.o
	@$(NM) -g --defined-only $< | awk '$$2 == "T" { print $$3 }' > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi
	@echo "  $$(wc -l < $@) NitroSystem header inline functions"

$(OUT)/nns/%.o: $(NNS)/libraries/%.c $(NNS_INLINE_SYMS) | libnitro
	@mkdir -p $(dir $@)
	@echo "  CC  $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@
	@$(WEAKEN_NNS_INLINE)

$(LIB): $(NNS_OBJS)
	@rm -f $@
	@$(AR) rcs $@ $^
	@echo "built $@"

# Unit tests run on the PC only.
TEST_BIN := $(OUT)/test_nitrosystem

$(OUT)/test_nitrosystem.o: tests/test_nitrosystem.c $(NNS_INLINE_SYMS)
	$(CC) $(CFLAGS) -c $< -o $@
	@$(WEAKEN_NNS_INLINE)

$(TEST_BIN): $(OUT)/test_nitrosystem.o $(LIB) $(LIBNITRO)
	$(CC) $(LDFLAGS) $< $(LIB) $(LIBNITRO) -lm -o $@

# The tests load the game's own 3D and 2D files from the decompilation.
test: $(TEST_BIN)
	./$(TEST_BIN) $(ROOT)/build/upstream/res

clean:
	rm -rf build
