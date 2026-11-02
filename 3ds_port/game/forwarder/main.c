/* The HOME Menu version (the .cia): a forwarder. It opens the Homebrew
 * Launcher version, sd:/3ds/pokeplatinum.3dsx, the way the Homebrew
 * Launcher does, so the two are one program: updating the game is copying
 * the new .3dsx, and the .cia (its icon and its banner from the player's
 * ROM) stays as it is.
 *
 * How: Luma3DS's loader runs a .3dsx in place of one title, its homebrew
 * title (the Homebrew Launcher Loader's, 000400000D921E00, unless Rosalina
 * was told another). Its service hb:ldr is told which .3dsx (SetTarget) and
 * the program's arguments (SetArgv). Then this title is made the homebrew
 * title for one start (Luma3DS's shared page: the title "selected", which
 * Luma's process manager makes the homebrew title as this program closes,
 * as Rosalina's "Switch the hb. title to the current app" does) and this
 * program closes with a jump to itself (libctru's chainloader): Luma3DS
 * loads the game in this title's place, so the HOME Menu shows this title
 * as the one running (with a jump to the Homebrew Launcher Loader's title
 * instead, the HOME button showed that one's icon: 2026-11-01). The game
 * puts the title selected before back as soon as it starts (main_3ds.c,
 * HB_TITLE_ARG), so the next start runs this program again. Emerald 3DS's
 * forwarder does the same (github.com/ZallaxDev/emerald-3ds). Where the
 * shared page isn't laid out as known (another Luma3DS), the jump to the
 * homebrew title, as before.
 *
 * Its arguments say it came from here (argv[1], FORWARDED_ARG plus this
 * title's ID), so the game's soft reset comes back through here rather than
 * to the Homebrew Launcher.
 *
 * Anything missing (the .3dsx, Luma3DS, the homebrew title): a message on
 * the upper screen and a button to close. */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define HOMEBREW_TITLE 0x000400000D921E00ULL   /* (Luma3DS's default) */
#define FORWARDED_ARG "--home-menu="           /* (+ this title's ID in hex: main_3ds.c) */
#define HB_TITLE_ARG "--hb-title="              /* (+ the homebrew title to put back: main_3ds.c) */
#define ARGV_BYTES 0x400                       /* (Luma3DS's loader's argument buffer) */

/* Luma3DS's shared page, its config part (Luma3DS's own layout, the same
 * since v10): the homebrew title now, and the one it becomes when the
 * running program closes. */
typedef struct LumaSharedConfig {
    u64 hbldr3dsxTid;
    u64 selectedHbldr3dsxTid;
    bool useHbldr;
} LumaSharedConfig;
#define LUMA_SHARED_CONFIG ((volatile LumaSharedConfig *)(OS_SHAREDCFG_VADDR + 0x800))

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

/* Whether this title can be made the homebrew title for the game's start:
 * Luma3DS, its loader on, its shared page laid out as known (its homebrew
 * title there the one it says). */
static bool CanTakeOver(void)
{
    s64 version = 0, hbTitle = 0;

    return R_SUCCEEDED(svcGetSystemInfo(&version, 0x10000, 0)) &&
           R_SUCCEEDED(svcGetSystemInfo(&hbTitle, 0x10000, 0x100)) && LUMA_SHARED_CONFIG->useHbldr &&
           LUMA_SHARED_CONFIG->hbldr3dsxTid == (u64)hbTitle;
}

/* argv: the .3dsx's path (as the Homebrew Launcher gives it), where it came
 * from, and (taken over) the homebrew title to put back (0: none). */
static void MakeArgv(const char *program, u64 restore)
{
    char *p = (char *)&sArgv[1];
    char *end = (char *)sArgv + sizeof(sArgv);
    u64 self = 0;

    memset(sArgv, 0, sizeof(sArgv));
    APT_GetProgramID(&self);
    sArgv[0] = restore != 0 ? 3 : 2;
    p += snprintf(p, (size_t)(end - p), "%s", program) + 1;
    p += snprintf(p, (size_t)(end - p), "%s%016llX", FORWARDED_ARG, (unsigned long long)self) + 1;
    if (restore != 0) {
        snprintf(p, (size_t)(end - p), "%s%016llX", HB_TITLE_ARG, (unsigned long long)restore);
    }
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
    if (CanTakeOver()) {
        /* The game in this title's place (see the top) */
        u64 self = 0, previous = LUMA_SHARED_CONFIG->selectedHbldr3dsxTid;

        if (R_SUCCEEDED(APT_GetProgramID(&self)) && self != 0) {
            MakeArgv(program, previous != 0 ? previous : LUMA_SHARED_CONFIG->hbldr3dsxTid);
            if (R_SUCCEEDED(HbldrSetTarget(program + 5)) && R_SUCCEEDED(HbldrSetArgv(sArgv, sizeof(sArgv)))) {
                svcCloseHandle(sHbldr);
                LUMA_SHARED_CONFIG->selectedHbldr3dsxTid = self;
                aptSetChainloader(self, MEDIATYPE_SD);
                return 0;
            }
        }
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
    MakeArgv(program, 0);
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
