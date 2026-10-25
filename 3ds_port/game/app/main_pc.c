/*
 * The game on a PC, without a screen: for finding out how far it gets, and
 * where it stops, faster than on a 3DS.
 *
 *   pokeplatinum-pc <platinum3ds.pak> <manifest.json> [--save FILE]
 *                   [--frames N] [--wav FILE] [--stop-at-missing-file]
 *                   [--stall-seconds N]
 *                   [--screenshots DIR] [--screenshot-every N]
 *                   [--dump-3d FRAME]
 *                   [--press FRAME:BUTTONS,...] [--touch FRAME:X:Y,...]
 *                   [--control DIR] [--gx-thread | --gx-thread-skip]
 *                   [--gx-record FILE] [--wide]
 *   pokeplatinum-pc --check-overlays
 *
 * Runs the same game code as the 3DS program, with the same data pack, as
 * fast as the PC goes. The save is a raw 512 KB file, as emulators keep
 * them: --save FILE, or platinum.sav next to the data pack; it's made if
 * missing. Everything the port logs goes to stderr, each line
 * with the frame number. With --frames N it stops after N frames (exit
 * status 0); with --stop-at-missing-file, at the first file the game asks
 * for that the pack doesn't have (exit status 5). If the game crashes, the
 * crash is reported with a backtrace.
 *
 * --screenshots DIR saves what the two screens show (port_render2d.c) as
 * PNG files, the upper screen above the lower one, every 60 frames (one
 * second of game time; --screenshot-every N to change it), and once more
 * when the runner stops. --dump-3d FRAME captures the newest finished 3D
 * image as 3d_frame_<FRAME>.png in the current directory, preserving alpha.
 * --press presses buttons at given frames, since the
 * PC runner has none: "--press 8700:A,9000:START,9100:UP+B" holds each for
 * 6 frames (A B X Y L R START SELECT UP DOWN LEFT RIGHT). --touch touches
 * the lower screen the same way: "--touch 7000:120:80" (X 0-255, Y 0-191).
 *
 * If no frame comes for 5 seconds (--stall-seconds N to change it), the game
 * is stuck: waiting in a loop for something the port doesn't do yet. The
 * runner then prints where every thread is, as function names and file:line
 * (with addr2line, from binutils), and stops (exit status 6). The thread
 * that isn't waiting in the port's thread switching is the one that's stuck.
 *
 * --check-overlays loads every overlay with the game's overlay manager and
 * checks that each load resets the overlay's data (exit status 1 if not).
 * It needs no data pack.
 *
 * Build: make -C 3ds_port/game TARGET=host32 program (32-bit, as on the
 * 3DS). TARGET=host (64-bit) builds too, but Nintendo's graphics formats
 * hold 4-byte pointers, so there the game can't get far once it loads them.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <nitro.h>
#include <nitro_port.h>
#include <nitro_port_gx3d.h>
#include <nitro_port_gx3d_vertex.h>
#include <nitro_port_gx3d_texture.h>
#include <nitro_port_lowermenu.h>
#include <nitro_port_wide.h>
#include <nitro_port_wideart.h>

#include "platinum.h"

void NitroPort_SoundRender(int16_t *stereo, size_t frames);
void NitroPort_SoundPumpReplies(void);

#define WAV_SAMPLES 512u
static const char *sWavPath;
static FILE *sWav;
static pthread_t sSoundThread;
static volatile int sSoundRunning;
static int sSoundThreadCreated;
static uint32_t sWavBytes;
static uint64_t sWavNextFrameNs;

/* When recording WAV, the original game must not race through its 60 Hz
 * scenes faster than the audio clock. This only paces the game; the sound
 * driver still runs from the audio thread, never OnFrame. */
static void pace_game_for_wav(void)
{
    struct timespec now;
    uint64_t current, remaining;
    struct timespec sleepFor;

    if (!sWav) return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    current = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    if (!sWavNextFrameNs || current > sWavNextFrameNs + 100000000ull) {
        sWavNextFrameNs = current;
    } else {
        sWavNextFrameNs += 16666667ull;
    }
    if (current >= sWavNextFrameNs) return;
    remaining = sWavNextFrameNs - current;
    sleepFor.tv_sec = (time_t)(remaining / 1000000000ull);
    sleepFor.tv_nsec = (long)(remaining % 1000000000ull);
    nanosleep(&sleepFor, NULL);
}

static void wav_u16(FILE *f, uint16_t n)
{
    fputc(n & 255u, f);
    fputc((n >> 8) & 255u, f);
}
static void wav_u32(FILE *f, uint32_t n)
{
    wav_u16(f, (uint16_t)n);
    wav_u16(f, (uint16_t)(n >> 16));
}
static void wav_header(FILE *f, uint32_t bytes)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    wav_u32(f, bytes + 36u);
    fwrite("WAVEfmt ", 1, 8, f);
    wav_u32(f, 16);
    wav_u16(f, 1);      /* PCM */
    wav_u16(f, 2);      /* stereo */
    wav_u32(f, 32768);
    wav_u32(f, 32768u * 4u);
    wav_u16(f, 4);
    wav_u16(f, 16);
    fwrite("data", 1, 4, f);
    wav_u32(f, bytes);
}
static void *SoundThread(void *unused)
{
    int16_t samples[WAV_SAMPLES * 2u];
    struct timespec period = {0, 15625000}; /* 512 / 32768 s */
    (void)unused;
    while (__atomic_load_n(&sSoundRunning, __ATOMIC_ACQUIRE)) {
        NitroPort_SoundRender(samples, WAV_SAMPLES);
        if (sWav && sWavBytes <= UINT32_MAX - WAV_SAMPLES * 4u) {
            if (fwrite(samples, sizeof(samples), 1, sWav) == 1)
                sWavBytes += sizeof(samples);
        }
        nanosleep(&period, NULL);
    }
    return NULL;
}
static void StartSound(void)
{
    if (sWavPath) {
        sWav = fopen(sWavPath, "wb");
        if (sWav) wav_header(sWav, 0);
        else fprintf(stderr, "[audio] can't create WAV file: %s\n", sWavPath);
    }
    __atomic_store_n(&sSoundRunning, 1, __ATOMIC_RELEASE);
    if (pthread_create(&sSoundThread, NULL, SoundThread, NULL) == 0)
        sSoundThreadCreated = 1;
    else {
        sSoundRunning = 0;
        fprintf(stderr, "[audio] can't start ARM7 audio clock\n");
    }
}
static void StopSound(void)
{
    __atomic_store_n(&sSoundRunning, 0, __ATOMIC_RELEASE);
    if (sSoundThreadCreated) {
        pthread_join(sSoundThread, NULL);
        sSoundThreadCreated = 0;
    }
    if (sWav) {
        wav_header(sWav, sWavBytes);
        fflush(sWav);
        fclose(sWav);
        sWav = NULL;
    }
}

static volatile long sFrame;
static long sFrameLimit = -1;
static int sStopAtMissingFile;
static int sStallSeconds = 5;
static const char *sScreenshotDir;
static long sScreenshotEvery = 60;
static long sDump3DFrame = -1;

#define MAX_PRESSES 4096
static struct {
    long frame;
    uint16_t buttons;
} sPresses[MAX_PRESSES];
static int sPressCount;

/* --control DIR: the runner pauses and takes commands from DIR/cmd. */
static const char *sControlDir;
static long sRunUntil;
static uint16_t sLiveButtons;
static long sLiveButtonsUntil = -1;
static int sLiveTouchX, sLiveTouchY;
static long sLiveTouchUntil = -1;
static int sLiveTouchMenu;   /* (mtouch: at the 3DS's lower screen, through the menu) */

#define MAX_TOUCHES 4096
static struct {
    long frame;
    int x, y;
} sTouches[MAX_TOUCHES];
static int sTouchCount;

/* ------------------------------------------------------------ screenshots */

static uint32_t sCrcTable[256];

static uint32_t Crc(uint32_t crc, const unsigned char *p, size_t n)
{
    size_t i;

    if (sCrcTable[1] == 0) {
        uint32_t c, k;

        for (k = 0; k < 256; k++) {
            c = k;
            for (i = 0; i < 8; i++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            sCrcTable[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc = sCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static void Put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void Chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char head[8], tail[4];
    uint32_t crc;

    Put32(head, len);
    memcpy(head + 4, type, 4);
    crc = Crc(0, head + 4, 4);
    crc = Crc(crc, data, len);
    Put32(tail, crc);
    fwrite(head, 1, 8, f);
    fwrite(data, 1, len, f);
    fwrite(tail, 1, 4, f);
}

/* A PNG of RGB rows (each a filter byte, 0, then 3 bytes a pixel; freed
 * here), uncompressed (deflate's stored blocks), so no image library is
 * needed. */
static int WritePngRows(const char *path, unsigned char *rows, int w, int h)
{
    size_t raw = (size_t)h * (size_t)(1 + w * 3);
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    unsigned char *z = malloc(zlen);
    unsigned char ihdr[13];
    uint32_t a = 1, b = 0;
    size_t i, pos = 0, done = 0;
    FILE *f;

    if (rows == NULL || z == NULL) {
        free(rows);
        free(z);
        return -1;
    }
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    while (done < raw) {
        size_t n = raw - done < 65535 ? raw - done : 65535;

        z[pos++] = done + n == raw ? 1 : 0;
        z[pos++] = (unsigned char)n;
        z[pos++] = (unsigned char)(n >> 8);
        z[pos++] = (unsigned char)~n;
        z[pos++] = (unsigned char)(~n >> 8);
        memcpy(z + pos, rows + done, n);
        pos += n;
        done += n;
    }
    for (i = 0; i < raw; i++) {
        a = (a + rows[i]) % 65521;
        b = (b + a) % 65521;
    }
    Put32(z + pos, (b << 16) | a);
    pos += 4;

    f = fopen(path, "wb");
    if (f == NULL) {
        free(rows);
        free(z);
        return -1;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    Put32(ihdr, (uint32_t)w);
    Put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* bits per channel */
    ihdr[9] = 2;    /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    Chunk(f, "IHDR", ihdr, 13);
    Chunk(f, "IDAT", z, (uint32_t)pos);
    Chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(rows);
    free(z);
    return 0;
}

/* A PNG of DS colours (15-bit). */
static int WritePng(const char *path, const uint16_t *pixels, int w, int h)
{
    unsigned char *rows = malloc((size_t)h * (size_t)(1 + w * 3));
    int x, y;

    if (rows == NULL) {
        return -1;
    }
    for (y = 0; y < h; y++) {
        unsigned char *r = rows + (size_t)y * (size_t)(1 + w * 3);

        *r++ = 0;   /* filter: none */
        for (x = 0; x < w; x++) {
            uint16_t c = pixels[y * w + x];

            *r++ = (unsigned char)(((c & 31) << 3) | ((c & 31) >> 2));
            *r++ = (unsigned char)((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
            *r++ = (unsigned char)((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
        }
    }
    return WritePngRows(path, rows, w, h);
}

/* A PNG of (r << 24) | (g << 16) | (b << 8) | a texels (the alpha left out). */
static int WritePngRgba(const char *path, const uint32_t *pixels, int w, int h)
{
    unsigned char *rows = malloc((size_t)h * (size_t)(1 + w * 3));
    int x, y;

    if (rows == NULL) {
        return -1;
    }
    for (y = 0; y < h; y++) {
        unsigned char *r = rows + (size_t)y * (size_t)(1 + w * 3);

        *r++ = 0;
        for (x = 0; x < w; x++) {
            uint32_t c = pixels[y * w + x];

            *r++ = (unsigned char)(c >> 24);
            *r++ = (unsigned char)(c >> 16);
            *r++ = (unsigned char)(c >> 8);
        }
    }
    return WritePngRows(path, rows, w, h);
}

/* As the 3DS does (main_3ds.c, DrawFrame): the display state taken when
 * the game's V-blank work is done, which the remaster's picture is drawn
 * from (the VRAM as it is: the game changes it only in that work). */
static int sStateTaken;

void NitroPort_VBlankWorkDone(void)
{
    NitroPort_Render2DTakeState(0);
    NitroPort_Render2DTakeState(1);
    sStateTaken = 1;
}

/* The 3DS remaster's picture of the screens, put together on the CPU the
 * way the 3DS's GPU does it (port_compose_pica.c), at the DS's scale: the
 * upper screen 320 pixels wide with --wide (the 3DS's 400 at 1.25), the
 * lower one 256, centred under it. For checking layouts on the PC: the 3D
 * comes from the reference rasterizer (with --wide, its 256 pixels show
 * the wider view and are stretched to the 320), its translucency is only
 * approximate, and there's no stereo or upscaling. */
static void RemasterRender(uint16_t *out);

/* (the remaster) The upper screen kept while an app is in the lower menu's
 * panel (NitroPort_LowerMenuHoldUpper), as the compositor keeps it: rshot
 * shows it. Kept as it is when the game asks; let go once the game's
 * picture there is as bright as what's kept. Each frame. */
static uint16_t sKeptTop[320 * 192];
static int sTopKept, sTopKeptBright;
/* The field's picture (NITRO_LOWERMENU_HOLD_FIELD): what was kept at full
 * brightness, with the game's forgetting's count then; shown now. */
static uint16_t sFieldTop[320 * 192];
static int sFieldValid, sTopShowsField;
static uint32_t sFieldSerial;

static void KeepUpperScreen(void)
{
    extern unsigned char NitroPort_IoReg[];
    const unsigned char *io = NitroPort_IoReg + (NitroPort_EngineAOnTop() ? 0 : 0x1000);
    int wish = NitroPort_LowerMenuUpperHeld();
    int bright = NitroPort_LowerMenuBrightness((unsigned)(io[0x6C] | (io[0x6D] << 8)));

    if (!sTopKept && wish > 0) {
        static uint16_t both[320 * 384];

        RemasterRender(both);
        memcpy(sKeptTop, both, sizeof(sKeptTop));
        sTopKept = 1;
        /* (kept mid-fade: as the end of a fade nearest it) */
        sTopKeptBright = bright <= -8 ? -16 : bright >= 8 ? 16 : 0;
        sTopShowsField = 0;
        if (sTopKeptBright == 0) {
            memcpy(sFieldTop, sKeptTop, sizeof(sFieldTop));
            sFieldSerial = NitroPort_LowerMenuFieldSerial();
            sFieldValid = 1;
            sTopShowsField = 1;
        }
    } else if (sTopKept && (wish < 0 || (wish == 0 && bright == sTopKeptBright))) {
        sTopKept = 0;   /* (wish < 0: let go now) */
        sTopShowsField = 0;
    }
    /* (kept showing the field's picture: an app in the panel after one
     * that took both screens; as bright as the field from then on) */
    if (sTopKept && wish == NITRO_LOWERMENU_HOLD_FIELD && !sTopShowsField && sFieldValid &&
        sFieldSerial == NitroPort_LowerMenuFieldSerial()) {
        memcpy(sKeptTop, sFieldTop, sizeof(sKeptTop));
        sTopKeptBright = 0;
        sTopShowsField = 1;
    }
}

static int RemasterShot(const char *path)
{
    static uint16_t out[320 * 384];

    RemasterRender(out);
    if (sTopKept) {
        memcpy(out, sKeptTop, sizeof(sKeptTop));
    }
    return WritePng(path, out, 320, 384);
}

static void RemasterRender(uint16_t *out)
{
    static uint16_t under[320 * 192], over[320 * 192];
    static uint8_t overAlpha[320 * 192], alpha3d[256 * 192];
    static uint16_t image3d[256 * 192];
    int aOnTop = NitroPort_EngineAOnTop() ? 1 : 0;
    int screen;

    memset(out, 0, 320 * 384 * sizeof(out[0]));
    for (screen = 0; screen < 2; screen++) {
        int engine = (screen == 0) == aOnTop ? 0 : 1;
        NitroRender2DSplit split;
        int width, x, y, left;

        memset(&split, 0, sizeof(split));
        split.over = over;
        split.overAlpha = overAlpha;
        split.windowsToEdge = 1;   /* (as the 3DS remaster has it) */
        split.snapshot = sStateTaken;
        if (screen == 0) {
            NitroPort_WideLayout(engine, &split);
        }
        NitroPort_Render2DSplit(engine, under, &split);
        width = split.width > 256 ? split.width : 256;
        left = (320 - width) / 2;
        if (split.has3d) {
            /* the newest 3D image, as the 2D renderer would take it */
            const uint16_t *img = NitroPort_Gx3dAcquireImage();
            const uint8_t *alpha = img != NULL ? NitroPort_Gx3dImageAlpha(img) : NULL;

            memset(image3d, 0, sizeof(image3d));
            if (img != NULL) {
                memcpy(image3d, img, sizeof(image3d));
                if (alpha != NULL) {
                    memcpy(alpha3d, alpha, sizeof(alpha3d));
                } else {
                    memset(alpha3d, 31, sizeof(alpha3d));
                }
                NitroPort_Gx3dReleaseImage(img);
            }
        }
        for (y = 0; y < 192; y++) {
            for (x = 0; x < width; x++) {
                uint16_t c = under[y * width + x];

                if (split.has3d) {
                    /* where the 3D's x is: the DS's 256 at origin, or (wide)
                     * the whole canvas */
                    int scroll3d = split.scroll3dLines ? split.scroll3dLine[y] : split.scroll3d;
                    int scroll = scroll3d >= 256 ? scroll3d - 512 : scroll3d;
                    int ix = NitroPort_WideScreenExtra() && screen == 0 && width > 256
                                 ? (x + scroll) * 256 / width
                                 : x - split.origin + scroll;

                    if (ix >= 0 && ix < 256 && (image3d[y * 256 + ix] & 0x8000)) {
                        uint16_t p = image3d[y * 256 + ix] & 0x7FFF;
                        int a = alpha3d[y * 256 + ix], k;
                        uint16_t m = 0;

                        for (k = 0; k < 15; k += 5) {
                            int pc = (p >> k) & 31, uc = (c >> k) & 31;

                            m |= (uint16_t)(((pc * a + uc * (31 - a)) / 31) << k);
                        }
                        c = m;
                    }
                }
                if (split.has3d && (over[y * width + x] & 0x8000)) {
                    int a = overAlpha[y * width + x], k;
                    uint16_t o = over[y * width + x] & 0x7FFF, m = 0;

                    for (k = 0; k < 15; k += 5) {
                        m |= (uint16_t)(((((o >> k) & 31) * a + ((c >> k) & 31) * (16 - a)) / 16) << k);
                    }
                    c = m;
                }
                {
                    int mode = (split.masterBright >> 14) & 3, f = split.masterBright & 31, k;
                    uint16_t m = 0;

                    if (f > 16) {
                        f = 16;
                    }
                    if (f && (mode == 1 || mode == 2)) {
                        for (k = 0; k < 15; k += 5) {
                            int v = (c >> k) & 31;

                            v = mode == 1 ? v + (31 - v) * f / 16 : v - v * f / 16;
                            m |= (uint16_t)(v << k);
                        }
                        c = m;
                    }
                }
                out[(screen * 192 + y) * 320 + left + x] = c;
            }
        }
    }
}

/* The 3DS remaster's lower screen as the menu (port_lowermenu.c), at the
 * 3DS's resolution, put together as the compositor does: the menu's
 * picture; in its panel the map's picture, or the DS's lower screen at
 * 15/16 (its top row carried on above it, its bottom row below), sampled as
 * the GPU samples its texture of 2 x 2 texels a pixel (bilinear); black
 * over the panel as dark as its fade; the master brightness over it all. */
static void MenuRender(uint32_t *out);

/* The panel's picture with its 3D, if an app shows some there (below). */
static void CanvasWith3D(uint16_t *canvas, int width, const NitroRender2DSplit *split, const uint16_t *over,
                         const uint8_t *overAlpha);

/* The lower screen's engine as the panel shows it, at the DS's pixels (256
 * wide; 248 lines when it's tall, else 192): a tall layout checked against
 * its mock-up, pixel for pixel. */
static int CanvasShot(const char *path)
{
    static uint16_t canvas[320 * NITRO_LOWERMENU_TALL_LINES], over[320 * 192];
    static uint8_t overAlpha[320 * 192];
    static uint16_t out[256 * NITRO_LOWERMENU_TALL_LINES];
    NitroRender2DSplit split;
    int lines = NitroPort_LowerMenuTall() ? NITRO_LOWERMENU_TALL_LINES : 192, width, y;

    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    split.windowsToEdge = 1;
    split.snapshot = sStateTaken;
    split.height = lines == 192 ? 0 : lines;
    NitroPort_Render2DSplit(NitroPort_EngineAOnTop() ? 1 : 0, canvas, &split);
    width = split.width > 256 ? split.width : 256;
    if (split.has3d) {
        CanvasWith3D(canvas, width, &split, over, overAlpha);
    }
    for (y = 0; y < lines; y++) {
        memcpy(&out[y * 256], &canvas[y * width], 256 * sizeof(out[0]));
    }
    return WritePng(path, out, 256, lines);
}

static int MenuShot(const char *path)
{
    static uint32_t out[NITRO_LOWERMENU_W * NITRO_LOWERMENU_H];

    MenuRender(out);
    return WritePngRgba(path, out, NITRO_LOWERMENU_W, NITRO_LOWERMENU_H);
}

/* (the remaster) The panel's fades go by the runner's frames (its V-blanks). */
static uint32_t MenuClock(void)
{
    return (uint32_t)sFrame;
}

/* --map-thread: the map's drawer on a thread of its own, as the 3DS has it
 * (off: drawn on the game's thread as it's handed over, the runner's
 * pictures the same every run). */
static sem_t sMapGo;

static void MapWake(void)
{
    sem_post(&sMapGo);
}

static void *MapThread(void *arg)
{
    (void)arg;
    for (;;) {
        while (sem_wait(&sMapGo) != 0) {
        }
        NitroPort_LowerMenuMapWork();
    }
    return NULL;
}

static void StartMapThread(void)
{
    pthread_t thread;

    if (sem_init(&sMapGo, 0, 0) == 0 && pthread_create(&thread, NULL, MapThread, NULL) == 0) {
        pthread_detach(thread);
        NitroPort_LowerMenuSetMapWorker(MapWake);
        NitroPort_Log("[pc] the map's drawer on a thread of its own");
    }
}

/* The panel's picture with its 3D (an app shown there with it: the
 * summary's Pokemon, the party's sparkles), as RemasterRender puts a
 * screen together: the newest 3D image over what's behind it, the layers
 * in front over that, on the DS's 192 lines (a tall picture's first). */
static void CanvasWith3D(uint16_t *canvas, int width, const NitroRender2DSplit *split, const uint16_t *over,
                         const uint8_t *overAlpha)
{
    static uint16_t image3d[256 * 192];
    static uint8_t alpha3d[256 * 192];
    const uint16_t *img = NitroPort_Gx3dAcquireImage();
    const uint8_t *alpha = img != NULL ? NitroPort_Gx3dImageAlpha(img) : NULL;
    int x, y, k;

    memset(image3d, 0, sizeof(image3d));
    if (img != NULL) {
        memcpy(image3d, img, sizeof(image3d));
        if (alpha != NULL) {
            memcpy(alpha3d, alpha, sizeof(alpha3d));
        } else {
            memset(alpha3d, 31, sizeof(alpha3d));
        }
        NitroPort_Gx3dReleaseImage(img);
    }
    for (y = 0; y < 192; y++) {
        int scroll3d = split->scroll3dLines ? split->scroll3dLine[y] : split->scroll3d;
        int scroll = scroll3d >= 256 ? scroll3d - 512 : scroll3d;

        for (x = 0; x < width; x++) {
            uint16_t c = canvas[y * width + x];
            int ix = x - split->origin + scroll;

            if (ix >= 0 && ix < 256 && (image3d[y * 256 + ix] & 0x8000)) {
                uint16_t p = image3d[y * 256 + ix] & 0x7FFF, m = 0;
                int a = alpha3d[y * 256 + ix];

                for (k = 0; k < 15; k += 5) {
                    m |= (uint16_t)(((((p >> k) & 31) * a + ((c >> k) & 31) * (31 - a)) / 31) << k);
                }
                c = m;
            }
            if (over[y * width + x] & 0x8000) {
                int a = overAlpha[y * width + x];
                uint16_t o = over[y * width + x] & 0x7FFF, m = 0;

                for (k = 0; k < 15; k += 5) {
                    m |= (uint16_t)(((((o >> k) & 31) * a + ((c >> k) & 31) * (16 - a)) / 16) << k);
                }
                c = m;
            }
            canvas[y * width + x] = c;
        }
    }
}

static void MenuRender(uint32_t *out)
{
    static uint16_t under[320 * NITRO_LOWERMENU_TALL_LINES], over[320 * 192];
    static uint8_t overAlpha[320 * 192];
    int engine = NitroPort_EngineAOnTop() ? 1 : 0;   /* the lower screen's */
    int tall = NitroPort_LowerMenuTall();             /* (drawn on below the DS's rows: the canvas's own) */
    int mapShown = NitroPort_LowerMenuPanel() == NITRO_LOWERMENU_PANEL_MAP, fade = NitroPort_LowerMenuPanelFade();
    NitroRender2DSplit split;
    const uint32_t *pic;
    const uint16_t *canvas = under;
    int x, y, width, mode, f;

    if (!NitroPort_LowerMenuOn() && !NitroPort_LowerMenuKeptShown()) {
        NitroPort_Log("[pc] mshot: the lower screen isn't the menu now (drawn anyway)");
    }
    pic = NitroPort_LowerMenuLockFrame(NULL);
    memcpy(out, pic, NITRO_LOWERMENU_W * NITRO_LOWERMENU_H * sizeof(out[0]));
    NitroPort_LowerMenuUnlockFrame();
    memset(&split, 0, sizeof(split));
    split.over = over;
    split.overAlpha = overAlpha;
    split.windowsToEdge = 1;
    split.snapshot = sStateTaken;
    split.height = tall ? NITRO_LOWERMENU_TALL_LINES : 0;
    NitroPort_Render2DSplit(engine, under, &split);   /* (its master brightness too) */
    width = split.width > 256 ? split.width : 256;
    if (split.has3d) {
        CanvasWith3D(under, width, &split, over, overAlpha);
    }
    if (mapShown) {
        /* (the map's picture: none yet, the panel black, the fade) */
        const uint32_t *map = NitroPort_LowerMenuLockMap(NULL);

        for (y = 0; map != NULL && y < NITRO_LOWERMENU_PANEL_H; y++) {
            memcpy(out + (NITRO_LOWERMENU_PANEL_Y0 + y) * NITRO_LOWERMENU_W + NITRO_LOWERMENU_PANEL_X0,
                   map + y * NITRO_LOWERMENU_PANEL_W, NITRO_LOWERMENU_PANEL_W * sizeof(out[0]));
        }
        NitroPort_LowerMenuUnlockMap();
        goto fade;
    }
    for (y = 0; y < NITRO_LOWERMENU_PANEL_H; y++) {
        for (x = 0; x < NITRO_LOWERMENU_PANEL_W; x++) {
            /* the canvas's point there, in its 2x2 texels (as the GPU) */
            float tx = ((float)x + 0.5f) * (2.0f * NITRO_LOWERMENU_CANVAS_W / NITRO_LOWERMENU_PANEL_W) - 0.5f;
            float ty = ((float)y + 0.5f) * (2.0f * NITRO_LOWERMENU_CANVAS_H / NITRO_LOWERMENU_PANEL_H) - 0.5f;
            int t0x = (int)tx, t0y = (int)ty, k, i;
            float fx = tx - (float)t0x, fy = ty - (float)t0y, acc[3] = { 0, 0, 0 };

            for (i = 0; i < 4; i++) {
                int txi = t0x + (i & 1), tyi = t0y + (i >> 1);
                int cx = txi / 2 > 255 ? 255 : txi / 2, cy = tyi / 2 - (tall ? 0 : NITRO_LOWERMENU_DS_ROW0);
                float w = ((i & 1) ? fx : 1.0f - fx) * ((i >> 1) ? fy : 1.0f - fy);
                uint16_t c;

                cy = cy < 0 ? 0 : cy > (tall ? NITRO_LOWERMENU_TALL_LINES - 1 : 191) ? (tall ? NITRO_LOWERMENU_TALL_LINES - 1 : 191) : cy;
                c = canvas[cy * width + cx];
                for (k = 0; k < 3; k++) {
                    int v = (c >> (5 * k)) & 31;
                    acc[k] += w * (float)((v << 3) | (v >> 2));
                }
            }
            out[(NITRO_LOWERMENU_PANEL_Y0 + y) * NITRO_LOWERMENU_W + NITRO_LOWERMENU_PANEL_X0 + x] =
                ((uint32_t)(acc[0] + 0.5f) << 24) | ((uint32_t)(acc[1] + 0.5f) << 16) |
                ((uint32_t)(acc[2] + 0.5f) << 8) | 0xFFu;
        }
    }
fade:
    if (fade > 0) {
        /* (black over the panel at fade / 16, as the GPU blends it) */
        uint32_t a = (uint32_t)(fade > 16 ? 16 : fade) * 255u / 16u;

        for (y = 0; y < NITRO_LOWERMENU_PANEL_H; y++) {
            for (x = 0; x < NITRO_LOWERMENU_PANEL_W; x++) {
                uint32_t *p = &out[(NITRO_LOWERMENU_PANEL_Y0 + y) * NITRO_LOWERMENU_W + NITRO_LOWERMENU_PANEL_X0 + x];
                uint32_t c = *p, m = 0xFFu;
                int k;

                for (k = 24; k >= 8; k -= 8) {
                    m |= (((c >> k) & 0xFFu) * (255u - a) / 255u) << k;
                }
                *p = m;
            }
        }
    }
    mode = (split.masterBright >> 14) & 3;
    f = split.masterBright & 31;
    f = f > 16 ? 16 : f;
    if (f && (mode == 1 || mode == 2)) {
        for (y = 0; y < NITRO_LOWERMENU_W * NITRO_LOWERMENU_H; y++) {
            uint32_t c = out[y], m = 0xFFu;
            int k;

            for (k = 24; k >= 8; k -= 8) {
                int v = (int)((c >> k) & 0xFFu);
                v = mode == 1 ? v + (255 - v) * f / 16 : v - v * f / 16;
                m |= (uint32_t)v << k;
            }
            out[y] = m;
        }
    }
}

/* Both screens, the upper one above the lower one. */
static void Screenshot(void)
{
    static uint16_t both[256 * 384];
    char path[1024];

    if (sScreenshotDir == NULL) {
        return;
    }
    NitroPort_RenderScreens(both, both + 256 * 192);
    snprintf(path, sizeof(path), "%s/frame_%07ld.png", sScreenshotDir, sFrame);
    if (WritePng(path, both, 256, 384) != 0) {
        NitroPort_Log("[pc] can't write %s", path);
    }
}

/* ---------------------------------------------------------------- buttons */

static const struct { const char *name; uint16_t bit; } sButtonNames[] = {
    { "A", PLATINUM_BUTTON_A }, { "B", PLATINUM_BUTTON_B },
    { "X", PLATINUM_BUTTON_X }, { "Y", PLATINUM_BUTTON_Y },
    { "L", PLATINUM_BUTTON_L }, { "R", PLATINUM_BUTTON_R },
    { "START", PLATINUM_BUTTON_START }, { "SELECT", PLATINUM_BUTTON_SELECT },
    { "UP", PLATINUM_KEY_UP }, { "DOWN", PLATINUM_KEY_DOWN },
    { "LEFT", PLATINUM_KEY_LEFT }, { "RIGHT", PLATINUM_KEY_RIGHT },
};

/* "A+UP" -> button bits; 0 if a name isn't known. */
static uint16_t ParseButtonList(const char *p)
{
    uint16_t buttons = 0;

    for (;;) {
        size_t len = strcspn(p, "+ \t\r\n");
        size_t k;
        int found = 0;

        for (k = 0; k < sizeof(sButtonNames) / sizeof(sButtonNames[0]); k++) {
            if (strlen(sButtonNames[k].name) == len && strncmp(p, sButtonNames[k].name, len) == 0) {
                buttons |= sButtonNames[k].bit;
                found = 1;
            }
        }
        if (!found) {
            return 0;
        }
        p += len;
        if (*p != '+') {
            return buttons;
        }
        p++;
    }
}

static int ParsePresses(const char *spec)
{
    static const struct { const char *name; uint16_t bit; } names[] = {
        { "A", PLATINUM_BUTTON_A }, { "B", PLATINUM_BUTTON_B },
        { "X", PLATINUM_BUTTON_X }, { "Y", PLATINUM_BUTTON_Y },
        { "L", PLATINUM_BUTTON_L }, { "R", PLATINUM_BUTTON_R },
        { "START", PLATINUM_BUTTON_START }, { "SELECT", PLATINUM_BUTTON_SELECT },
        { "UP", PLATINUM_KEY_UP }, { "DOWN", PLATINUM_KEY_DOWN },
        { "LEFT", PLATINUM_KEY_LEFT }, { "RIGHT", PLATINUM_KEY_RIGHT },
    };
    const char *p = spec;

    while (*p != '\0') {
        char *end;
        long frame = strtol(p, &end, 10);
        uint16_t buttons = 0;

        if (end == p || *end != ':' || sPressCount >= MAX_PRESSES) {
            return -1;
        }
        p = end + 1;
        for (;;) {
            size_t len = strcspn(p, "+,");
            size_t k;
            int found = 0;

            for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
                if (strlen(names[k].name) == len && strncmp(p, names[k].name, len) == 0) {
                    buttons |= names[k].bit;
                    found = 1;
                }
            }
            if (!found) {
                return -1;
            }
            p += len;
            if (*p != '+') {
                break;
            }
            p++;
        }
        sPresses[sPressCount].frame = frame;
        sPresses[sPressCount].buttons = buttons;
        sPressCount++;
        if (*p == ',') {
            p++;
        }
    }
    return 0;
}

static uint16_t HeldButtons(long frame)
{
    uint16_t held = 0;
    int i;

    for (i = 0; i < sPressCount; i++) {
        if (frame >= sPresses[i].frame && frame < sPresses[i].frame + 6) {
            held |= sPresses[i].buttons;
        }
    }
    return held;
}

/* --touch FRAME:X:Y,...: touch the lower screen at (X, Y) for 6 frames. */
static int ParseTouches(const char *spec)
{
    const char *p = spec;

    while (*p != '\0') {
        char *end;
        long frame = strtol(p, &end, 10);
        long x, y;

        if (end == p || *end != ':' || sTouchCount >= MAX_TOUCHES) {
            return -1;
        }
        p = end + 1;
        x = strtol(p, &end, 10);
        if (end == p || *end != ':' || x < 0 || x > 255) {
            return -1;
        }
        p = end + 1;
        y = strtol(p, &end, 10);
        if (end == p || y < 0 || y > 191) {
            return -1;
        }
        p = end;
        sTouches[sTouchCount].frame = frame;
        sTouches[sTouchCount].x = (int)x;
        sTouches[sTouchCount].y = (int)y;
        sTouchCount++;
        if (*p == ',') {
            p++;
        } else if (*p != '\0') {
            return -1;
        }
    }
    return 0;
}

static void ApplyTouch(long frame)
{
    int i;

    for (i = 0; i < sTouchCount; i++) {
        if (frame >= sTouches[i].frame && frame < sTouches[i].frame + 6) {
            Platinum_SetTouch(1, sTouches[i].x, sTouches[i].y);
            return;
        }
    }
    Platinum_SetTouch(0, 0, 0);
}

void NitroPort_Log(const char *fmt, ...)
{
    va_list args;

    fprintf(stderr, "[%6ld] ", sFrame);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

/* --control DIR: when the runner reaches the frame it was told to run to,
 * writes the current frame number to DIR/frame and waits for a file
 * DIR/cmd; it runs its lines and deletes it. Commands:
 *   run N            run N more frames, then wait again
 *   press A+UP [N]   hold buttons for N frames (default 6) from now
 *   touch X Y [N]    touch the lower screen for N frames (default 6)
 *   shot PATH        save both screens (PNG, upper above lower)
 *   rshot PATH       the same as the 3DS remaster lays them out (--wide)
 *   mshot PATH       the 3DS's lower screen as the menu, at its 320 x 240
 *   cshot PATH       the lower screen's engine as the panel shows it, at the DS's
 *                    pixels (256 x 248 when it's tall)
 *   mtouch X Y [N]   touch the 3DS's lower screen at (X, Y) of its 320 x 240
 *   dump3d PATH      save the newest finished 3D image (PNG)
 *   ref3d PATH       draw the newest scene with the reference rasterizer
 *   gpusim PATH      draw it as the 3DS GPU path would (port_gx3d_gpusim.c)
 *   interp 0|1       in-between scenes off or on (NitroPort_Gx3dSetInterpolation)
 *   midsim PATH      draw the newest in-between scene (halfway between the
 *                    newest scene and the one before) as gpusim does, and
 *                    log how many of its polygons were matched
 *   look S G [E A]   the remaster's look in gpusim: sun shadows (S) and the
 *                    colour grade (G) 0 or 1; the sun fixed at elevation E,
 *                    azimuth A (degrees), else by the clock (TZ)
 *   tri3d X Y        log the newest scene's triangles covering pixel (X, Y)
 *   bench2d N [E]    draw each screen (or engine E only: 0 A, 1 B) N times
 *                    and log the time per draw
 *   bench2ds N [E]   the same, drawn as the 3DS remaster does (two layers;
 *                    the upper screen wide, with its pictures)
 *   scene3d PATH     write the newest scene's triangles as text (one per line)
 *   regs PATH        write the I/O registers (0x4000000-0x4001FFF) as binary
 *   tex3d PARAM PAL PATH  write a texture (hex TEXIMAGE_PARAM, palette base)
 *                    as it is now: width, height (u32 each), then RGBA8 words
 *   layers PATH      each layer alone (BG0-3, OBJ) of both engines, a sheet
 *   capture DIR      a wide art capture into DIR/captures (docs/WIDE_ART.md)
 *   wideart DIR      read the wide art pictures in DIR again
 *   bgs              log each engine's display control and BGs
 *   vram PATH        write engine A's BG VRAM (512 KB) and the palettes (2 KB)
 *   oam              log each engine's shown OBJs (position, size, tile,
 *                    palette, priority)
 *   quit             stop the runner
 * For driving the game by hand (scripts, or an assistant) while debugging. */
static void ControlWait(void)
{
    char path[1024], line[1100];

    int announced = 0;

    while (sFrame >= sRunUntil) {
        FILE *f;

        if (!announced) {
            /* Waiting: tell the controller which frame we're on. */
            snprintf(path, sizeof(path), "%s/frame", sControlDir);
            f = fopen(path, "w");
            if (f != NULL) {
                fprintf(f, "%ld\n", sFrame);
                fclose(f);
            }
            fflush(stderr);
            announced = 1;
        }
        snprintf(path, sizeof(path), "%s/cmd", sControlDir);
        f = fopen(path, "r");
        if (f == NULL) {
            usleep(20000);
            continue;
        }
        while (fgets(line, sizeof(line), f) != NULL) {
            char arg[1024];
            long a = 0, b = 0, c = 0;

            if (sscanf(line, "run %ld", &a) == 1) {
                sRunUntil = sFrame + (a > 0 ? a : 1);
            } else if (sscanf(line, "press %1023s %ld", arg, &a) >= 1) {
                sLiveButtons = ParseButtonList(arg);
                sLiveButtonsUntil = sFrame + (a > 0 ? a : 6);
                if (sLiveButtons == 0) {
                    NitroPort_Log("[pc] control: unknown buttons %s", arg);
                }
            } else if (sscanf(line, "touch %ld %ld %ld", &a, &b, &c) >= 2) {
                if (sLiveTouchMenu) {
                    NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);   /* (an mtouch let go) */
                }
                sLiveTouchX = (int)a;
                sLiveTouchY = (int)b;
                sLiveTouchUntil = sFrame + (c > 0 ? c : 6);
                sLiveTouchMenu = 0;
            } else if (sscanf(line, "mtouch %ld %ld %ld", &a, &b, &c) >= 2) {
                /* the 3DS's lower screen touched at (a, b) of its 320 x 240,
                 * as the 3DS takes it (the menu's buttons, or the game's
                 * screen in its panel) */
                sLiveTouchX = (int)a;
                sLiveTouchY = (int)b;
                sLiveTouchUntil = sFrame + (c > 0 ? c : 6);
                sLiveTouchMenu = 1;
            } else if (sscanf(line, "mshot %1023s", arg) == 1) {
                if (MenuShot(arg) != 0) {
                    NitroPort_Log("[pc] control: can't write %s", arg);
                }
            } else if (sscanf(line, "cshot %1023s", arg) == 1) {
                if (CanvasShot(arg) != 0) {
                    NitroPort_Log("[pc] control: can't write %s", arg);
                }
            } else if (sscanf(line, "shot %1023s", arg) == 1) {
                static uint16_t both[256 * 384];

                NitroPort_RenderScreens(both, both + 256 * 192);
                if (WritePng(arg, both, 256, 384) != 0) {
                    NitroPort_Log("[pc] control: can't write %s", arg);
                }
            } else if (strncmp(line, "oam", 3) == 0) {
                /* the shown OBJs of each engine: position, size, tile, palette, priority */
                extern unsigned char NitroPort_Oam[0x800];   /* (port_memory.c) */
                int engine, i;

                for (engine = 0; engine < 2; engine++) {
                    const unsigned char *oam = NitroPort_Oam + (engine ? 0x400 : 0);

                    for (i = 0; i < 128; i++) {
                        unsigned a0 = oam[i * 8] | (oam[i * 8 + 1] << 8), a1 = oam[i * 8 + 2] | (oam[i * 8 + 3] << 8),
                                 a2 = oam[i * 8 + 4] | (oam[i * 8 + 5] << 8);

                        if ((a0 & 0x300) == 0x200) {
                            continue;   /* hidden */
                        }
                        fprintf(stderr, "[oam] %c%3d: x %3u y %3u shape %u size %u mode %u %s tile %4u pal %2u prio %u%s\n",
                                engine ? 'B' : 'A', i, a1 & 0x1FF, a0 & 0xFF, a0 >> 14, a1 >> 14, (a0 >> 10) & 3,
                                (a0 & 0x2000) ? "8bpp" : "4bpp", a2 & 0x3FF, a2 >> 12, (a2 >> 10) & 3,
                                (a0 & 0x100) ? " affine" : "");
                    }
                }
            } else if (strncmp(line, "vram ", 5) == 0) {
                /* Engine A's BG VRAM (512 KB) then the palettes (2 KB), as
                 * binary: to look at a BG's map and tiles. */
                extern unsigned char NitroPort_BgVram[], NitroPort_Pltt[];
                char *end = line + strlen(line);
                FILE *out;

                while (end > line + 5 && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
                    *--end = 0;
                }
                out = fopen(line + 5, "wb");
                if (out != NULL) {
                    fwrite(NitroPort_BgVram, 1, 0x80000, out);
                    fwrite(NitroPort_Pltt, 1, 0x800, out);
                    fclose(out);
                }
            } else if (strncmp(line, "bgs", 3) == 0) {
                /* Each engine's display control and BGs (mode, size,
                 * scroll), to see what a screen is made of. */
                extern unsigned char NitroPort_IoReg[];
                int engine, bg;

                for (engine = 0; engine < 2; engine++) {
                    const unsigned char *io = NitroPort_IoReg + (engine ? 0x1000 : 0);
                    uint32_t dispcnt = (uint32_t)io[0] | ((uint32_t)io[1] << 8) | ((uint32_t)io[2] << 16) |
                                       ((uint32_t)io[3] << 24);

                    fprintf(stderr, "[bgs] engine %c: DISPCNT %08x (mode %u, bg mode %u, layers %02x)\n",
                            engine ? 'B' : 'A', dispcnt, dispcnt >> 16 & 3, dispcnt & 7, dispcnt >> 8 & 0x1F);
                    for (bg = 0; bg < 4; bg++) {
                        uint16_t bgcnt = (uint16_t)(io[8 + bg * 2] | (io[9 + bg * 2] << 8));
                        uint16_t hofs = (uint16_t)((io[0x10 + bg * 4] | (io[0x11 + bg * 4] << 8)) & 0x1FF);
                        uint16_t vofs = (uint16_t)((io[0x12 + bg * 4] | (io[0x13 + bg * 4] << 8)) & 0x1FF);

                        fprintf(stderr, "[bgs]   BG%d: BGCNT %04x (size %u, prio %u, %s), scroll %u,%u%s\n", bg,
                                bgcnt, bgcnt >> 14, bgcnt & 3, bgcnt & 0x80 ? "256 colours" : "16 colours",
                                hofs, vofs, (dispcnt >> (8 + bg)) & 1 ? "" : " (off)");
                    }
                }
            } else if (sscanf(line, "layers %1023s", arg) == 1) {
                /* Each layer alone (BG0-3, OBJ), both screens side by
                 * side: 5 panels a row, engine A's row above engine B's. */
                static uint16_t both[256 * 384], sheet[256 * 5 * 384];
                extern unsigned char NitroPort_IoReg[];
                int engine, layer, y;

                for (engine = 0; engine < 2; engine++) {
                    unsigned char *reg = NitroPort_IoReg + (engine ? 0x1000 : 0) + 1;
                    unsigned char saved = *reg;

                    for (layer = 0; layer < 5; layer++) {
                        *reg = (unsigned char)((saved & ~0x1F) | (saved & (1 << layer)));
                        NitroPort_Render2D(engine, both);
                        for (y = 0; y < 192; y++) {
                            memcpy(&sheet[((size_t)engine * 192 + y) * 1280 + layer * 256], &both[y * 256], 512);
                        }
                    }
                    *reg = saved;
                }
                if (WritePng(arg, sheet, 1280, 384) != 0) {
                    NitroPort_Log("[pc] control: can't write %s", arg);
                }
            } else if (sscanf(line, "capture %1023s", arg) == 1) {
                /* the upper screen for the wide art editor, into a new
                 * folder in arg/captures (docs/WIDE_ART.md) */
                char made[1100];

                if (NitroPort_WideArtCapture(arg, made, sizeof(made)) == 0) {
                    fprintf(stderr, "[pc] captured %s\n", made);
                }
            } else if (sscanf(line, "wideart %1023s", arg) == 1) {
                NitroPort_WideArtLoad(arg);
            } else if (sscanf(line, "rshot %1023s", arg) == 1) {
                if (RemasterShot(arg) != 0) {
                    NitroPort_Log("[pc] control: can't write %s", arg);
                }
            } else if (sscanf(line, "dump3d %1023s", arg) == 1) {
                if (!NitroPort_Gx3dDumpLatest(arg)) {
                    NitroPort_Log("[pc] control: no 3D image for %s", arg);
                }
            } else if (sscanf(line, "ref3d %1023s", arg) == 1) {
                /* The newest scene through the PC reference rasterizer. */
                static uint16_t image[NITRO_GX3D_WIDTH * NITRO_GX3D_HEIGHT];
                NitroPort_Gx3dRasterizeScene(NitroPort_Gx3dScene(), image, NULL);
                if (!NitroPort_Gx3dWritePng(arg, image)) {
                    NitroPort_Log("[pc] control: can't write %s", arg);
                }
            } else if (sscanf(line, "gpusim %1023s", arg) == 1) {
                /* The same scene the way the 3DS GPU path draws it. */
                static uint16_t image[NITRO_GX3D_WIDTH * NITRO_GX3D_HEIGHT];
                NitroPort_Gx3dSimulateGpu(NitroPort_Gx3dScene(), image);
                if (!NitroPort_Gx3dWritePng(arg, image)) {
                    NitroPort_Log("[pc] control: can't write %s", arg);
                }
            } else if (sscanf(line, "interp %ld", &a) == 1) {
                NitroPort_Gx3dSetInterpolation((int)a);
            } else if (sscanf(line, "midsim %1023s", arg) == 1) {
                static uint16_t image[NITRO_GX3D_WIDTH * NITRO_GX3D_HEIGHT];
                unsigned matched = 0, polygons = 0;
                const NitroGx3dScene *mid = NitroPort_Gx3dMidScene(&matched, &polygons);
                NitroPort_Log("[pc] in-between scene: %s, %u of %u polygons halfway", mid ? "made" : "none",
                              matched, polygons);
                if (mid) {
                    NitroPort_Gx3dSimulateGpu(mid, image);
                    if (!NitroPort_Gx3dWritePng(arg, image)) {
                        NitroPort_Log("[pc] control: can't write %s", arg);
                    }
                }
            } else if (strncmp(line, "look ", 5) == 0) {
                int sh = 0, gr = 0, n;
                float e = 55.0f, az = 32.0f;
                n = sscanf(line + 5, "%d %d %f %f", &sh, &gr, &e, &az);
                NitroPort_Gx3dSimulateLook(sh, gr, n == 4, e, az);
            } else if (sscanf(line, "tri3d %ld %ld", &a, &b) == 2) {
                /* The newest scene's triangles that cover pixel (a, b). */
                const NitroGx3dScene *scene = NitroPort_Gx3dScene();
                uint32_t i;
                for (i = 0; scene && i < scene->count; i++) {
                    const NitroGx3dTriangle *t = &scene->triangles[i];
                    double px = a + 0.5, py = b + 0.5, e[3];
                    int k, pos = 0, neg = 0;
                    for (k = 0; k < 3; k++) {
                        const NitroGx3dVertex *p0 = &t->v[k], *p1 = &t->v[(k + 1) % 3];
                        e[k] = (px - p0->x / 256.0) * (p1->y / 256.0 - p0->y / 256.0) -
                               (py - p0->y / 256.0) * (p1->x / 256.0 - p0->x / 256.0);
                        pos += e[k] >= 0;
                        neg += e[k] <= 0;
                    }
                    if (pos == 3 || neg == 3) {
                        NitroPort_Log("[pc] tri %u: attr %08lx tex %08lx pal %lx depth %lu/%lu/%lu",
                                      (unsigned)i, (unsigned long)t->polygon_attr,
                                      (unsigned long)t->texture_param, (unsigned long)t->palette_base,
                                      (unsigned long)t->v[0].depth, (unsigned long)t->v[1].depth,
                                      (unsigned long)t->v[2].depth);
                        for (k = 0; k < 3; k++) {
                            NitroPort_Log("[pc]   v%d: xy %.1f,%.1f w %ld st %.2f,%.2f",
                                          k, t->v[k].x / 256.0, t->v[k].y / 256.0,
                                          (long)t->v[k].clip[3], t->v[k].s, t->v[k].t);
                        }
                    }
                }
            } else if (sscanf(line, "scene3d %1023s", arg) == 1) {
                /* attr tex pal swap, then per vertex: x y (1/256 px) w depth s t color */
                const NitroGx3dScene *scene = NitroPort_Gx3dScene();
                FILE *out = fopen(arg, "w");
                uint32_t i;
                for (i = 0; out && scene && i < scene->count; i++) {
                    const NitroGx3dTriangle *t = &scene->triangles[i];
                    int k;
                    fprintf(out, "%08lx %08lx %lx %lx", (unsigned long)t->polygon_attr,
                            (unsigned long)t->texture_param, (unsigned long)t->palette_base,
                            (unsigned long)scene->swap_param);
                    for (k = 0; k < 3; k++) {
                        const NitroGx3dVertex *v = &t->v[k];
                        fprintf(out, " %ld %ld %ld %lu %.3f %.3f %04x", (long)v->x, (long)v->y,
                                (long)v->clip[3], (unsigned long)v->depth, v->s, v->t, v->color);
                    }
                    fputc('\n', out);
                }
                if (out) fclose(out);
            } else if (sscanf(line, "tex3d %lx %lx %1023s", &a, &b, arg) == 3) {
                static uint32_t texels[1024 * 1024];
                NitroGx3dTexBanks banks;
                FILE *out = fopen(arg, "wb");
                uint32_t size[2];
                NitroPort_Gx3dMapTextureBanks(&banks);
                size[0] = NitroPort_Gx3dTexWidth((uint32_t)a);
                size[1] = NitroPort_Gx3dTexHeight((uint32_t)a);
                NitroPort_Gx3dDecodeTexture(&banks, (uint32_t)a, (uint32_t)b, texels);
                if (out) {
                    fwrite(size, 4, 2, out);
                    fwrite(texels, 4, (size_t)size[0] * size[1], out);
                    fclose(out);
                }
            } else if (sscanf(line, "regs %1023s", arg) == 1) {
                FILE *out = fopen(arg, "wb");
                if (out) {
                    extern unsigned char NitroPort_IoReg[];
                    fwrite(NitroPort_IoReg, 1, 0x2000, out);
                    fclose(out);
                }
            } else if (sscanf(line, "bench2d %ld", &a) == 1) {
                /* What drawing each screen costs right now ("bench2d N E":
                 * engine E only, 0 A or 1 B). */
                static uint16_t screen[256 * 192];
                int engine, only = -1;
                sscanf(line, "bench2d %ld %d", &a, &only);
                for (engine = 0; engine < 2; engine++) {
                    if (only >= 0 && engine != only) continue;
                    struct timespec t0, t1;
                    long k;
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    for (k = 0; k < a; k++) NitroPort_Render2D(engine, screen);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    NitroPort_Log("[pc] bench2d engine %c: %.3f ms/draw", engine ? 'B' : 'A',
                                  ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / (a > 0 ? a : 1));
                }
            } else if (sscanf(line, "bench2ds %ld", &a) == 1) {
                /* The same, as the 3DS remaster draws them: in two layers,
                 * the upper screen on the wider canvas with its pictures. */
                static uint16_t under[320 * 192], over[320 * 192];
                static uint8_t overAlpha[320 * 192];
                int engine, only = -1, aOnTop = NitroPort_EngineAOnTop() ? 1 : 0;
                sscanf(line, "bench2ds %ld %d", &a, &only);
                for (engine = 0; engine < 2; engine++) {
                    struct timespec t0, t1;
                    long k;
                    int top = (engine == 0) == aOnTop;
                    if (only >= 0 && engine != only) continue;
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    for (k = 0; k < a; k++) {
                        NitroRender2DSplit split;
                        memset(&split, 0, sizeof(split));
                        split.over = over;
                        split.overAlpha = overAlpha;
                        if (top) NitroPort_WideLayout(engine, &split);
                        NitroPort_Render2DSplit(engine, under, &split);
                    }
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    NitroPort_Log("[pc] bench2ds engine %c (%s): %.3f ms/draw", engine ? 'B' : 'A',
                                  top ? "upper" : "lower",
                                  ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / (a > 0 ? a : 1));
                }
            } else if (strncmp(line, "quit", 4) == 0) {
                fclose(f);
                remove(path);
                NitroPort_Log("[pc] control: quit");
                StopSound();
                NitroPort_CardFlush();
                fflush(NULL);   /* stderr, and a --gx-record file */
                _exit(0);
            }
        }
        fclose(f);
        remove(path);
        announced = 0;
    }
}

/* --gx-record FILE: every call into the geometry engine, for
 * nitro/tests/gx3d_replay.c (records: type, word count, words). */
static FILE *sGxRecord;
static void GxRecord(int type, const uint32_t *words, size_t count)
{
    uint32_t head[2];

    head[0] = (uint32_t)type;
    head[1] = (uint32_t)count;
    fwrite(head, sizeof(head), 1, sGxRecord);
    if (count) {
        fwrite(words, sizeof(*words), count, sGxRecord);
    }
}

/* --gx-thread: the geometry engine on a thread of its own, as on the 3DS
 * (--gx-thread-skip: leaving out scenes when it's behind, as there). */
static int sGxThread;
static sem_t sGxWorkerSem, sGxProducerSem;
static volatile int sGxRunning = 1;
static void GxWaitWorker(void *c) { (void)c; while (sem_wait(&sGxWorkerSem) != 0) {} }
static void GxWakeWorker(void *c) { (void)c; sem_post(&sGxWorkerSem); }
static void GxWaitProducer(void *c) { (void)c; while (sem_wait(&sGxProducerSem) != 0) {} }
static void GxWakeProducer(void *c) { (void)c; sem_post(&sGxProducerSem); }
static void *GeometryThread(void *unused)
{
    (void)unused;
    NitroPort_Gx3dThreadMain(&sGxRunning);
    return NULL;
}
static void StartGeometryThread(int keepEveryScene)
{
    NitroGx3dThreadOps ops;
    pthread_t thread;

    memset(&ops, 0, sizeof(ops));
    ops.wait_worker = GxWaitWorker;
    ops.wake_worker = GxWakeWorker;
    ops.wait_producer = GxWaitProducer;
    ops.wake_producer = GxWakeProducer;
    ops.keep_every_scene = keepEveryScene;
    sem_init(&sGxWorkerSem, 0, 0);
    sem_init(&sGxProducerSem, 0, 0);
    NitroPort_Gx3dStartThread(&ops);
    if (pthread_create(&thread, NULL, GeometryThread, NULL) != 0) {
        fprintf(stderr, "can't start the geometry thread\n");
        exit(2);
    }
    NitroPort_Log("[pc] geometry engine on its own thread%s",
                  keepEveryScene ? "" : " (scenes left out when it's behind)");
}

void NitroPort_OnFrame(void)
{
    /* (--wide) the wider view only while the 3D is on the upper screen */
    if (NitroPort_WideScreenExtra()) {
        NitroPort_Gx3dSetWideView(NitroPort_EngineAOnTop() ? 320 : 256);
    }
    NitroPort_Gx3dFlush();
    NitroPort_SoundPumpReplies(); /* deliver ARM7 alarms on ARM9's thread */
    pace_game_for_wav();
    sFrame++;
    /* (the frame's lines, for an H-blank effect: what the screenshots and
     * the screens drawn below show) */
    (void)NitroPort_HBlankLinesOnce();
    /* (--wide DIR) its pictures shared without the game's art, as their
     * screens show (docs/WIDE_ART.md, "Sharing your pictures") */
    if (NitroPort_WideScreenExtra() && sFrame % 15 == 0 && NitroPort_WideArtUnshared() > 0) {
        NitroPort_Render2DShareArt(NitroPort_EngineAOnTop() ? 0 : 1);
    }
    if (sDump3DFrame == sFrame) {
        char path[128];
        snprintf(path,sizeof(path),"3d_frame_%07ld.png",sFrame);
        if (NitroPort_Gx3dDumpLatest(path))
            NitroPort_Log("[gx3d] wrote reference frame %ld to %s",sFrame,path);
        else
            NitroPort_Log("[gx3d] cannot dump frame %ld (no image or file error)",sFrame);
    }
    if (sScreenshotDir != NULL && sScreenshotEvery > 0 && sFrame % sScreenshotEvery == 0) {
        Screenshot();
    }
    if (sFrameLimit >= 0 && sFrame >= sFrameLimit) {
        Screenshot();
        NitroPort_Log("[pc] stopping after %ld frames, as asked", sFrame);
        StopSound();            /* closes and finalizes optional WAV file */
        fflush(NULL);           /* stderr, and a --gx-record file */
        _exit(0);
    }
    if (NitroPort_LowerMenuAvailable()) {
        KeepUpperScreen();   /* (an app in the menu's panel: the upper screen kept) */
        NitroPort_LowerMenuPanelShown(MenuClock());   /* (drawn as it is whenever it's drawn: never a picture behind) */
        (void)NitroPort_LowerMenuPanelFade();   /* (each frame, as the compositor: a tab change made once it's black) */
    }
    if (sControlDir != NULL) {
        ControlWait();
    }
    Platinum_SetButtons((uint16_t)(HeldButtons(sFrame) |
                                   (sFrame < sLiveButtonsUntil ? sLiveButtons : 0)));
    if (sFrame < sLiveTouchUntil && sLiveTouchMenu) {
        int x = 0, y = 0;

        if (NitroPort_LowerMenuTouch(1, sLiveTouchX, sLiveTouchY, &x, &y)) {
            Platinum_SetTouch(1, x, y);
        } else {
            Platinum_SetTouch(0, 0, 0);
        }
    } else if (sFrame < sLiveTouchUntil) {
        Platinum_SetTouch(1, sLiveTouchX, sLiveTouchY);
    } else {
        /* No finger on the menu: told every frame, as the 3DS does (a
         * finger down as the menu came on counts once it's let go). */
        if (NitroPort_LowerMenuOn() || NitroPort_LowerMenuKeptShown()) {
            NitroPort_LowerMenuTouch(0, 0, 0, NULL, NULL);
        }
        sLiveTouchMenu = 0;
        if (sTouchCount > 0 || sLiveTouchUntil >= 0) {
            ApplyTouch(sFrame);
        }
    }
}

void NitroPort_Terminate(void)
{
    NitroPort_Log("[pc] the game stopped itself (OS_Terminate)");
    fflush(stderr);
    _exit(3);
}

void NitroPort_ResetSystem(u32 parameter)
{
    NitroPort_Log("[pc] the game asked to restart (OS_ResetSystem %lu); stopping",
                  (unsigned long)parameter);
    fflush(stderr);
    _exit(4);
}

void NitroPort_FileMissing(const char *path)
{
    NitroPort_Log("[nitro] the game asked for a file that isn't in the data pack: %s", path);
    if (sStopAtMissingFile) {
        NitroPort_Log("[pc] stopping there, as asked");
        fflush(stderr);
        _exit(5);
    }
}

/* Print return addresses as function, file:line (addr2line, which also
 * names static functions and inlined calls); raw symbols if that fails. */
static void PrintFrames(void *const *frames, int count)
{
    char command[64 * 24 + 128];
    size_t used = (size_t)snprintf(command, sizeof(command),
                                   "addr2line -f -i -p -e /proc/%d/exe", (int)getpid());
    int f;

    for (f = 0; f < count && used < sizeof(command) - 24; f++) {
        /* A return address points after the call: look up the call. */
        used += (size_t)snprintf(command + used, sizeof(command) - used, " %#lx",
                                 (unsigned long)((const char *)frames[f] - 1));
    }
    snprintf(command + used, sizeof(command) - used, " 1>&2");
    fflush(stderr);
    if (system(command) != 0) {
        backtrace_symbols_fd(frames, count, 2);
    }
}

static void OnInterrupt(int sig)
{
    (void)sig;
    Screenshot();
    fprintf(stderr, "[%6ld] [pc] stopped with Ctrl+C\n", sFrame);
    _exit(130);
}

static void OnCrash(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);

    fprintf(stderr, "[%6ld] [pc] crashed (signal %d). Backtrace:\n", sFrame, sig);
    /* Frames 0 and 1: this handler and the signal trampoline. */
    PrintFrames(frames + 2, n - 2);
    _exit(128 + sig);
}

/* Integer division by zero. The 3DS has no divide instruction: GCC calls a
 * library routine, which gives 0 for x / 0 and doesn't stop the program
 * (nor did the DS's). x86 traps instead. Some of Nintendo's code divides by
 * zero harmlessly (a value it then never uses), so here a division by zero
 * does what it does on the 3DS: quotient 0, remainder the dividend. Each
 * place is logged once. Any other arithmetic fault is a crash. */
static void OnArithmeticFault(int sig, siginfo_t *info, void *context)
{
#if defined(__i386__) || defined(__x86_64__)
    ucontext_t *uc = context;
#if defined(__x86_64__)
    greg_t *ip = &uc->uc_mcontext.gregs[REG_RIP];
    greg_t *ax = &uc->uc_mcontext.gregs[REG_RAX];
    greg_t *dx = &uc->uc_mcontext.gregs[REG_RDX];
#else
    greg_t *ip = &uc->uc_mcontext.gregs[REG_EIP];
    greg_t *ax = &uc->uc_mcontext.gregs[REG_EAX];
    greg_t *dx = &uc->uc_mcontext.gregs[REG_EDX];
#endif
    const unsigned char *code = (const unsigned char *)*ip;
    const unsigned char *p = code;
    static const unsigned char *logged[32];
    static int loggedCount;
    int wide = 0, rex = 0, mod, rm, i, seen = 0;

    if (info->si_code != FPE_INTDIV) {
        OnCrash(sig);
    }
    /* DIV/IDIV r/m: [66] [REX] F6|F7 ModRM [SIB] [disp] */
    if (*p == 0x66) {
        p++;
    }
#if defined(__x86_64__)
    if ((*p & 0xF0) == 0x40) {
        rex = *p++;
    }
#endif
    if ((*p != 0xF6 && *p != 0xF7) || ((p[1] >> 3) & 7) < 6) {
        OnCrash(sig);
    }
    wide = *p == 0xF7;
    p++;
    mod = *p >> 6;
    rm = *p & 7;
    p++;
    if (mod != 3 && rm == 4) {               /* SIB byte */
        int base = *p & 7;

        p++;
        if (mod == 0 && base == 5) {
            p += 4;
        }
    } else if (mod == 0 && rm == 5) {        /* [disp32] / [rip+disp32] */
        p += 4;
    }
    p += mod == 1 ? 1 : mod == 2 ? 4 : 0;

    for (i = 0; i < loggedCount; i++) {
        seen |= logged[i] == code;
    }
    if (!seen && loggedCount < 32) {
        void *where[1] = { (void *)(code + 1) };

        logged[loggedCount++] = code;
        NitroPort_Log("[pc] integer division by zero; continuing with 0, as on the 3DS, at:");
        PrintFrames(where, 1);
    }
    if (wide) {
        *dx = *ax;                           /* remainder: the dividend */
        *ax = 0;                             /* quotient: 0 */
    } else {
        *ax = (*ax & ~(greg_t)0xFFFF) | ((*ax & 0xFF) << 8);   /* AH = AL % 0, AL = 0 */
    }
    (void)rex;
    *ip = (greg_t)(uintptr_t)p;
#else
    (void)info;
    (void)context;
    OnCrash(sig);
#endif
}

/* ------------------------------------------------- where a stuck game is */

#define MAX_DUMPED_THREADS 64
#define MAX_FRAMES 40

static struct {
    pid_t tid;
    int count;
    void *frames[MAX_FRAMES];
} sDumps[MAX_DUMPED_THREADS];
static volatile int sDumpCount;

/* Runs on each thread the watchdog signals: note where that thread is. */
static void OnDumpSignal(int sig)
{
    int i = __atomic_fetch_add(&sDumpCount, 1, __ATOMIC_SEQ_CST);

    (void)sig;
    if (i < MAX_DUMPED_THREADS) {
        sDumps[i].tid = (pid_t)syscall(SYS_gettid);
        sDumps[i].count = backtrace(sDumps[i].frames, MAX_FRAMES);
    }
}

static void DumpThreads(void)
{
    pid_t self = (pid_t)syscall(SYS_gettid);
    DIR *dir = opendir("/proc/self/task");
    struct dirent *entry;
    int i, n;

    signal(SIGUSR1, OnDumpSignal);
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        pid_t tid = (pid_t)atoi(entry->d_name);

        if (tid > 0 && tid != self) {
            syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
        }
    }
    if (dir != NULL) {
        closedir(dir);
    }
    usleep(300 * 1000);

    n = sDumpCount < MAX_DUMPED_THREADS ? sDumpCount : MAX_DUMPED_THREADS;
    for (i = 0; i < n; i++) {
        fprintf(stderr, "---- thread %d:\n", (int)sDumps[i].tid);
        /* Frames 0 and 1: the signal handler and the signal trampoline. */
        PrintFrames(sDumps[i].frames + 2, sDumps[i].count - 2);
    }
}

/* If frames stop coming, say where the game is and stop. */
static void *Watch(void *arg)
{
    long last = -1;
    int still = 0;

    (void)arg;
    for (;;) {
        long now;

        sleep(1);
        now = sFrame;
        still = now == last ? still + 1 : 0;
        last = now;
        if (still >= sStallSeconds) {
            NitroPort_Log("[pc] no frame for %d seconds: the game is stuck. Where each thread "
                          "is (the one not waiting in port_thread.c is the stuck one):",
                          sStallSeconds);
            DumpThreads();
            Screenshot();
            NitroPort_Log("[pc] stopping (stuck)");
            fflush(stderr);
            _exit(6);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    PlatinumSettings settings;
    const char *error;
    const char *savePath = NULL;
    char *defaultSave = NULL;
    int i, classicMenu = 0, mapThread = 0;

    if (argc == 2 && strcmp(argv[1], "--check-overlays") == 0) {
        return Platinum_CheckOverlays() == 0 ? 0 : 1;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <platinum3ds.pak> <manifest.json> [--save FILE] "
                "[--frames N] [--wav FILE] [--stop-at-missing-file] [--stall-seconds N]\n"
                "       [--screenshots DIR] [--screenshot-every N] [--dump-3d FRAME]\n"
                "       [--press FRAME:BUTTONS,...] [--touch FRAME:X:Y,...]\n       [--control DIR] [--gx-thread | --gx-thread-skip] [--gx-record FILE] [--classic-menu] [--map-thread]\n       %s --check-overlays\n", argv[0], argv[0]);
        return 2;
    }
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            sWavPath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            sFrameLimit = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stop-at-missing-file") == 0) {
            sStopAtMissingFile = 1;
        } else if (strcmp(argv[i], "--screenshots") == 0 && i + 1 < argc) {
            sScreenshotDir = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-every") == 0 && i + 1 < argc) {
            sScreenshotEvery = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--dump-3d") == 0 && i + 1 < argc) {
            char *end;
            long requested;
            errno = 0;
            requested = strtol(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' || requested < 1) {
                fprintf(stderr, "--dump-3d: expected a positive frame number\n");
                return 2;
            }
            sDump3DFrame = requested;
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            if (ParsePresses(argv[++i]) != 0) {
                fprintf(stderr, "--press: expected FRAME:BUTTON[+BUTTON],... e.g. 8700:A,9000:START\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--control") == 0 && i + 1 < argc) {
            sControlDir = argv[++i];
            sStallSeconds = 0;   /* waiting for commands isn't a stall */
        } else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) {
            if (ParseTouches(argv[++i]) != 0) {
                fprintf(stderr, "--touch: expected FRAME:X:Y,... (lower screen, X 0-255, Y 0-191)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--gx-record") == 0 && i + 1 < argc) {
            sGxRecord = fopen(argv[++i], "wb");
            if (sGxRecord == NULL) {
                fprintf(stderr, "--gx-record: can't write %s\n", argv[i]);
                return 2;
            }
            NitroPort_Gx3dSetRecorder(GxRecord);
        } else if (strcmp(argv[i], "--wide") == 0) {
            /* The 3DS remaster's wider view (NitroPort_Gx3dSetWideView):
             * on the PC's 256-pixel picture the 3D comes out squeezed,
             * showing what the 3DS's 400 pixels would. */
            NitroPort_Gx3dSetWideView(320);
            NitroPort_SetWideScreen(32);
        } else if (strcmp(argv[i], "--wide-art") == 0 && i + 1 < argc) {
            /* hand-drawn pictures for the wide screen (docs/WIDE_ART.md) */
            NitroPort_WideArtLoad(argv[++i]);
        } else if (strcmp(argv[i], "--gx-thread") == 0) {
            sGxThread = 1;
        } else if (strcmp(argv[i], "--gx-thread-skip") == 0) {
            sGxThread = 2;
        } else if (strcmp(argv[i], "--stall-seconds") == 0 && i + 1 < argc) {
            sStallSeconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--map-thread") == 0) {
            mapThread = 1;
        } else if (strcmp(argv[i], "--classic-menu") == 0) {
            /* the DS's start menu on the upper screen, not the lower
             * screen's menu (the 3DS's classic display) */
            classicMenu = 1;
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGINT, OnInterrupt);
    signal(SIGSEGV, OnCrash);
    signal(SIGBUS, OnCrash);
    {
        struct sigaction fault;

        memset(&fault, 0, sizeof(fault));
        fault.sa_sigaction = OnArithmeticFault;
        fault.sa_flags = SA_SIGINFO;
        sigaction(SIGFPE, &fault, NULL);
    }
    signal(SIGILL, OnCrash);
    signal(SIGABRT, OnCrash);

    error = Platinum_OpenPak(argv[1], argv[2]);
    if (error != NULL) {
        fprintf(stderr, "%s: %s\n", argv[1], error);
        return 2;
    }

    if (savePath == NULL) {
        /* platinum.sav in the data pack's folder */
        const char *slash = strrchr(argv[1], '/');
        size_t dir = slash != NULL ? (size_t)(slash - argv[1] + 1) : 0;

        defaultSave = malloc(dir + sizeof("platinum.sav"));
        if (defaultSave == NULL) {
            return 2;
        }
        memcpy(defaultSave, argv[1], dir);
        strcpy(defaultSave + dir, "platinum.sav");
        savePath = defaultSave;
    }

    Platinum_DefaultSettings(&settings);
    settings.savePath = savePath;
    /* The lower screen as a menu in the field, as the 3DS's remaster has it
     * (mshot shows it, mtouch touches it). */
    NitroPort_LowerMenuSetAvailable(!classicMenu);
    NitroPort_LowerMenuSetClock(MenuClock);
    if (mapThread && !classicMenu) {
        StartMapThread();
    }

    {
        pthread_t watcher;
        void *warm[2];

        backtrace(warm, 2);   /* loads libgcc now, not inside a signal handler */
        if (sStallSeconds > 0) {
            pthread_create(&watcher, NULL, Watch, NULL);
        }
    }
    if (sGxThread) {
        StartGeometryThread(sGxThread == 1);
    }
    StartSound(); /* sound progresses on its own clock, never game frames */
    Platinum_Run(&settings);
    StopSound();
}
