/*
 * C versions of NitroSDK's atomic swap (libraries/mi/src/mi_swap.c, which is
 * one ARM SWP instruction each): store a new value and return the old one,
 * with nothing able to run in between. Used for simple locks.
 */

#include <nitro/types.h>
#include <nitro/mi/swap.h>

u32 MI_SwapWord(u32 setData, volatile u32 *destp)
{
    return __atomic_exchange_n(destp, setData, __ATOMIC_SEQ_CST);
}

u8 MI_SwapByte(u32 setData, volatile u8 *destp)
{
    return __atomic_exchange_n(destp, (u8)setData, __ATOMIC_SEQ_CST);
}
