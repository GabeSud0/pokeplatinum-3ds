/*
 * Odds and ends the SDK expects from the DS toolchain or linker.
 */

#include <nitro.h>
#include <nitro_port.h>

/* SDK_REFER_SYMBOL: makes sure the linker keeps a symbol. Nothing to do. */
void OSi_ReferSymbol(void *symbol)
{
    (void)symbol;
}

/* Metrowerks' runtime keeps a list of C++ static destructors here; the
 * overlay loader walks it when unloading. There is no C++ in the game. */
void *__global_destructor_chain = NULL;

/* The DS linker writes a table of overlay hashes for download play. The
 * port doesn't load overlays from a cartridge, so the table is empty. */
u8 SDK_OVERLAYTABLE_DIGEST[20];
u8 SDK_OVERLAY_DIGEST[1];
/* ..._END is the same address as the start: zero entries. */
__asm__(".globl SDK_OVERLAY_DIGEST_END\n"
        ".set SDK_OVERLAY_DIGEST_END, SDK_OVERLAY_DIGEST\n");

/* Decompresses an overlay compressed backwards from its end (the DS stores
 * overlays this way). The port links all overlays into the program, so
 * nothing should call this; report it if something does. */
void MIi_UncompressBackward(void *bottom)
{
    (void)bottom;
    NITRO_PORT_UNIMPLEMENTED();
}
