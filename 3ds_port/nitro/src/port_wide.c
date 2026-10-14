/*
 * Where the game's windows are, for the 3DS remaster's wider upper screen
 * (include/nitro_port_wide.h). The game's thread adds and removes them;
 * the drawing threads read them: a small table under a spin lock.
 */
#include <nitro_port_wide.h>
#include <nitro_port_render2d.h>
#include <string.h>

#define MAX_TRACKED 96

typedef struct Tracked {
    const void *window;
    uint8_t bgLayer, left, top, width, height;
    int8_t anchor;
    uint8_t anchorWidth;   /* the tiles that move (0: all of the window's) */
} Tracked;

static Tracked sTable[MAX_TRACKED];
static int sCount;
static volatile int sLock;
static volatile uint32_t sSerial;

static void lock(void)
{
    while (__atomic_test_and_set(&sLock, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock(void)
{
    __atomic_clear(&sLock, __ATOMIC_RELEASE);
}

/* The game's Window struct (bg_window.h): a BgConfig pointer, then the BG
 * layer and the rectangle, a byte each. Read back to tell whether the
 * window is still the one added (the game sometimes reuses or frees a
 * window's struct without removing it). */
static int still_there(const Tracked *t)
{
    const uint8_t *w = (const uint8_t *)t->window + sizeof(void *);

    return w[0] == t->bgLayer && w[1] == t->left && w[2] == t->top && w[3] == t->width &&
           w[4] == t->height;
}

void NitroPort_WindowAdded(const void *window, int bgLayer, int left, int top, int width, int height)
{
    int i, slot = -1;

    if (bgLayer < 0 || bgLayer > 7 || width <= 0 || height <= 0) {
        return;
    }
    lock();
    for (i = 0; i < sCount; i++) {
        if (sTable[i].window == window) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* A free slot: a new one, or one whose window has gone. */
        if (sCount < MAX_TRACKED) {
            slot = sCount++;
        } else {
            for (i = 0; i < sCount; i++) {
                if (!still_there(&sTable[i])) {
                    slot = i;
                    break;
                }
            }
        }
    }
    if (slot >= 0) {
        sTable[slot].window = window;
        sTable[slot].bgLayer = (uint8_t)bgLayer;
        sTable[slot].left = (uint8_t)left;
        sTable[slot].top = (uint8_t)top;
        sTable[slot].width = (uint8_t)width;
        sTable[slot].height = (uint8_t)height;
        sTable[slot].anchor = 0;
        sTable[slot].anchorWidth = 0;
    }
    sSerial++;
    unlock();
}

void NitroPort_WindowAnchor(const void *window, int side, int widthTiles)
{
    int i;

    lock();
    for (i = 0; i < sCount; i++) {
        if (sTable[i].window == window) {
            sTable[i].anchor = (int8_t)(side < 0 ? -1 : side > 0 ? 1 : 0);
            sTable[i].anchorWidth = (uint8_t)(widthTiles > 0 && widthTiles < sTable[i].width ? widthTiles : 0);
            break;
        }
    }
    sSerial++;
    unlock();
}

void NitroPort_WindowRemoved(const void *window)
{
    int i;

    lock();
    for (i = 0; i < sCount; i++) {
        if (sTable[i].window == window) {
            sTable[i] = sTable[--sCount];
            break;
        }
    }
    sSerial++;
    unlock();
}

int NitroPort_WideWindows(int engine, int bg, NitroWideWindow *out, int max)
{
    int i, n = 0;
    uint8_t layer = (uint8_t)((engine ? 4 : 0) + bg);

    lock();
    for (i = 0; i < sCount && n < max; i++) {
        const Tracked *t = &sTable[i];

        if (t->bgLayer != layer || t->anchor == 0 || !still_there(t)) {
            continue;
        }
        out[n].left = t->left;
        out[n].top = t->top;
        out[n].width = t->anchorWidth ? t->anchorWidth : t->width;
        out[n].height = t->height;
        out[n].anchor = t->anchor;
        n++;
    }
    unlock();
    return n;
}

static int sForcePictures = -1;

void NitroPort_WideForcePictures(int mode)
{
    sForcePictures = mode;
}

int NitroPort_WidePictures(void)
{
    if (sForcePictures >= 0) {
        return sForcePictures;
    }
    /* The screens made of pictures rather than the game's UI: the opening
     * movie and the title (game_opening), the opening cutscene (game_start),
     * the credits, the cutscenes. */
    return NitroPort_OverlayLoadedNamed("game_opening") || NitroPort_OverlayLoadedNamed("game_start") ||
           NitroPort_OverlayLoadedNamed("end_credits") || NitroPort_OverlayLoadedNamed("cutscenes");
}

uint32_t NitroPort_WideWindowsSerial(void)
{
    return sSerial;
}

/* ------------------------------------------------------- the layout */

static int sWideExtra;

void NitroPort_SetWideScreen(int extra)
{
    sWideExtra = extra < 0 ? 0 : extra > 32 ? 32 : extra;
}

int NitroPort_WideScreenExtra(void)
{
    return sWideExtra;
}

/*
 * The rules. The DS's picture in the middle, as the DS lays it out (the
 * game's windows, menus and sprites keep their places: moved apart, they
 * came apart). Beside it, each BG goes on without anything stretched: a
 * line whose edge is one colour goes on in it (a plain border, a fade's
 * black, a window's frame); on the screens made of pictures (the opening
 * movie, the title, the opening cutscene: NitroPort_WidePictures) a BG
 * whose picture reaches the edge is mirrored at it, the picture folding
 * over on itself (one that stops short of the edge, the title's logo, say,
 * isn't); elsewhere (the game's UI: menus) a BG shows nothing beside the
 * DS's screen (the backdrop, or the 3D). The windows the
 * game anchors (the place name sign, patch 0014) move to the screen's
 * edge; the battle's health boxes the game moves itself (patch 0013,
 * NitroPort_WideScreenExtra). The battle's background (engine A's BG3) is
 * a picture wider than the DS's screen (a 512-pixel map: the trainer's
 * slide-in scrolls across it), so beyond the DS's edges it shows its own
 * art.
 */
void NitroPort_WideLayout(int engine, struct NitroRender2DSplit *split)
{
    int bg;

    split->width = 256 + 2 * sWideExtra;
    split->origin = sWideExtra;
    for (bg = 0; bg < 4; bg++) {
        split->clip[bg] = 1;
    }
    if (engine == 0 && NitroPort_OverlayLoadedNamed("battle")) {
        split->clip[3] = 0;
    }
    split->anchorWindows = sWideExtra > 0;
}
