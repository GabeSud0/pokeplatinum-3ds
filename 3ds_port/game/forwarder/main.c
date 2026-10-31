/* The HOME Menu version (the .cia): a forwarder. It opens the Homebrew
 * Launcher version, sd:/3ds/pokeplatinum.3dsx, the way the Homebrew
 * Launcher does, so the two are one program: updating the game is copying
 * the new .3dsx, and the .cia (its icon and its banner from the player's
 * ROM) stays as it is.
 *
 * How: Luma3DS's loader runs a .3dsx in place of one title, its homebrew
 * title (the Homebrew Launcher Loader's, 000400000D921E00, unless Rosalina
 * was told another). Its service hb:ldr is told which .3dsx (SetTarget) and
 * the program's arguments (SetArgv); then this program closes with a jump
 * to that title (libctru's chainloader), and the game runs there exactly as
 * it does from the Homebrew Launcher. Its arguments say it came from here
 * (argv[1], FORWARDED_ARG plus this title's ID), so the game's soft reset
 * comes back through here rather than to the Homebrew Launcher.
 *
 * Anything missing (the .3dsx, Luma3DS, the homebrew title): a message on
 * the upper screen and a button to close. */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define HOMEBREW_TITLE 0x000400000D921E00ULL   /* (Luma3DS's default) */
#define FORWARDED_ARG "--home-menu="           /* (+ this title's ID in hex: main_3ds.c) */
#define ARGV_BYTES 0x400                       /* (Luma3DS's loader's argument buffer) */

/* Where the .3dsx may be: the web builder's place, then a folder of its own. */
static const char *const sPrograms[] = {
    "sdmc:/3ds/pokeplatinum.3dsx",
    "sdmc:/3ds/pokeplatinum/pokeplatinum.3dsx",
};

static Handle sHbldr;
static u32 sArgv[ARGV_BYTES / 4];   /* argc, then the arguments one after the other */

static Result HbldrSetTarget(const char *path)
{
    u32 *cmd = getThreadCommandBuffer();
    Result rc;

    cmd[0] = IPC_MakeHeader(2, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmd[2] = (u32)path;
    rc = svcSendSyncRequest(sHbldr);
    return R_SUCCEEDED(rc) ? (Result)cmd[1] : rc;
}

static Result HbldrSetArgv(const void *args, u32 size)
{
    u32 *cmd = getThreadCommandBuffer();
    Result rc;

    cmd[0] = IPC_MakeHeader(3, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(size, 1);
    cmd[2] = (u32)args;
    rc = svcSendSyncRequest(sHbldr);
    return R_SUCCEEDED(rc) ? (Result)cmd[1] : rc;
}

/* The homebrew title (Luma3DS says which, with its kernel extension) and
 * where it's installed: whether it's there at all. */
static bool HomebrewTitle(u64 *title, FS_MediaType *media)
{
    s64 out = 0;
    AM_TitleInfo info;
    bool found = false;

    *title = HOMEBREW_TITLE;
    if (R_SUCCEEDED(svcGetSystemInfo(&out, 0x10000, 0x100)) && out != 0) {
        *title = (u64)out;
    }
    if (R_FAILED(amInit())) {
        /* (no way to look: where a title of its kind is) */
        *media = (*title >> 32) == 0x00040000u ? MEDIATYPE_SD : MEDIATYPE_NAND;
        return true;
    }
    if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, title, &info))) {
        *media = MEDIATYPE_SD;
        found = true;
    } else if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_NAND, 1, title, &info))) {
        *media = MEDIATYPE_NAND;
        found = true;
    }
    amExit();
    return found;
}

/* argv: the .3dsx's path (as the Homebrew Launcher gives it), then where
 * it came from. */
static void MakeArgv(const char *program)
{
    char *p = (char *)&sArgv[1];
    char *end = (char *)sArgv + sizeof(sArgv);
    u64 self = 0;

    memset(sArgv, 0, sizeof(sArgv));
    APT_GetProgramID(&self);
    sArgv[0] = 2;
    p += snprintf(p, (size_t)(end - p), "%s", program) + 1;
    snprintf(p, (size_t)(end - p), "%s%016llX", FORWARDED_ARG, (unsigned long long)self);
}

static void ShowProblem(const char *what, const char *help)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\n  Pokemon Platinum 3DS\n\n  %s\n\n%s\n\n  Press any button to close.\n", what, help);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown()) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
}

int main(void)
{
    const char *program = NULL;
    u64 title = 0;
    FS_MediaType media = MEDIATYPE_SD;
    size_t i;

    for (i = 0; i < sizeof(sPrograms) / sizeof(sPrograms[0]) && program == NULL; i++) {
        struct stat st;

        if (stat(sPrograms[i], &st) == 0) {
            program = sPrograms[i];
        }
    }
    if (program == NULL) {
        ShowProblem("The game isn't on the SD card.",
                    "  The HOME Menu version opens\n"
                    "  sd:/3ds/pokeplatinum.3dsx:\n"
                    "  copy it there (from the web\n"
                    "  builder's ZIP, or a release).");
        return 0;
    }
    if (R_FAILED(svcConnectToPort(&sHbldr, "hb:ldr"))) {
        ShowProblem("This needs Luma3DS.",
                    "  The HOME Menu version opens the\n"
                    "  game through Luma3DS's homebrew\n"
                    "  loader. Update Luma3DS, or open\n"
                    "  pokeplatinum.3dsx from the\n"
                    "  Homebrew Launcher.");
        return 0;
    }
    if (!HomebrewTitle(&title, &media)) {
        svcCloseHandle(sHbldr);
        ShowProblem("The Homebrew Launcher isn't installed.",
                    "  The HOME Menu version opens the\n"
                    "  game through the Homebrew\n"
                    "  Launcher Loader (installed with\n"
                    "  3ds.hacks.guide's Finalizing\n"
                    "  Setup). Install it, or open\n"
                    "  pokeplatinum.3dsx from the\n"
                    "  Homebrew Launcher.");
        return 0;
    }
    MakeArgv(program);
    if (R_FAILED(HbldrSetTarget(program + 5)) || R_FAILED(HbldrSetArgv(sArgv, sizeof(sArgv)))) {
        svcCloseHandle(sHbldr);
        ShowProblem("Luma3DS didn't take the game.",
                    "  Open pokeplatinum.3dsx from the\n"
                    "  Homebrew Launcher instead.");
        return 0;
    }
    svcCloseHandle(sHbldr);
    /* (on closing: the jump, which the HOME Menu does) */
    aptSetChainloader(title, (u8)media);
    return 0;
}
