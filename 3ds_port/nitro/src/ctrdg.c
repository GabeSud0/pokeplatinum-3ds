/*
 * The GBA cartridge slot (NitroSDK's CTRDG library).
 *
 * The game uses it for Pal Park (bringing Pokémon over from GBA games) and
 * for distribution cartridges. The 3DS has no GBA slot, so this answers
 * "no cartridge inserted", which the game already handles; that's the
 * correct behaviour here, not a stand-in.
 */

#include <nitro.h>
#include <nitro_port.h>

void CTRDG_Init(void)
{
}

void CTRDG_Enable(BOOL enable)
{
    (void)enable;
}

BOOL CTRDG_IsExisting(void)
{
    return FALSE;
}

BOOL CTRDG_IsAgbCartridge(void)
{
    return FALSE;
}

BOOL CTRDG_IsPulledOut(void)
{
    return FALSE; /* nothing was ever inserted, so nothing was pulled out */
}

void CTRDG_TerminateForPulledOut(void)
{
}

u16 CTRDG_GetAgbMakerCode(void)
{
    return 0;
}

u32 CTRDG_GetAgbGameCode(void)
{
    return 0;
}

BOOL CTRDG_Read32(const u32 *address, u32 *rdata)
{
    (void)address;
    (void)rdata;
    return FALSE;
}

BOOL CTRDG_CpuCopy8(const void *src, void *dest, u32 size)
{
    (void)src; (void)dest; (void)size;
    return FALSE;
}

BOOL CTRDG_CpuCopy16(const void *src, void *dest, u32 size)
{
    (void)src; (void)dest; (void)size;
    return FALSE;
}

BOOL CTRDG_CpuCopy32(const void *src, void *dest, u32 size)
{
    (void)src; (void)dest; (void)size;
    return FALSE;
}

u16 CTRDG_IdentifyAgbBackup(CTRDGBackupType type)
{
    (void)type;
    return 1; /* non-zero: failed */
}

void CTRDG_ReadAgbFlash(u16 sec_num, u32 offset, u8 *dst, u32 size)
{
    (void)sec_num; (void)offset; (void)dst; (void)size;
    NITRO_PORT_UNIMPLEMENTED(); /* unreachable: no cartridge is ever found */
}

void CTRDG_WriteAndVerifyAgbFlashAsync(u16 sec_num, u8 *src, u32 verifysize, CTRDG_TASK_FUNC callback)
{
    (void)sec_num; (void)src; (void)verifysize; (void)callback;
    NITRO_PORT_UNIMPLEMENTED();
}
