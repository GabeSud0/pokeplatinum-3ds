/*
 * Where the game's windows are, for the 3DS remaster's wider upper screen
 * (include/nitro_port_wide.h), and the splits and parts of a screen laid
 * out anew.
 * The game's thread adds and removes them; the drawing threads read them:
 * small tables under a spin lock.
 */
#include <nitro_port_wide.h>
#include <nitro_port_render2d.h>
#include <nitro_port_lowermenu.h>
#include <string.h>

#define MAX_TRACKED 96

typedef struct Tracked {
    const void *window;
    uint8_t bgLayer, left, top, width, height;
    int8_t anchor;
    uint8_t anchorWidth;   /* the tiles that move (0: all of the window's) */
    uint8_t frame;         /* NitroPort_WindowFramed: 0 none, 1 a menu's, 2 a text box's */
    uint8_t ghost;         /* removed with its text box's frame still drawn (window NULL) */
    uint8_t indicator;     /* NitroPort_WindowIndicator */
    uint32_t overlays;     /* (a ghost: the overlays' serial then; gone when they change) */
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
    const uint8_t *w;

    if (t->ghost) {
        return t->overlays == NitroPort_OverlaySerial();
    }
    w = (const uint8_t *)t->window + sizeof(void *);

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
    /* (a text box removed with its frame left drawn: gone once another
     * window comes over it, the same one again most often) */
    for (i = 0; i < sCount; i++) {
        Tracked *g = &sTable[i];

        if (g->ghost && g->bgLayer == bgLayer && left < g->left + g->width + 3 && g->left - 2 < left + width &&
            top < g->top + g->height + 1 && g->top - 1 < top + height) {
            sTable[i--] = sTable[--sCount];
        }
    }
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
        sTable[slot].frame = 0;
        sTable[slot].ghost = 0;
        sTable[slot].indicator = 0;
    }
    sSerial++;
    unlock();
}

void NitroPort_WindowFramed(const void *window, int frame)
{
    int i;

    lock();
    for (i = 0; i < sCount; i++) {
        if (sTable[i].window == window) {
            sTable[i].frame = (uint8_t)(frame == 1 || frame == 2 ? frame : 0);
            break;
        }
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

void NitroPort_WindowIndicator(const void *window, int on)
{
    int i;

    lock();
    for (i = 0; i < sCount; i++) {
        if (sTable[i].window == window && !sTable[i].ghost) {
            if (sTable[i].indicator != (on != 0)) {
                sTable[i].indicator = on != 0;
                sSerial++;
            }
            break;
        }
    }
    unlock();
}

void NitroPort_WindowRemoved(const void *window)
{
    int i;

    lock();
    for (i = 0; i < sCount; i++) {
        if (sTable[i].window == window && !sTable[i].ghost) {
            if (sTable[i].frame == 2) {
                /* Its frame still drawn (the game took the window away
                 * without erasing it: Rowan's talk between his messages,
                 * with a choice up): still widened, till another window
                 * comes over it or the overlays change (another screen). */
                sTable[i].window = NULL;
                sTable[i].ghost = 1;
                sTable[i].overlays = NitroPort_OverlaySerial();
            } else {
                sTable[i] = sTable[--sCount];
            }
            break;
        }
    }
    sSerial++;
    unlock();
}

void NitroPort_WideBgReset(int bgLayer)
{
    int i;

    lock();
    for (i = 0; i < sCount; i++) {
        if (sTable[i].ghost && sTable[i].bgLayer == bgLayer) {
            sTable[i--] = sTable[--sCount];
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
        NitroWideWindow *w = &out[n];

        if (t->bgLayer != layer || (t->anchor == 0 && t->frame == 0) || !still_there(t)) {
            continue;
        }
        memset(w, 0, sizeof(*w));
        if (t->anchor != 0) {
            /* (the game says where it goes: the place name sign) */
            w->left = t->left;
            w->top = t->top;
            w->width = t->anchorWidth ? t->anchorWidth : t->width;
            w->height = t->height;
            w->anchor = t->anchor;
            n++;
            continue;
        }
        {
            /* With its frame: a menu's a tile all round, a text box's 2
             * tiles left, 3 right, a tile above and below. */
            int fl = t->frame == 2 ? 2 : 1, fr = t->frame == 2 ? 3 : 1;
            int left = t->left - fl, right = t->left + t->width + fr;
            int byLeft = left <= 2, byRight = right >= 30;

            if (left < 0 || right > 32 || t->top < 1 || t->top + t->height + 1 > 32) {
                continue;   /* (a frame past the DS's screen: not one of these) */
            }
            w->left = (uint8_t)left;
            w->top = (uint8_t)(t->top - 1);
            w->width = (uint8_t)(right - left);
            w->height = (uint8_t)(t->height + 2);
            if (t->frame == 2 && byLeft && byRight) {
                w->widen = 1;
                w->frameLeft = (uint8_t)fl;
                w->frameRight = (uint8_t)fr;
                w->indicator = t->indicator && t->width > 3;
            } else if (byRight && !byLeft) {
                w->anchor = 1;
            } else if (byLeft && !byRight) {
                w->anchor = -1;
            } else {
                continue;   /* (in the middle, or across the screen: where it is) */
            }
            n++;
        }
    }
    unlock();
    return n;
}

static int sForcePictures = -1;

void NitroPort_WideForcePictures(int mode)
{
    sForcePictures = mode;
}

int NitroPort_WideCells(void)
{
    /* (cached as NitroPort_WidePictures is) */
    static uint32_t cache = 0xFFFFFFFFu;
    uint32_t serial = NitroPort_OverlaySerial() & 0x7FFFFFFFu, c = __atomic_load_n(&cache, __ATOMIC_RELAXED);
    int cells;

    if (sForcePictures >= 0) {
        return sForcePictures || NitroPort_OverlayLoadedNamed("battle");
    }
    if (c != 0xFFFFFFFFu && (c >> 1) == serial) {
        return (int)(c & 1);
    }
    /* The pictures' screens, and the battle: its trainers slide off and
     * its balls fly beside the DS's screen (the player went a frame early,
     * half a sprite still showing, 2026-10-26). */
    cells = NitroPort_WidePictures() || NitroPort_OverlayLoadedNamed("battle");
    __atomic_store_n(&cache, (serial << 1) | (uint32_t)cells, __ATOMIC_RELAXED);
    return cells;
}

int NitroPort_WidePictures(void)
{
    /* The answer, worked out again when an overlay is loaded or unloaded:
     * the overlay's serial number and the answer in one word, so another
     * thread reads both at once (the game asks for each sprite it places). */
    static uint32_t cache = 0xFFFFFFFFu;
    uint32_t serial = NitroPort_OverlaySerial() & 0x7FFFFFFFu, c = __atomic_load_n(&cache, __ATOMIC_RELAXED);
    int pictures;

    if (sForcePictures >= 0) {
        return sForcePictures;
    }
    if (c != 0xFFFFFFFFu && (c >> 1) == serial) {
        return (int)(c & 1);
    }
    /* The screens made of pictures rather than the game's UI: the opening
     * movie and the title (game_opening), the opening cutscene (game_start),
     * the credits, the cutscenes. */
    pictures = NitroPort_OverlayLoadedNamed("game_opening") || NitroPort_OverlayLoadedNamed("game_start") ||
               NitroPort_OverlayLoadedNamed("end_credits") || NitroPort_OverlayLoadedNamed("cutscenes");
    __atomic_store_n(&cache, (serial << 1) | (uint32_t)pictures, __ATOMIC_RELAXED);
    return pictures;
}

uint32_t NitroPort_WideWindowsSerial(void)
{
    return sSerial;
}

/* ------------------------------------------- a screen laid out anew */

static NitroWideSplit sSplits[2][NITRO_WIDE_MAX_SPLITS];
static int sSplitCount[2];

void NitroPort_WideSplitScreen(int engine, const short *splits, int count)
{
    int e = engine ? 1 : 0, i, n = 0;

    if (splits == NULL || count < 0) {
        count = 0;
    }
    lock();
    for (i = 0; i < count && n < NITRO_WIDE_MAX_SPLITS; i++) {
        const short *row = splits + i * 7;
        NitroWideSplit *s = &sSplits[e][n];

        if (row[1] < 0 || row[2] <= row[1] || (row[0] & 0x0F) == 0) {
            continue;   /* (no lines, or no BGs) */
        }
        s->bgs = (uint8_t)(row[0] & 0x0F);
        s->top = (uint16_t)row[1];
        s->bottom = (uint16_t)row[2];
        s->x = row[3];
        s->left = row[4];
        s->right = row[5];
        s->fill = (uint8_t)(row[6] != 0);
        n++;
    }
    sSplitCount[e] = n;
    sSerial++;
    unlock();
}

int NitroPort_WideSplits(int engine, NitroWideSplit *out)
{
    int e = engine ? 1 : 0, n;

    lock();
    n = sSplitCount[e];
    memcpy(out, sSplits[e], (size_t)n * sizeof(*out));
    unlock();
    return n;
}

static NitroWidePart sParts[2][NITRO_WIDE_MAX_PARTS];
static int sPartCount[2];

void NitroPort_WideScreenParts(int engine, const short *rows, int count)
{
    NitroWidePart got[NITRO_WIDE_MAX_PARTS];
    int e = engine ? 1 : 0, i, n = 0;

    if (rows == NULL || count < 0) {
        count = 0;
    }
    memset(got, 0, sizeof(got));
    lock();
    for (i = 0; i < count && n < NITRO_WIDE_MAX_PARTS; i++) {
        const short *row = rows + i * 8;
        NitroWidePart *p = &got[n];

        if (row[0] < NITRO_WIDE_MOVE || row[0] > NITRO_WIDE_NO_ART) {
            continue;
        }
        if (row[0] == NITRO_WIDE_CLEAR) {
            if ((row[1] & 0x0F) == 0 || row[6] < 0 || row[6] > 15 || row[7] == 0) {
                continue;   /* (no BG, no such palette, no colours) */
            }
        } else if (row[0] == NITRO_WIDE_PICTURE) {
            if ((row[1] & 0x0F) == 0 || row[7] <= 0 || row[7] >= NITRO_WIDE_PICTURES) {
                continue;   /* (no BG, or no such picture) */
            }
        } else if (row[0] == NITRO_WIDE_NO_ART) {
            if ((row[1] & 0x0F) == 0) {
                continue;   /* (no BGs) */
            }
        } else if (row[0] != NITRO_WIDE_SHIFT3D && (row[3] <= row[2] || row[5] <= row[4])) {
            continue;   /* (no lines, or no columns) */
        }
        if ((row[0] == NITRO_WIDE_MOVE || row[0] == NITRO_WIDE_FILL || row[0] == NITRO_WIDE_SHOW) && (row[1] & 0x0F) == 0) {
            continue;   /* (no BGs) */
        }
        p->kind = (uint8_t)row[0];
        p->bgs = (uint8_t)(row[1] & (0x0F | NITRO_WIDE_SCREEN_COLUMNS));
        p->top = row[2];
        p->bottom = row[3];
        p->x0 = row[4];
        p->x1 = row[5];
        p->dx = row[6];
        p->col = row[7];
        n++;
    }
    /* (the same layout again, a frame of an animation that hasn't moved:
     * nothing to draw again for it) */
    if (n != sPartCount[e] || memcmp(got, sParts[e], (size_t)n * sizeof(got[0])) != 0) {
        memcpy(sParts[e], got, (size_t)n * sizeof(got[0]));
        sPartCount[e] = n;
        sSerial++;
    }
    unlock();
}

static const char *const sPictureNames[NITRO_WIDE_PICTURES] = {
    NULL,
    "pc_boxes_divider",
    "pc_boxes_ball",
    "party_panel",
    "party_ball",
    "party_light_off",
    "party_light_red",
    "party_light_blue",
    "party_light_green",
    "party_light_amber",
    "party_light_white",
};

const char *NitroPort_WidePictureName(int picture)
{
    return picture > 0 && picture < NITRO_WIDE_PICTURES ? sPictureNames[picture] : NULL;
}

int NitroPort_WideParts(int engine, NitroWidePart *out)
{
    int e = engine ? 1 : 0, n;

    lock();
    n = sPartCount[e];
    memcpy(out, sParts[e], (size_t)n * sizeof(*out));
    unlock();
    return n;
}

/* ------------------------------------------------------- the layout */

static int sWideExtra;
static unsigned sRepeating;   /* (NitroPort_RepeatingBg: engine A's BGs, a bit each) */

void NitroPort_RepeatingBg(int bg, int on)
{
    if (bg < 0 || bg > 3) {
        return;
    }
    lock();
    sRepeating = on ? sRepeating | (1u << bg) : sRepeating & ~(1u << bg);
    unlock();
}

int NitroPort_RepeatingBgs(void)
{
    unsigned bgs;
    int laidOut;

    if (!NitroPort_OverlayLoadedNamed("overlay5") || NitroPort_OverlayLoadedNamed("battle")) {
        return 0;
    }
    lock();
    bgs = sRepeating;
    laidOut = sPartCount[0] > 0;
    unlock();
    return laidOut ? 0 : (int)bgs;
}

void NitroPort_SetWideScreen(int extra)
{
    sWideExtra = extra < 0 ? 0 : extra > 32 ? 32 : extra;
}

static int sPixelFieldOn;   /* (NitroPort_SetPixelField) */

void NitroPort_SetPixelField(int on)
{
    __atomic_store_n(&sPixelFieldOn, on != 0, __ATOMIC_RELEASE);
}

int NitroPort_PixelField(void)
{
    /* (the overlays' part kept by their serial: the game's code reads this
     * for every cell it draws, NitroPort_WideScreenExtra) */
    static uint32_t cache = 0xFFFFFFFFu;
    uint32_t serial = NitroPort_OverlaySerial() & 0x7FFFFFFFu, c;
    int field, laidOut;

    if (!__atomic_load_n(&sPixelFieldOn, __ATOMIC_ACQUIRE) || sWideExtra <= 0) {
        return 0;
    }
    c = __atomic_load_n(&cache, __ATOMIC_RELAXED);
    if (c != 0xFFFFFFFFu && (c >> 1) == serial) {
        field = (int)(c & 1);
    } else {
        field = NitroPort_OverlayLoadedNamed("overlay5") && !NitroPort_OverlayLoadedNamed("battle") &&
                !NitroPort_WidePictures();
        __atomic_store_n(&cache, (serial << 1) | (uint32_t)field, __ATOMIC_RELAXED);
    }
    if (!field || NitroPort_LowerMenuPanelShowsUpper()) {
        return 0;   /* (the lower menu's panel showing the upper screen: at the DS's size there) */
    }
    lock();
    laidOut = sPartCount[0] > 0;
    unlock();
    return !laidOut;
}

int NitroPort_WideScreenExtra(void)
{
    /* (the field at the DS's pixel size: the 3DS's whole 400 pixels) */
    return sWideExtra > 0 && NitroPort_PixelField() ? NITRO_PIXEL_FIELD_EXTRA : sWideExtra;
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
 * isn't); elsewhere (the game's UI: menus) a BG's lines with something
 * at the DS's edge that isn't plain carry on the plain lines next to them
 * (a menu's background goes on behind its panels; port_render2d.c,
 * DecideBandFills), and a BG with nothing there shows nothing (the
 * backdrop, or the 3D). The windows the game anchors (the place name
 * sign, patch 0014) move to the screen's edge, and so do the framed ones
 * by the DS's edges (menus, patch 0015), their sprites with them; a
 * text box across the screen has its frame widened to the edges, its text
 * at the left. A screen redrawn for the wider screen the game lays out in
 * parts that move apart (the trainer card, patch 0016:
 * NitroPort_WideSplitScreen; the summary, the party, the bag, the boxes,
 * the Pokédex's list, patches 0017-0022: NitroPort_WideScreenParts). The
 * battle's health boxes the game moves itself (patch 0013,
 * NitroPort_WideScreenExtra). The battle's background (engine A's BG3) is
 * a 512-pixel map (the trainer's slide-in scrolls across it), so beyond
 * the DS's edges it shows its own art where the map has some (the
 * player's platform's left part), never the map's mirrored half
 * (TextBgSegments, port_render2d.c).
 */
static volatile int sFieldEffect;

void NitroPort_SetFieldEffect(int on)
{
    __atomic_store_n(&sFieldEffect, on != 0, __ATOMIC_RELEASE);
}

int NitroPort_FieldEffect(void)
{
    return __atomic_load_n(&sFieldEffect, __ATOMIC_ACQUIRE);
}

void NitroPort_WideLayout(int engine, struct NitroRender2DSplit *split)
{
    int bg, extra = sWideExtra;

    split->top = 0;
    split->stretch = 0;
    if (engine == 0 && sWideExtra > 0 && NitroPort_FieldEffect() && NitroPort_OverlayLoadedNamed("overlay5") &&
        !NitroPort_OverlayLoadedNamed("battle")) {
        /* (a battle's start: the DS's picture, stretched by the compositor
         * over the 3D at the screen's size) */
        split->width = 256;
        split->origin = 0;
        split->stretch = 1;
        for (bg = 0; bg < 4; bg++) {
            split->clip[bg] = 1;
        }
        split->anchorWindows = 0;
        return;
    }
    if (engine == 0 && NitroPort_PixelField()) {
        /* (the field at the DS's pixel size on the 3DS's whole screen: the
         * DS's picture in the middle of 400 x 240) */
        extra = NITRO_PIXEL_FIELD_EXTRA;
        split->top = NITRO_PIXEL_FIELD_TOP;
        split->height = NITRO_PIXEL_FIELD_H;
    }
    split->width = 256 + 2 * extra;
    split->origin = extra;
    for (bg = 0; bg < 4; bg++) {
        split->clip[bg] = 1;
    }
    if (engine == 0 && NitroPort_OverlayLoadedNamed("battle")) {
        int laidOut;

        /* (not a screen laid out in parts while the battle's loaded: the
         * evolution after a battle, its background BG3 too, with a
         * hand-drawn picture: 2026-10-26) */
        lock();
        laidOut = sPartCount[0] > 0;
        unlock();
        if (!laidOut) {
            split->clip[3] = 0;
        }
    } else if (engine == 0) {
        /* (the field: its weather's BG a repeating pattern, NitroPort_RepeatingBg) */
        int bgs = NitroPort_RepeatingBgs();

        for (bg = 0; bg < 4; bg++) {
            if ((bgs >> bg) & 1) {
                split->clip[bg] = 0;
            }
        }
    }
    split->anchorWindows = sWideExtra > 0;
}
