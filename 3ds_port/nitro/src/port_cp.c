/*
 * Software version of the DS's maths coprocessor: the 64-bit divider and the
 * square-root unit.
 *
 * The game and the fx library write operands into the divider registers and
 * read the results back a few cycles later. With no such hardware on the 3DS,
 * the patched SDK headers call these functions after every write, and we
 * compute the results straight away, so they are ready before anyone reads
 * them and the "busy" bit is never set.
 *
 * Edge cases (division by zero, overflow) follow the hardware's documented
 * behaviour (GBATEK, "DS Maths"), because game code may rely on them.
 */

#include <nitro/types.h>
#include <nitro/ioreg.h>
#include <nitro/cp/divider.h>
#include <nitro/cp/sqrt.h>

#define REG64(addr) (*(volatile u64 *)(addr))
#define REG32(addr) (*(volatile u32 *)(addr))

void CPi_PortUpdateDiv(void)
{
    u16 cnt = reg_CP_DIVCNT;
    u32 mode = cnt & REG_CP_DIVCNT_MODE_MASK;
    u64 numerRaw = REG64(REG_DIV_NUMER_ADDR);
    u64 denomRaw = REG64(REG_DIV_DENOM_ADDR);
    s64 numer, denom, quot, rem;

    switch (mode) {
    case 0: /* 32 / 32 */
        numer = (s32)(u32)numerRaw;
        denom = (s32)(u32)denomRaw;
        break;
    case 2: /* 64 / 64 */
        numer = (s64)numerRaw;
        denom = (s64)denomRaw;
        break;
    default: /* 1 and 3: 64 / 32 */
        numer = (s64)numerRaw;
        denom = (s32)(u32)denomRaw;
        break;
    }

    /* The error flag looks at the whole 64-bit denominator, whatever the mode. */
    if (denomRaw == 0) {
        cnt |= REG_CP_DIVCNT_DIV0_MASK;
    } else {
        cnt &= ~REG_CP_DIVCNT_DIV0_MASK;
    }

    if (denom == 0) {
        /* Hardware: remainder = numerator, quotient = -1 (or +1 if the
         * numerator is negative). In 32-bit mode the upper half of the
         * quotient comes out inverted. */
        rem = numer;
        quot = (numer < 0) ? 1 : -1;
        if (mode == 0) {
            quot = (s64)((u64)quot ^ 0xFFFFFFFF00000000ULL);
        }
    } else if (denom == -1 && numer == INT64_MIN) {
        /* Overflow wraps around (only reachable in the 64-bit modes). */
        quot = INT64_MIN;
        rem = 0;
    } else {
        /* C division truncates toward zero, like the hardware. */
        quot = numer / denom;
        rem = numer % denom;
    }

    REG64(REG_DIV_RESULT_ADDR) = (u64)quot;
    REG64(REG_DIVREM_RESULT_ADDR) = (u64)rem;
    reg_CP_DIVCNT = (u16)(cnt & ~REG_CP_DIVCNT_BUSY_MASK);
}

/* floor(sqrt(x)) for any 64-bit x, exactly (no floating point). */
static u32 isqrt64(u64 x)
{
    u64 result = 0;
    u64 bit = 1ULL << 62;

    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= result + bit) {
            x -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (u32)result;
}

void CPi_PortUpdateSqrt(void)
{
    u16 cnt = reg_CP_SQRTCNT;
    u64 param = REG64(REG_SQRT_PARAM_ADDR);

    if ((cnt & REG_CP_SQRTCNT_MODE_MASK) == 0) {
        param = (u32)param; /* 32-bit mode uses the low word only */
    }

    REG32(REG_SQRT_RESULT_ADDR) = isqrt64(param);
    reg_CP_SQRTCNT = (u16)(cnt & ~REG_CP_SQRTCNT_BUSY_MASK);
}
