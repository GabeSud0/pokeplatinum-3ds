/*
 * Synthetic FS_* -> libpak integration checks. No ROM or original game bytes.
 * Run with a tiny pack created by tests/make_fs_fixture.py / builder/pack.py.
 */
#include <nitro.h>
#include <nitro_port_fs.h>

#include <stdio.h>
#include <string.h>

static int sChecks, sFailures;

#define CHECK(test) do { \
    ++sChecks; \
    if (!(test)) { \
        ++sFailures; \
        fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #test); \
    } \
} while (0)

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
    FSOverlayInfo overlay;
    unsigned char overlay_data[16];
    char path[512], manifest[512];

    if (argc != 2) {
        fprintf(stderr, "usage: test_fs_pak <synthetic fixture folder>\\n");
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

    /* Overlay payload loading uses precisely the same file-ID path. The
     * rest of FS_LoadOverlay needs overlay metadata/relocation integration. */
    memset(&overlay, 0, sizeof(overlay));
    memset(overlay_data, 0, sizeof(overlay_data));
    overlay.header.file_id = 23;
    overlay.header.ram_address = overlay_data;
    overlay.header.ram_size = sizeof("FAKE-OVERLAY") - 1;
    overlay.header.bss_size = 0;
    overlay.header.flag = 0;
    CHECK(FS_LoadOverlayImage(&overlay));
    CHECK(memcmp(overlay_data, "FAKE-OVERLAY", sizeof("FAKE-OVERLAY") - 1) == 0);

    NitroPort_ClosePak();
    CHECK(!NitroPort_IsPakOpen());
    FS_InitFile(&file);
    CHECK(!FS_OpenFile(&file, "rom:/hello.bin"));
    FS_End();

    printf("pak-backed NitroFS: %d checks, %d failed\\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
