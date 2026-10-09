/*
 * The DS BIOS ("SVC" system calls), in C.
 *
 * On the DS these are routines in the console's boot ROM, reached with the
 * SWI instruction (pokeplatinum gets the entry points from libsyscall). The
 * 3DS has no DS BIOS in native mode, so here they are as plain functions,
 * following GBATEK's description of each call.
 */

#include <nitro.h>
#include <nitro_port.h>

void SVC_WaitByLoop(s32 count)
{
    /* A timing delay for DS hardware; nothing on the 3DS needs it. */
    (void)count;
}

/* CpuSet: bits 0-20 of the control word are the number of units, bit 24 =
 * fill with the first source unit instead of copying, bit 26 = 32-bit units
 * (else 16-bit). */
void SVC_CpuSet(const void *srcp, void *destp, u32 dmaCntData)
{
    u32 count = dmaCntData & 0x1FFFFF;
    BOOL fill = (dmaCntData & (1u << 24)) != 0;
    u32 i;

    if (dmaCntData & (1u << 26)) {
        const u32 *s = srcp;
        u32 *d = destp;
        for (i = 0; i < count; i++) {
            d[i] = fill ? s[0] : s[i];
        }
    } else {
        const u16 *s = srcp;
        u16 *d = destp;
        for (i = 0; i < count; i++) {
            d[i] = fill ? s[0] : s[i];
        }
    }
}

/* CpuFastSet: always 32-bit units, in blocks of 8 words (the count is rounded
 * up to a multiple of 8, as the BIOS does). */
void SVC_CpuSetFast(const void *srcp, void *destp, u32 dmaCntData)
{
    u32 count = ((dmaCntData & 0x1FFFFF) + 7) & ~7u;
    BOOL fill = (dmaCntData & (1u << 24)) != 0;
    const u32 *s = srcp;
    u32 *d = destp;
    u32 i;

    for (i = 0; i < count; i++) {
        d[i] = fill ? s[0] : s[i];
    }
}

void SVC_UnpackBits(const void *srcp, void *destp, const MIUnpackBitsParam *paramp)
{
    MI_UnpackBits(srcp, destp, (MIUnpackBitsParam *)paramp);
}

void SVC_UncompressLZ8(const void *srcp, void *destp)
{
    MI_UncompressLZ8(srcp, destp);
}

void SVC_UncompressRL8(const void *srcp, void *destp)
{
    MI_UncompressRL8(srcp, destp);
}

s32 SVC_UncompressLZ16FromDevice(const void *srcp, void *destp, const void *paramp,
                                 const MIReadStreamCallbacks *callbacks)
{
    (void)srcp; (void)destp; (void)paramp; (void)callbacks;
    NITRO_PORT_UNIMPLEMENTED();
    return -1;
}

s32 SVC_UncompressRL16FromDevice(const void *srcp, void *destp, const void *paramp,
                                 const MIReadStreamCallbacks *callbacks)
{
    (void)srcp; (void)destp; (void)paramp; (void)callbacks;
    NITRO_PORT_UNIMPLEMENTED();
    return -1;
}

s32 SVC_UncompressHuffmanFromDevice(const void *srcp, void *destp, u8 *tableBufp,
                                    const MIReadStreamCallbacks *callbacks)
{
    (void)srcp; (void)destp; (void)tableBufp; (void)callbacks;
    NITRO_PORT_UNIMPLEMENTED();
    return -1;
}

/* CRC-16 with the reflected polynomial 0xA001 (the same CRC the DS uses for
 * cartridge headers), starting from `start`, over `size` bytes. */
u16 SVC_GetCRC16(u32 start, const void *datap, u32 size)
{
    const u8 *p = datap;
    u32 crc = start & 0xFFFF;
    u32 i;
    int bit;

    for (i = 0; i < size; i++) {
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return (u16)crc;
}

BOOL SVC_IsMmemExpanded(void)
{
    return FALSE; /* retail DS: 4 MB */
}

s32 SVC_Div(s32 number, s32 denom)
{
    return denom ? number / denom : 0;
}

s32 SVC_DivRem(s32 number, s32 denom)
{
    return denom ? number % denom : number;
}

u16 SVC_Sqrt(u32 src)
{
    u32 result = 0;
    u32 bit = 1u << 30;

    while (bit > src) {
        bit >>= 2;
    }
    while (bit) {
        if (src >= result + bit) {
            src -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (u16)result;
}

void SVC_Halt(void)
{
    OS_Halt();
}

void SVC_Sleep(void)
{
    NITRO_PORT_UNIMPLEMENTED();
}

void SVC_SetSoundBias(s32 stepLoops)
{
    (void)stepLoops; /* speaker bias: handled by the 3DS sound hardware */
}

void SVC_ResetSoundBias(s32 stepLoops)
{
    (void)stepLoops;
}
