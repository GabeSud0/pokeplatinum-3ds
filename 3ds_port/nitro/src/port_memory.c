/*
 * The DS's fixed memory areas, as ordinary memory.
 *
 * With SDK_PORT_3DS, the SDK's HW_MAIN_MEM, HW_ITCM, HW_WRAM, palette, OAM and
 * VRAM addresses point here instead of at DS hardware addresses (see
 * patches/NitroSDK, mmap_global.h). Sizes match the DS.
 *
 * About 14 MB in total. The New 3DS has plenty; the PC test build doesn't care.
 */

#include <nitro/types.h>

#define ALIGNED __attribute__((aligned(32)))

/* 8 MB: the SDK's shared/system area lives at offset 0x7FF000-0x7FFFFF. */
unsigned char NitroPort_MainMem[0x800000] ALIGNED;

unsigned char NitroPort_Itcm[0x8000] ALIGNED;
unsigned char NitroPort_Wram[0x8000] ALIGNED;

/* The linker symbol the SDK uses for the start of DTCM (16 KB). */
u32 SDK_AUTOLOAD_DTCM_START[0x4000 / sizeof(u32)] ALIGNED;

/* Palettes: BG A, OBJ A, BG B, OBJ B, 0x200 bytes each. */
unsigned char NitroPort_Pltt[0x800] ALIGNED;

/* OAM: engine A then engine B, 0x400 bytes each. */
unsigned char NitroPort_Oam[0x800] ALIGNED;

/* CPU views of VRAM (see the note in mmap_global.h). */
unsigned char NitroPort_BgVram[0x80000] ALIGNED;
unsigned char NitroPort_DbBgVram[0x20000] ALIGNED;
unsigned char NitroPort_ObjVram[0x40000] ALIGNED;
unsigned char NitroPort_DbObjVram[0x20000] ALIGNED;
/* 4 KB aligned: the texture loader stores LCDC addresses as 4 KB page numbers. */
unsigned char NitroPort_LcdcVram[0xA4000] __attribute__((aligned(4096)));
