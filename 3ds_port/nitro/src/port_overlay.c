/*
 * Overlays.
 *
 * On the DS, much of a game's code lives in overlays: pieces loaded from the
 * cartridge into a shared area of memory when needed, over whatever was
 * there before. Loading one reads its code and initialised data afresh and
 * clears its zero-initialised data (FS_LoadOverlay), so every load starts
 * from the overlay's initial state.
 *
 * In the port, every overlay's code is linked into the program and stays
 * there. What has to be kept is the fresh start: the game build gathers each
 * overlay's data into sections of its own (tools/game_overlays.py), and the
 * program hands their bounds to NitroPort_SetOverlays. A copy of the initial
 * data is taken then; loading an overlay copies it back and clears the
 * zero-initialised part.
 *
 * The SDK's overlay functions (fs_overlay.c) call the functions below
 * instead of reading the cartridge. The overlay "addresses" they report are
 * only labels (the game's overlay manager compares them to see which
 * overlays would overlap): each overlay gets its own range, inside the
 * ITCM or DTCM blocks for overlays that the DS loaded there.
 */

#include <nitro.h>
#include <nitro_port.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const NitroPortOverlay *sOverlays;
static u32 sCount;
static u8 **sInitialData;
static BOOL *sLoaded;

void NitroPort_SetOverlays(const NitroPortOverlay *overlays, u32 count)
{
    u32 i;

    free(sInitialData);
    free(sLoaded);
    sOverlays = overlays;
    sCount = count;
    sInitialData = calloc(count ? count : 1, sizeof(*sInitialData));
    sLoaded = calloc(count ? count : 1, sizeof(*sLoaded));
    if (sInitialData == NULL || sLoaded == NULL) {
        NitroPort_Log("[nitro] out of memory for the overlay table");
        abort();
    }
    for (i = 0; i < count; i++) {
        const NitroPortOverlay *o = &overlays[i];
        size_t size = (size_t)(o->dataEnd - o->dataStart);

        if (size > 0) {
            sInitialData[i] = malloc(size);
            if (sInitialData[i] == NULL) {
                NitroPort_Log("[nitro] out of memory for overlay %s's data", o->name);
                abort();
            }
            memcpy(sInitialData[i], o->dataStart, size);
        }
        /* Not loaded yet. The program's loader may not have cleared this
         * memory (it lies past the C runtime's bss). */
        if (o->bssEnd > o->bssStart) {
            memset(o->bssStart, 0, (size_t)(o->bssEnd - o->bssStart));
        }
    }
}

u32 NitroPort_OverlayCount(void)
{
    return sCount;
}

BOOL NitroPort_IsOverlayLoaded(u32 id)
{
    return id < sCount && sLoaded[id];
}

static u32 LabelAddress(u32 id)
{
    switch (sOverlays[id].region) {
    case NITRO_PORT_OVERLAY_ITCM:
        return HW_ITCM;
    case NITRO_PORT_OVERLAY_DTCM:
        return HW_DTCM;
    default:
        /* 1 MB apart, from 0x40000000: no memory the port uses on the 3DS
         * or the PC, and never read or written. */
        return 0x40000000u + (id << 20);
    }
}

BOOL NitroPort_OverlayInfo(FSOverlayInfo *p_ovi, MIProcessor target, FSOverlayID id)
{
    if (target != MI_PROCESSOR_ARM9 || id >= sCount) {
        return FALSE;
    }
    memset(p_ovi, 0, sizeof(*p_ovi));
    p_ovi->header.id = id;
    p_ovi->header.ram_address = (u8 *)(uintptr_t)LabelAddress(id);
    p_ovi->header.ram_size = 0x100;   /* any size under 1 MB keeps them apart */
    p_ovi->header.bss_size = 0;
    p_ovi->header.file_id = id;
    p_ovi->target = MI_PROCESSOR_ARM9;
    return TRUE;
}

void NitroPort_OverlayClearBss(const FSOverlayInfo *p_ovi)
{
    const NitroPortOverlay *o;

    if (p_ovi->header.id >= sCount) {
        return;
    }
    o = &sOverlays[p_ovi->header.id];
    if (o->bssEnd > o->bssStart) {
        memset(o->bssStart, 0, (size_t)(o->bssEnd - o->bssStart));
    }
}

BOOL NitroPort_OverlayLoadImage(const FSOverlayInfo *p_ovi)
{
    u32 id = p_ovi->header.id;
    const NitroPortOverlay *o;

    if (id >= sCount) {
        return FALSE;
    }
    o = &sOverlays[id];
    if (sInitialData[id] != NULL) {
        memcpy(o->dataStart, sInitialData[id], (size_t)(o->dataEnd - o->dataStart));
    }
    NitroPort_OverlayClearBss(p_ovi);
    sLoaded[id] = TRUE;
    NitroPort_Log("[nitro] overlay %s loaded", o->name);
    return TRUE;
}

void NitroPort_OverlayUnloaded(const FSOverlayInfo *p_ovi)
{
    u32 id = p_ovi->header.id;

    if (id < sCount) {
        sLoaded[id] = FALSE;
    }
}
