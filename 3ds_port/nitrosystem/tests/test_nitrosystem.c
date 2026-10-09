/*
 * Unit tests for libnitrosystem (NitroSystem built with GCC).
 *
 * Runs on the PC: `make` in 3ds_port/nitrosystem. The argument is the
 * decompilation's res/ folder; the 3D and 2D tests load the game's real
 * files from it.
 *
 * Expected values come from outside the code under test: by-hand arithmetic,
 * the DS hardware's documented behaviour, or the raw bytes of the game's
 * files read by the small, independent readers below. The 2D file loaders
 * fix up 4-byte pointers inside the files, so their tests only run in a
 * 32-bit build (TARGET=host32, as on the 3DS); a 64-bit build skips them.
 */

#define _XOPEN_SOURCE 700
#include <nitro.h>
#include <nitro_port.h>
#include <nnsys.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>

static int sChecks, sFailures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);      \
        sChecks++;                                                           \
        if (a_ != e_) {                                                      \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s = %lld (0x%llx), expected %lld (0x%llx)\n", \
                   __FILE__, __LINE__, #actual, a_, (unsigned long long)a_,  \
                   e_, (unsigned long long)e_);                              \
        }                                                                    \
    } while (0)

/* Checks inside per-file loops: report the file once, count every check. */
static const char *sCurrentFile;
#define CHECK_FILE(cond)                                                     \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, sCurrentFile); \
        }                                                                    \
    } while (0)

#define IS_32BIT (sizeof(void *) == 4)

/* Static memory: on a 64-bit PC it sits below 4 GB (non-PIE build), where
 * the SDK's 32-bit address arithmetic works. */
/* Keep data buffers in static storage (not on the test process stack). */
static u8 sFile[256 * 1024] __attribute__((aligned(32)));
/* NitroSystem patches offset fields into live pointers in place. Compare
 * against these untouched bytes, never against already-unpacked fields. */
static u8 sRawFile[sizeof(sFile)] __attribute__((aligned(32)));
static u8 sHeapMem[64 * 1024] __attribute__((aligned(32)));

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24)); }

static u32 load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(sFile, 1, sizeof(sFile), f);
    fclose(f);
    memcpy(sRawFile, sFile, n);
    return (u32)n;
}

/* Independent reader for the 2D formats (NCGR, NCLR, ...): a 16-byte file
 * header, then blocks laid out one after another, each starting with a
 * 4-byte signature and a 4-byte size. Returns the block, or NULL. */
static const u8 *find_block(const u8 *file, u32 size, const char *sig)
{
    u32 off = rd16(file + 12);
    int n = rd16(file + 14), i;
    for (i = 0; i < n && off + 8 <= size; i++) {
        if (memcmp(file + off, sig, 4) == 0) return file + off;
        off += rd32(file + off + 4);
    }
    return NULL;
}

/* ------------------------------------------------------------- Fnd heaps */

static void test_exp_heap(void)
{
    NNSFndHeapHandle heap = NNS_FndCreateExpHeap(sHeapMem, sizeof(sHeapMem));
    u32 freeAtStart;
    u8 *a, *b, *c;

    CHECK(heap != NNS_FND_HEAP_INVALID_HANDLE);
    freeAtStart = NNS_FndGetTotalFreeSizeForExpHeap(heap);
    CHECK(freeAtStart > 0 && freeAtStart < sizeof(sHeapMem));

    /* From the front, default 4-byte alignment. */
    a = NNS_FndAllocFromExpHeap(heap, 100);
    CHECK(a != NULL);
    CHECK(a >= sHeapMem && a + 100 <= sHeapMem + sizeof(sHeapMem));
    CHECK_EQ((u32)a & 3, 0);
    CHECK(NNS_FndGetSizeForMBlockExpHeap(a) >= 100);

    /* Negative alignment allocates from the back of the heap. */
    b = NNS_FndAllocFromExpHeapEx(heap, 1000, -32);
    CHECK(b != NULL);
    CHECK_EQ((u32)b & 31, 0);
    CHECK(b > a + 100);
    CHECK(b + 1000 <= sHeapMem + sizeof(sHeapMem));

    c = NNS_FndAllocFromExpHeapEx(heap, 200, 16);
    CHECK(c != NULL);
    CHECK_EQ((u32)c & 15, 0);
    CHECK(c >= a + 100 && c + 200 <= b);
    CHECK(NNS_FndFindContainHeap(c) == heap);

    /* Writing a whole block must not disturb its neighbours. */
    memset(a, 0xAA, 100);
    memset(c, 0xCC, 200);
    memset(b, 0xBB, 1000);
    CHECK(a[99] == 0xAA && c[0] == 0xCC && c[199] == 0xCC && b[0] == 0xBB);

    /* Growing a block in place, then freeing everything, gives back all memory. */
    CHECK(NNS_FndResizeForMBlockExpHeap(heap, c, 400) >= 400);
    NNS_FndFreeToExpHeap(heap, a);
    NNS_FndFreeToExpHeap(heap, b);
    NNS_FndFreeToExpHeap(heap, c);
    CHECK_EQ(NNS_FndGetTotalFreeSizeForExpHeap(heap), freeAtStart);

    /* Too big fails cleanly. */
    CHECK(NNS_FndAllocFromExpHeap(heap, sizeof(sHeapMem)) == NULL);
    NNS_FndDestroyExpHeap(heap);
}

static void test_frame_heap(void)
{
    NNSFndHeapHandle heap = NNS_FndCreateFrmHeap(sHeapMem, sizeof(sHeapMem));
    u8 *a, *b, *c, *t;

    CHECK(heap != NNS_FND_HEAP_INVALID_HANDLE);

    /* A frame heap hands out memory in order, with no per-block header. */
    a = NNS_FndAllocFromFrmHeap(heap, 64);
    b = NNS_FndAllocFromFrmHeap(heap, 64);
    CHECK(a != NULL && b != NULL);
    CHECK_EQ(b - a, 64);

    /* Saved state: everything after it can be released at once. */
    CHECK(NNS_FndRecordStateForFrmHeap(heap, 'TEST'));
    c = NNS_FndAllocFromFrmHeap(heap, 256);
    /* RecordState writes a marker into the heap: 16 bytes on 32-bit and
     * 32 on 64-bit; there is no fixed 64-byte gap from b to c. */
    CHECK(c > b + 64);
    t = NNS_FndAllocFromFrmHeapEx(heap, 128, -32);   /* from the back */
    CHECK(t != NULL && ((u32)t & 31) == 0 && t > c);
    CHECK(NNS_FndFreeByStateToFrmHeap(heap, 'TEST'));
    /* The saved-state marker is itself freed on rewind, so the next
     * allocation returns at the point before the marker, not at c. */
    CHECK(NNS_FndAllocFromFrmHeap(heap, 32) == b + 64);

    NNS_FndFreeToFrmHeap(heap, NNS_FND_FRMHEAP_FREE_ALL);
    CHECK(NNS_FndAllocFromFrmHeap(heap, 16) == a);
    NNS_FndDestroyFrmHeap(heap);
}

static void test_unit_heap(void)
{
    NNSFndHeapHandle heap = NNS_FndCreateUnitHeap(sHeapMem, 4096, 64);
    u32 units, i;
    u8 *first, *prev = NULL;

    CHECK(heap != NNS_FND_HEAP_INVALID_HANDLE);
    CHECK_EQ(NNS_FndGetMemBlockSizeForUnitHeap(heap), 64);
    units = NNS_FndCountFreeBlockForUnitHeap(heap);
    /* All of the 4 KB except the heap's own header, in 64-byte units. */
    CHECK(units >= 4096 / 64 - 2 && units < 4096 / 64);

    first = NNS_FndAllocFromUnitHeap(heap);
    prev = first;
    for (i = 1; i < units; i++) {
        u8 *p = NNS_FndAllocFromUnitHeap(heap);
        if (p == NULL) break;
        if (p != prev + 64) break;   /* consecutive units */
        prev = p;
    }
    CHECK_EQ(i, units);
    CHECK(NNS_FndAllocFromUnitHeap(heap) == NULL);
    CHECK_EQ(NNS_FndCountFreeBlockForUnitHeap(heap), 0);

    NNS_FndFreeToUnitHeap(heap, first);
    CHECK_EQ(NNS_FndCountFreeBlockForUnitHeap(heap), 1);
    CHECK(NNS_FndAllocFromUnitHeap(heap) == first);
    NNS_FndDestroyUnitHeap(heap);
}

/* ------------------------------------------------- Gfd VRAM managers */

static void test_vram_managers(void)
{
    static u8 work[4096];
    NNSGfdTexKey k1, k2, k3, k4;
    NNSGfdPlttKey p1, p2;

    /* Texture VRAM frame manager: one 128 KB slot. NitroSystem's
     * DoAllocNrm_ allocates from the end of this slot; opt is ignored.
     * Two allocations therefore occupy [0x1F000, 0x20000) and then
     * [0x1D000, 0x1F000), without overlapping. */
    NNS_GfdInitFrmTexVramManager(1, TRUE);
    k1 = NNS_GfdAllocTexVram(0x1000, FALSE, NNS_GFD_ALLOC_FROM_LOW);
    k2 = NNS_GfdAllocTexVram(0x2000, FALSE, NNS_GFD_ALLOC_FROM_LOW);
    CHECK_EQ(NNS_GfdGetTexKeyAddr(k1), 0x1F000);
    CHECK_EQ(NNS_GfdGetTexKeySize(k1), 0x1000);
    CHECK_EQ(NNS_GfdGetTexKeyAddr(k2), 0x1D000);
    CHECK(NNS_GfdAllocTexVram(0x20000, FALSE, NNS_GFD_ALLOC_FROM_LOW) == NNS_GFD_ALLOC_ERROR_TEXKEY);

    /* Linked-list manager: 128 KB, no compressed-texture region.
     * Its first-fit allocator starts at address 0; opt is ignored. */
    NNS_GfdInitLnkTexVramManager(0x20000, 0, work, sizeof(work), TRUE);
    k1 = NNS_GfdAllocTexVram(0x1000, FALSE, 0);
    k2 = NNS_GfdAllocTexVram(0x2000, FALSE, 0);
    CHECK(k1 != NNS_GFD_ALLOC_ERROR_TEXKEY && k2 != NNS_GFD_ALLOC_ERROR_TEXKEY);
    CHECK_EQ(NNS_GfdGetTexKeyAddr(k1), 0);
    CHECK_EQ(NNS_GfdGetTexKeySize(k1), 0x1000);
    CHECK_EQ(NNS_GfdGetTexKeyAddr(k2), 0x1000);
    CHECK_EQ(NNS_GfdGetTexKeySize(k2), 0x2000);

    /* Sizes round up to the 16-byte texture granularity. */
    k3 = NNS_GfdAllocTexVram(0x7F1, FALSE, 0);
    CHECK_EQ(NNS_GfdGetTexKeyAddr(k3), 0x3000);
    CHECK_EQ(NNS_GfdGetTexKeySize(k3), 0x800);

    /* Freed space is reused. */
    CHECK_EQ(NNS_GfdFreeTexVram(k1), 0);
    k4 = NNS_GfdAllocTexVram(0x800, FALSE, 0);
    CHECK_EQ(NNS_GfdGetTexKeyAddr(k4), 0);

    /* More than is left fails. */
    CHECK(NNS_GfdAllocTexVram(0x20000, FALSE, 0) == NNS_GFD_ALLOC_ERROR_TEXKEY);

    /* Palette VRAM, frame manager: 4-color palettes (8 bytes) and the rest
     * (16-byte aligned). */
    NNS_GfdInitFrmPlttVramManager(0x2000, TRUE);
    p1 = NNS_GfdAllocPlttVram(0x20, FALSE, NNS_GFD_ALLOC_FROM_LOW);
    p2 = NNS_GfdAllocPlttVram(0x08, TRUE, NNS_GFD_ALLOC_FROM_LOW);
    CHECK(p1 != NNS_GFD_ALLOC_ERROR_PLTTKEY && p2 != NNS_GFD_ALLOC_ERROR_PLTTKEY);
    CHECK_EQ(NNS_GfdGetPlttKeyAddr(p1), 0);
    CHECK_EQ(NNS_GfdGetPlttKeySize(p1), 0x20);
    CHECK_EQ(NNS_GfdGetPlttKeyAddr(p2), 0x20);
    CHECK_EQ(NNS_GfdGetPlttKeySize(p2), 0x08);
}

/* --------------------------------------------------- G2d sprite (OAM) manager */

static void test_oam_manager(void)
{
    NNSG2dOamManagerInstance man;
    GXOamAttr oam;
    const u16 *hwOam = (const u16 *)HW_OAM;
    int i, ok;

    NNS_G2dInitOamManagerModule();
    CHECK(NNS_G2dGetNewOamManagerInstance(&man, 0, 16, 0, 0, NNS_G2D_OAMTYPE_MAIN));
    CHECK_EQ(NNS_G2dGetOamManagerOamCapacity(&man), 16);

    /* One sprite: attribute words 0..2 as the DS stores them in OAM. */
    oam.attr0 = 0x2050;   /* y = 0x50, 256 colors */
    oam.attr1 = 0x4060;   /* x = 0x60, size 1 */
    oam.attr2 = 0x0123;   /* tile 0x123 */
    oam._3 = 0;
    CHECK(NNS_G2dEntryOamManagerOam(&man, &oam, 1));
    MI_CpuFill16((void *)HW_OAM, 0xFFFF, 16 * 8);
    NNS_G2dApplyAndResetOamManagerBuffer(&man);

    CHECK_EQ(hwOam[0], 0x2050);
    CHECK_EQ(hwOam[1], 0x4060);
    CHECK_EQ(hwOam[2], 0x0123);
    /* The manager's other 15 sprites are parked at y = 192, just below the
     * DS's 192-line screen, which hides them. */
    ok = 1;
    for (i = 1; i < 16; i++) ok &= (hwOam[i * 4] & 0xFF) == 192;
    CHECK(ok);
}

/* ----------------------------------------------- G3d: the game's 3D models */

static int sModels, sModelsInitialized, sModelChecksFailed;

/* Independent reader for the model set: a 3D file's header lists block
 * offsets; the MDL0 block holds a dictionary (Nintendo's "resource dict"):
 * 8-byte header, a search tree with numEntry + 1 nodes, then an entry table
 * {u16 entrySize, u16 namesOffset} followed by the entries, with 16-byte
 * names at namesOffset. */
static const u8 *find_mdl0(const u8 *file, u32 size)
{
    int i, n = rd16(file + 14);
    for (i = 0; i < n; i++) {
        u32 off = rd32(file + 16 + 4 * i);
        if (off + 8 <= size && memcmp(file + off, "MDL0", 4) == 0) return file + off;
    }
    return NULL;
}

static void check_model_file(const char *path)
{
    u32 size = load_file(path);
    const NNSG3dResFileHeader *hdr = (const NNSG3dResFileHeader *)sFile;
    const u8 *mdl0, *dict, *entries;
    NNSG3dResMdlSet *set;
    int numEntry, i, before = sFailures;

    sCurrentFile = path;
    sModels++;
    CHECK_FILE(size > 16 && size < sizeof(sFile));
    CHECK_FILE(memcmp(sFile, "BMD0", 4) == 0);
    CHECK_FILE(rd32(sFile + 8) == size);
    mdl0 = find_mdl0(sFile, size);
    set = NNS_G3dGetMdlSet(hdr);
    CHECK_FILE(mdl0 != NULL && (const u8 *)set == mdl0);
    if (mdl0 == NULL || set == NULL) return;

    dict = mdl0 + 8;
    numEntry = dict[1];
    entries = dict + rd16(dict + 6);
    CHECK_FILE(set->dict.numEntry == numEntry);

    for (i = 0; i < numEntry; i++) {
        const u8 *name = entries + rd16(entries + 2) + 16 * i;
        u32 mdlOfs = rd32(entries + 4 + rd16(entries) * i);
        NNSG3dResMdl *byIdx = NNS_G3dGetMdlByIdx(set, (u32)i);
        NNSG3dResMdl *byName = NNS_G3dGetMdlByName(set, (const NNSG3dResName *)name);
        NNSG3dRenderObj obj;

        /* The dictionary's search tree finds the same model as its index. */
        CHECK_FILE(byIdx == (NNSG3dResMdl *)(mdl0 + mdlOfs));
        CHECK_FILE(byName == byIdx);
        if (byIdx == NULL || byIdx != byName) continue;
        CHECK_FILE(byIdx->size > 0 && mdlOfs + byIdx->size <= size - (u32)(mdl0 - sFile));

        /* Validate the resource and the CPU-side render-object setup.
         * Do not execute NNS_G3dGlbFlushP/NNS_G3dDraw here: NitroSDK's
         * virtual registers currently have no geometry FIFO interpreter,
         * so GPU synchronization/draw paths may wait forever. A separate
         * renderer integration test will cover SBC drawing once implemented. */
        NNS_G3dRenderObjInit(&obj, byIdx);
        CHECK_FILE(obj.resMdl == byIdx);
        sModelsInitialized++;
    }
    if (sFailures != before) sModelChecksFailed++;
}

static int visit_model(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    size_t n = strlen(path);
    (void)st; (void)ftw;
    if (type == FTW_F && n > 6 && strcmp(path + n - 6, ".nsbmd") == 0) check_model_file(path);
    return 0;
}

static void test_models(const char *res)
{
    /* NNS_G3dInit configures/awaits the original DS geometry engine.
     * The platform port has only virtual registers, not a responding GPU,
     * so data-only tests intentionally avoid hardware initialization. */
    nftw(res, visit_model, 16, FTW_PHYS);
    printf("  3D models: %d files, %d render objects initialized, %d files with failures (GPU draw deferred)\n",
           sModels, sModelsInitialized, sModelChecksFailed);
    CHECK(sModels > 600);   /* the decompilation has 619 */
}

/* ----------------------------------------------- G2d: the game's 2D files */

static int s2dFiles[5];

static void check_2d_file(const char *path, int kind)
{
    u32 size = load_file(path);
    const u8 *blk, *rawBlk;

    sCurrentFile = path;
    s2dFiles[kind]++;
    /* The two dp_areawindow NCGR/NCLR fixtures contain eight extra bytes
     * after their header-declared binary length. Validate the declared
     * extent, permitting only an exact 8-byte trailer (not truncation or
     * arbitrarily sized extra payloads). */
    {
        const u32 declaredSize = rd32(sRawFile + 8);
        const BOOL validSize = size > 16 && size < sizeof(sFile) &&
            declaredSize >= 16 && declaredSize <= size &&
            (declaredSize == size || size - declaredSize == 8);
        CHECK_FILE(validSize);
        if (!validSize) {
            printf("  invalid resource size: %s (bytes read: %u; header length: %u)\n",
                   path, size, declaredSize);
            return;
        }
    }

    switch (kind) {
    case 0: {   /* NCGR: tiles. CHAR block: u16 height, u16 width, u32 format,
                   u32 mapping, u32 tile/bitmap, u32 size, u32 data offset */
        NNSG2dCharacterData *cd;
        blk = find_block(sFile, size, "RAHC");
        CHECK_FILE(blk != NULL);
        if (blk == NULL) return;
        rawBlk = sRawFile + (blk - sFile);
        CHECK_FILE(NNS_G2dGetUnpackedCharacterData(sFile, &cd));
        if (!blk || !cd) return;
        CHECK_FILE((const u8 *)cd == blk + 8);
        CHECK_FILE(cd->H == rd16(rawBlk + 8) && cd->W == rd16(rawBlk + 10));
        CHECK_FILE((u32)cd->pixelFmt == rd32(rawBlk + 12) && cd->szByte == rd32(rawBlk + 24));
        CHECK_FILE((const u8 *)cd->pRawData == blk + 8 + rd32(rawBlk + 28));
        CHECK_FILE((const u8 *)cd->pRawData + cd->szByte <= sFile + size);
        break;
    }
    case 1: {   /* NCLR: palettes. PLTT block: u32 format, u32 extended,
                   u32 size, u32 data offset */
        NNSG2dPaletteData *pd;
        blk = find_block(sFile, size, "TTLP");
        CHECK_FILE(blk != NULL);
        if (blk == NULL) return;
        rawBlk = sRawFile + (blk - sFile);
        CHECK_FILE(NNS_G2dGetUnpackedPaletteData(sFile, &pd));
        if (!blk || !pd) return;
        CHECK_FILE((u32)pd->fmt == rd32(rawBlk + 8) && pd->szByte == rd32(rawBlk + 16));
        CHECK_FILE((const u8 *)pd->pRawData == blk + 8 + rd32(rawBlk + 20));
        CHECK_FILE((const u8 *)pd->pRawData + pd->szByte <= sFile + size);
        break;
    }
    case 2: {   /* NCER: sprite cells. CEBK block: u16 cells, u16 attributes,
                   u32 cell table offset; each cell {u16 OAM count, u16 attr,
                   u32 OAM offset} plus an 8-byte bounding box if attr bit 0 */
        NNSG2dCellDataBank *bank;
        const u8 *cells, *oamBase;
        int i, entry;
        blk = find_block(sFile, size, "KBEC");
        CHECK_FILE(blk != NULL);
        if (blk == NULL) return;
        rawBlk = sRawFile + (blk - sFile);
        CHECK_FILE(NNS_G2dGetUnpackedCellBank(sFile, &bank));
        if (!blk || !bank) return;
        CHECK_FILE(bank->numCells == rd16(rawBlk + 8));
        entry = (rd16(rawBlk + 10) & 1) ? 16 : 8;
        cells = blk + 8 + rd32(rawBlk + 12);
        oamBase = cells + entry * bank->numCells;
        for (i = 0; i < bank->numCells; i++) {
            const NNSG2dCellData *c = NNS_G2dGetCellDataByIdx(bank, (u16)i);
            const u8 *raw = rawBlk + 8 + rd32(rawBlk + 12) + entry * i;
            CHECK_FILE(c->numOAMAttrs == rd16(raw));
            CHECK_FILE((const u8 *)c->pOamAttrArray == oamBase + rd32(raw + 4));
        }
        break;
    }
    case 3: {   /* NANR: animations. ABNK block: u16 sequences, u16 frames,
                   u32 offsets of sequences, frames and frame contents */
        NNSG2dAnimBankData *ab;
        blk = find_block(sFile, size, "KNBA");
        CHECK_FILE(blk != NULL);
        if (blk == NULL) return;
        rawBlk = sRawFile + (blk - sFile);
        CHECK_FILE(NNS_G2dGetUnpackedAnimBank(sFile, &ab));
        if (!blk || !ab) return;
        CHECK_FILE(ab->numSequences == rd16(rawBlk + 8) && ab->numTotalFrames == rd16(rawBlk + 10));
        CHECK_FILE((const u8 *)ab->pSequenceArrayHead == blk + 8 + rd32(rawBlk + 12));
        CHECK_FILE((const u8 *)ab->pFrameArrayHead == blk + 8 + rd32(rawBlk + 16));
        CHECK_FILE((const u8 *)ab->pAnimContents == blk + 8 + rd32(rawBlk + 20));
        break;
    }
    case 4: {   /* NSCR: background maps. SCRN block: u16 width, u16 height
                   (in pixels), u16 color mode, u16 format, u32 size, data */
        NNSG2dScreenData *sd;
        blk = find_block(sFile, size, "NRCS");
        CHECK_FILE(blk != NULL);
        if (blk == NULL) return;
        rawBlk = sRawFile + (blk - sFile);
        CHECK_FILE(NNS_G2dGetUnpackedScreenData(sFile, &sd));
        if (!blk || !sd) return;
        CHECK_FILE(sd->screenWidth == rd16(rawBlk + 8) && sd->screenHeight == rd16(rawBlk + 10));
        CHECK_FILE(sd->szByte == rd32(rawBlk + 16));
        /* one 2-byte entry per 8x8 tile in the usual text format */
        if (sd->colorMode == 0 && sd->screenFormat == 0)
            CHECK_FILE(sd->szByte == (u32)(sd->screenWidth / 8) * (sd->screenHeight / 8) * 2);
        CHECK_FILE((const u8 *)sd->rawData == blk + 20);
        break;
    }
    }
}

static int visit_2d(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    static const char *const ext[] = { ".NCGR", ".NCLR", ".NCER", ".NANR", ".NSCR" };
    size_t n = strlen(path);
    int k;
    (void)st; (void)ftw;
    if (type != FTW_F || n < 5) return 0;
    for (k = 0; k < 5; k++)
        if (strcmp(path + n - 5, ext[k]) == 0) check_2d_file(path, k);
    return 0;
}

static void test_2d_files(const char *res)
{
    if (!IS_32BIT) {
        printf("  2D files: skipped (64-bit build; run `make TARGET=host32`)\n");
        return;
    }
    nftw(res, visit_2d, 16, FTW_PHYS);
    printf("  2D files: %d tiles, %d palettes, %d cell banks, %d animations, %d maps\n",
           s2dFiles[0], s2dFiles[1], s2dFiles[2], s2dFiles[3], s2dFiles[4]);
    CHECK(s2dFiles[0] > 0 && s2dFiles[1] > 0 && s2dFiles[2] > 0 && s2dFiles[3] > 0 && s2dFiles[4] > 0);
}

/* ---------------------------------------------------------- structures */

static void test_struct_sizes(void)
{
    /* Layouts the game's data files depend on, from the formats above.
     * Pointers inside them are 4 bytes on the 3DS. */
    CHECK_EQ(sizeof(NNSG3dResFileHeader), 16);
    CHECK_EQ(sizeof(NNSG3dResDataBlockHeader), 8);
    CHECK_EQ(sizeof(NNSG3dResName), 16);
    CHECK_EQ(sizeof(GXOamAttr), 8);
    if (IS_32BIT) {
        CHECK_EQ(sizeof(NNSG2dCharacterData), 24);
        CHECK_EQ(sizeof(NNSG2dPaletteData), 16);
        CHECK_EQ(sizeof(NNSG2dCellData), 8);
        CHECK_EQ(sizeof(NNSG2dCellDataBank), 24);
        CHECK_EQ(sizeof(NNSG2dAnimBankData), 24);
    }
}

int main(int argc, char **argv)
{
    const char *res = argc > 1 ? argv[1] : "../../build/upstream/res";

    OS_Init();
    printf("libnitrosystem tests (%d-bit build), game files from %s\n",
           (int)sizeof(void *) * 8, res);

    /* Emit unbuffered progress so CI identifies a hanging subsystem even
     * if a test stops inside Nintendo's original draw/load code. */
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("  test: struct sizes");
    test_struct_sizes();
    puts("  test: Fnd expansion heap");
    test_exp_heap();
    puts("  test: Fnd frame heap");
    test_frame_heap();
    puts("  test: Fnd unit heap");
    test_unit_heap();
    puts("  test: Gfd VRAM managers");
    test_vram_managers();
    puts("  test: G2d OAM");
    test_oam_manager();
    puts("  test: G3d models");
    test_models(res);
    puts("  test: G2d resource files");
    test_2d_files(res);

    printf("%d checks, %d failed\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
