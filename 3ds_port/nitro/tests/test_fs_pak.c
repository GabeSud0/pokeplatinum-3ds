/*
 * Synthetic FS_* -> libpak integration checks. No ROM or original game bytes.
 * Run with a tiny pack created by tests/make_fs_fixture.py / builder/pack.py.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_fs.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int sChecks, sFailures;

#define CHECK(test) do { \
    ++sChecks; \
    if (!(test)) { \
        ++sFailures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
    } \
} while (0)

/* Two synthetic overlays: one with initialised and zero-initialised data,
 * one (loaded into ITCM on the DS) with none. */
static u8 sOvlData[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static u8 sOvlBss[4];
static const NitroPortOverlay sOverlays[] = {
    { "test_main", NITRO_PORT_OVERLAY_MAIN, sOvlData, sOvlData + sizeof(sOvlData),
      sOvlBss, sOvlBss + sizeof(sOvlBss) },
    { "test_itcm", NITRO_PORT_OVERLAY_ITCM, NULL, NULL, NULL, NULL },
};

static void check_overlays(void)
{
    FSOverlayInfo info;
    FSFile file;
    u32 address;

    NitroPort_SetOverlays(sOverlays, 2);
    CHECK(NitroPort_OverlayCount() == 2);
    CHECK(sOvlData[0] == 1);

    CHECK(FS_LoadOverlayInfo(&info, MI_PROCESSOR_ARM9, 0));
    CHECK(info.header.id == 0);
    CHECK(info.header.flag == 0 && info.header.sinit_init == NULL);
    address = (u32)(uintptr_t)FS_GetOverlayAddress(&info);
    CHECK(!(address >= HW_ITCM_IMAGE && address <= HW_ITCM_END));
    CHECK(!FS_LoadOverlayInfo(&info, MI_PROCESSOR_ARM9, 2));
    CHECK(!FS_LoadOverlayInfo(&info, MI_PROCESSOR_ARM7, 0));

    /* The ITCM overlay's label lies in ITCM (the game's overlay manager
     * keeps a separate list for those); each overlay has its own range. */
    CHECK(FS_LoadOverlayInfo(&info, MI_PROCESSOR_ARM9, 1));
    address = (u32)(uintptr_t)FS_GetOverlayAddress(&info);
    CHECK(address >= HW_ITCM_IMAGE && address <= HW_ITCM_END);

    /* Changed while loaded ... */
    CHECK(FS_LoadOverlay(MI_PROCESSOR_ARM9, 0));
    CHECK(NitroPort_IsOverlayLoaded(0));
    sOvlData[0] = 99;
    sOvlBss[3] = 42;
    CHECK(FS_UnloadOverlay(MI_PROCESSOR_ARM9, 0));
    CHECK(!NitroPort_IsOverlayLoaded(0));
    /* ... and back to the start when loaded again. */
    CHECK(FS_LoadOverlay(MI_PROCESSOR_ARM9, 0));
    CHECK(sOvlData[0] == 1 && sOvlData[7] == 8);
    CHECK(sOvlBss[3] == 0);

    /* The asynchronous form, as the game loads the field overlay: the
     * caller waits on and closes the file it gets. */
    sOvlData[1] = 77;
    CHECK(FS_LoadOverlayInfo(&info, MI_PROCESSOR_ARM9, 0));
    FS_InitFile(&file);
    CHECK(FS_LoadOverlayImageAsync(&info, &file));
    CHECK(FS_WaitAsync(&file));
    CHECK(FS_CloseFile(&file));
    FS_StartOverlay(&info);
    CHECK(sOvlData[1] == 2);

    /* An overlay with no data loads too. */
    CHECK(FS_LoadOverlay(MI_PROCESSOR_ARM9, 1));
    CHECK(FS_UnloadOverlay(MI_PROCESSOR_ARM9, 1));
}

static void check_file(const char *path, u32 id, const char *expected,
                       u32 expected_size)
{
    FSFile file;
    FSFileID fid;
    char buffer[64];
    s32 read;

    FS_InitFile(&file);
    CHECK(FS_ConvertPathToFileID(&fid, path));
    CHECK(fid.arc == FS_FindArchive("rom", 3));
    CHECK(fid.file_id == id);
    CHECK(FS_OpenFile(&file, path));
    CHECK(FS_GetLength(&file) == expected_size);
    CHECK(FS_GetPosition(&file) == 0);

    memset(buffer, 0, sizeof(buffer));
    read = FS_ReadFile(&file, buffer, 3);
    CHECK(read == (s32)(expected_size < 3 ? expected_size : 3));
    CHECK(memcmp(buffer, expected, (size_t)read) == 0);

    CHECK(FS_SeekFile(&file, 2, FS_SEEK_SET));
    CHECK(FS_GetPosition(&file) == (expected_size < 2 ? expected_size : 2));
    memset(buffer, 0, sizeof(buffer));
    read = FS_ReadFile(&file, buffer, sizeof(buffer));
    CHECK(read == (s32)(expected_size > 2 ? expected_size - 2 : 0));
    CHECK(memcmp(buffer, expected + (expected_size > 2 ? 2 : expected_size),
                 (size_t)read) == 0);
    CHECK(FS_ReadFile(&file, buffer, 1) == 0);

    CHECK(FS_SeekFile(&file, -3, FS_SEEK_END));
    CHECK(FS_GetPosition(&file) == (expected_size > 3 ? expected_size - 3 : 0));
    CHECK(FS_CloseFile(&file));

    FS_InitFile(&file);
    CHECK(FS_OpenFileFast(&file, fid));
    CHECK(FS_GetLength(&file) == expected_size);
    CHECK(FS_SeekFile(&file, 4, FS_SEEK_SET));
    CHECK(FS_GetPosition(&file) == (expected_size < 4 ? expected_size : 4));
    memset(buffer, 0, sizeof(buffer));
    read = FS_ReadFile(&file, buffer, 2);
    CHECK(read == (s32)(expected_size > 4 ? (expected_size - 4 < 2 ?
                                             expected_size - 4 : 2) : 0));
    CHECK(memcmp(buffer, expected + (expected_size > 4 ? 4 : expected_size),
                 (size_t)read) == 0);
    CHECK(FS_CloseFile(&file));
}

int main(int argc, char **argv)
{
    FSFile file;
    FSFileID missing;
    char path[512], manifest[512];

    if (argc != 2) {
        fprintf(stderr, "usage: test_fs_pak <synthetic fixture folder>\n");
        return 2;
    }
    if (snprintf(path, sizeof(path), "%s/synthetic.pak", argv[1]) >= (int)sizeof(path) ||
        snprintf(manifest, sizeof(manifest), "%s/manifest.json", argv[1]) >=
            (int)sizeof(manifest)) return 2;

    FS_Init(FS_DMA_NOT_USE);
    CHECK(FS_IsAvailable());
    CHECK(FS_FindArchive("rom", 3) != NULL);
    CHECK(!NitroPort_IsPakOpen());

    /* Missing data must fail closed. */
    FS_InitFile(&file);
    CHECK(!FS_OpenFile(&file, "rom:/hello.bin"));

    CHECK(NitroPort_OpenPak(path, manifest, "wrong-build") == PAK_ERR_BUILD_ID);
    CHECK(!NitroPort_IsPakOpen());
    CHECK(NitroPort_OpenPak(path, manifest, "fs-pak-test") == PAK_OK);
    CHECK(NitroPort_IsPakOpen());

    check_file("rom:/hello.bin", 0, "abcdefghij", 10);
    check_file("rom:/DATA\\nested.BIN", 8, "0123456789ABCDEF", 16);
    check_file("rom:/empty.dat", 31, "", 0);

    FS_InitFile(&file);
    CHECK(!FS_OpenFile(&file, "rom:/absent.bin"));
    CHECK(!FS_OpenFile(&file, "rom:/data/missing.dat"));

    missing.arc = FS_FindArchive("rom", 3);
    missing.file_id = 999;
    FS_InitFile(&file);
    CHECK(!FS_OpenFileFast(&file, missing));
    missing.file_id = 9; /* gap in the source NitroFS IDs */
    FS_InitFile(&file);
    CHECK(!FS_OpenFileFast(&file, missing));

    /* Overlays are linked into the program (src/port_overlay.c): loading
     * one restores its initial data and clears its zero-initialised data,
     * as reading a fresh copy from the cartridge did. */
    check_overlays();

    NitroPort_ClosePak();
    CHECK(!NitroPort_IsPakOpen());
    FS_InitFile(&file);
    CHECK(!FS_OpenFile(&file, "rom:/hello.bin"));
    FS_End();

    printf("pak-backed NitroFS: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
