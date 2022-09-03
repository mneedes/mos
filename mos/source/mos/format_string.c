
// Copyright 2021-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include "mos/defs.h"
#include "mos/format_string.h"

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

/* This structure reduces the required stack depth */
typedef struct {
    // Format settings
    u8   base;
    u8   isUpper;
    u8   isSigned;
    u8   minWidth;
    u8   prec;
    char padChar;
    // State variables
    u8   doNumeric;
    u8   inPrec;
    u8   inArg;
    u8   longCnt;
} State;

// No-inline prevents inadvertent entry into lazy-stacking floating point modes
static u32 MOS_NO_INLINE LLtoa(char * restrict pOut, State * pState, s64 in) {
    u64 adj = (u64)in;
    u8 shift = 0;
    switch (pState->base) {
    case 8:
        shift = 3;
        break;
    case 16:
        shift = 4;
        break;
    }
    s32 cnt = 0;
    if (shift) {
        const char * restrict digits = LowerCaseDigits;
        if (pState->isUpper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *pOut++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (pState->isSigned && in < 0) adj = (u64)-in;
        // Determine digits (in reverse order)
        do {
            *pOut++ = LowerCaseDigits[adj % pState->base];
            adj = adj / pState->base;
            cnt++;
        } while (adj != 0);
    }
    // Pad to minimum number of digits
    for (; cnt < pState->minWidth; cnt++) *pOut++ = pState->padChar;
    // Write sign
    if (pState->isSigned && in < 0) {
        *pOut++ = '-';
        cnt++;
    }
    // Reverse digit order in place
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = pOut[idx - cnt];
        pOut[idx - cnt] = pOut[-idx - 1];
        pOut[-idx - 1] = tmp;
    }
    return cnt;
}

static u32 Itoa(char * restrict pOut, State * pState, s32 in) {
    u32 adj = (u32)in;
    u8 shift = 0;
    switch (pState->base) {
    case 8:
        shift = 3;
        break;
    case 16:
        shift = 4;
        break;
    }
    s32 cnt = 0;
    if (shift) {
        const char * restrict digits = LowerCaseDigits;
        if (pState->isUpper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *pOut++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (pState->isSigned && in < 0) adj = (u32)-in;
        // Determine digits (in reverse order)
        do {
            *pOut++ = LowerCaseDigits[adj % pState->base];
            adj = adj / pState->base;
            cnt++;
        } while (adj != 0);
    }
    // Pad to minimum number of digits
    for (; cnt < pState->minWidth; cnt++) *pOut++ = pState->padChar;
    // Write sign
    if (pState->isSigned && in < 0) {
        *pOut++ = '-';
        cnt++;
    }
    // Reverse digit order in place
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = pOut[idx - cnt];
        pOut[idx - cnt] = pOut[-idx - 1];
        pOut[-idx - 1] = tmp;
    }
    return cnt;
}

u32 MosItoa(char * restrict pOut, s32 input, u16 base, bool isUpper,
            u16 minWidth, char padChar, bool isSigned) {
    State format = {
        .base = base, .isUpper = isUpper, .minWidth = minWidth,
        .padChar = padChar, .isSigned = isSigned
    };
    return Itoa(pOut, &format, input);
}

// No-inline prevents inadvertent entry into lazy-stacking floating point modes
static u32 MOS_NO_INLINE Dtoa(char * restrict pOut, State * pState, double in) {
    char * pOut_ = pOut;
    u64 * pIn = (u64 *)&in;
    bool negative = false;
    if (*pIn >> 63 == 0x1) {
        negative = true;
        in = -in;
        *pOut++ = '-';
    }
    /* First evaluate special values (e.g.: nan / inf) */
    u64 in_ = *pIn;
    u32 pfx = (u32)(in_ >> 32);
    if ((pfx & 0x7ff00000) == 0x7ff00000) {
        u32 sfx = (u32)(in_ & 0xffffffff);
        if (sfx == 0x0) {
            if (pfx == 0x7ff00000) {
                pOut[0] = 'i'; pOut[1] = 'n'; pOut[2] = 'f';
            } else {
                pOut[0] = 'n'; pOut[1] = 'a'; pOut[2] = 'n';
            }
            return negative + 3;
        } else {
            pOut[0] = 'n'; pOut[1] = 'a'; pOut[2] = 'n';
            return negative + 3;
        }
    }
    /* Round, get integer part and overflow clamp */
    double p = 0.5;
    for (u32 ix = 0; ix < pState->prec; ix++) p *= 0.1;
    in += p;
    s64 int_part = (s64)in;
    in -= (double)int_part;
    if (in >= (double)0xffffffffffffffff) {
        pOut[0] = 'o'; pOut[1] = 'v'; pOut[2] = 'f';
        return negative + 3;
    }
    pState->base      = 10;
    pState->isSigned = false;
    pState->minWidth = 0;
    pState->padChar  = '0';
    pOut += LLtoa(pOut, pState, int_part);
    /* Get fractional part */
    if (pState->prec) {
        *pOut++ = '.';
        u64 val = 1;
        for (u32 ix = 0; ix < pState->prec; ix++) val *= 10;
        in *= (double)val;
        int_part = (s64)in;
        pState->minWidth = pState->prec;
        pOut += LLtoa(pOut, pState, int_part);
    }
    return pOut - pOut_;
}

/*
 * A pre-scaling algorithm is used in lieu of arbitrary precision arithmetic for
 *   faster conversion time. Scaling does however introduce small rounding errors,
 *   but is generally good to around 10 digits beyond the decimal point.
 */
static const double s_nScaleUp[] = {
    1e1, 1e2, 1e3, 1e5, 1e10, 1e20, 1e39, 1e78, 1e155, 1e155
};

static const double s_nScaleDown[] = {
    1e-1, 1e-2, 1e-3, 1e-5, 1e-10, 1e-20, 1e-39, 1e-78, 1e-155, 1e-155
};

static const u8 s_nScaleExp10[] = {
    1, 2, 3, 5, 10, 20, 39, 78, 155, 155
};

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE DtoaE(char * restrict pOut, State * pState, double in) {
    char * pOut_ = pOut;
    u64 * pIn = (u64 *)&in;
    bool negative = false;
    /* Detect zero and negative zero */
    if (*pIn >> 63 == 0x1) {
        negative = true;
        in = -in;
        *pOut++ = '-';
    }
    /* First evaluate special values (e.g.: NaN / Inf) */
    u64 in_ = *pIn;
    u32 pfx = (u32)(in_ >> 32);
    if ((pfx & 0x7ff00000) == 0x7ff00000) {
        u32 sfx = (u32)(in_ & 0xffffffff);
        if (sfx == 0x0) {
            if (pfx == 0x7ff00000) {
                pOut[0] = 'i'; pOut[1] = 'n'; pOut[2] = 'f';
            } else {
                pOut[0] = 'n'; pOut[1] = 'a'; pOut[2] = 'n';
            }
            return negative + 3;
        } else {
            pOut[0] = 'n'; pOut[1] = 'a'; pOut[2] = 'n';
            return negative + 3;
        }
    }
    /* Pre-scale up or down and determine base10 exponent */
    s32 nExp10 = -1;
    u64 nMant;
    if (*pIn) {
        s32 nExp = (s32)(*pIn >> 52) - 1023;
        while (nExp < -3) {
            nExp >>= 2;
            u32 nIdx = 32 - (u32)__builtin_clz(-nExp);
            in *= s_nScaleUp[nIdx];
            nExp10 -= s_nScaleExp10[nIdx];
            nExp = (s32)(*pIn >> 52) - 1023;
        }
        while (nExp >= 0) {
            nExp >>= 2;
            u32 nIdx = 0;
            if (nExp) nIdx = 32 - (u32)__builtin_clz(nExp);
            in *= s_nScaleDown[nIdx];
            nExp10 += s_nScaleExp10[nIdx];
            nExp = (s32)(*pIn >> 52) - 1023;
        }
        /* Round and check if additional scale down is needed */
        double round = 0.5;
        for (u32 nIdx = 0; nIdx < pState->prec + 1; nIdx++) round *= 0.1;
        in += round;
        if (in >= 1.) {
            in *= 1e-1;
            nExp10 += 1;
            nExp = (s32)(*pIn >> 52) - 1023;
        }
        /* Initialize mantissa adding implied '1' */
        nMant = (*pIn & 0x000fffffffffffff) | (0x0010000000000000);
        nMant <<= (8 + nExp);
        nMant *= 10;
    } else {
        /* Zero (and indeed negative zero) are special cases */
        nExp10 = 0;
        nMant = 0;
    }
    /* Convert mantissa to decimal */
    char nCh = (nMant >> 60) + '0';
    nMant &= 0x0fffffffffffffff;
    *pOut++ = nCh;
    *pOut++ = '.';
    for (u32 nIx = 0; nIx < pState->prec; nIx++) {
        nMant *= 10;
        nCh = (nMant >> 60) + '0';
        nMant &= 0x0fffffffffffffff;
        *pOut++ = nCh;
    }
    /* Exponent */
    *pOut++ = 'e';
    if (nExp10 >= 0) *pOut++ = '+';
    pState->base      = 10;
    pState->isSigned = true;
    pState->minWidth = 2;
    pState->padChar  = '0';
    pOut += Itoa(pOut, pState, nExp10);
    return pOut - pOut_;
}

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE DtoaG(char * restrict pOut, State * pState, double in) {
   /* Go with f unless e mantissa is smaller.  If go with e then truncate zeros and subtract 1 from precision */
   /* 1.3e10 -> 1.3e10,  3e0 -> 3 */
   return 0;
}

static void
WriteBuf(char * restrict * pOut, const char * restrict pIn, s32 len, s32 * pRem) {
    u32 cnt = len;
    if (len > *pRem) {
        if (*pRem < 0) cnt = 0;
        else cnt = *pRem;
    }
    *pRem -= len;
    for (; cnt > 0; cnt--) {
        **pOut = *pIn++;
        (*pOut)++;
    }
}

s32
MosVSNPrintf(char * restrict pDest, mos_size size, const char * restrict pFmt, va_list args) {
    const char * restrict pCh = pFmt;
    char * restrict pOut = pDest;
    s32 rem = (s32)--size;
    State state = { .inArg = false };
    for (; *pCh != '\0'; pCh++) {
        if (!state.inArg) {
            if (*pCh == '%') {
                /* Found argument, set default state */
                state.minWidth = 0;
                state.prec      = 6;
                state.padChar  = ' ';
                state.inArg    = true;
                state.inPrec   = false;
                state.longCnt  = 0;
            } else WriteBuf(&pOut, pCh, 1, &rem);
        } else if (*pCh >= '0' && *pCh <= '9') {
            if (!state.inPrec) {
                if (state.minWidth == 0 && *pCh == '0') state.padChar = '0';
                else state.minWidth = (10 * state.minWidth) + (*pCh - '0');
            } else {
                state.prec = (10 * state.prec) + (*pCh - '0');
            }
        } else {
            /* Argument will be consumed (unless modifier found) */
            state.inArg     = false;
            state.doNumeric = false;
            switch (*pCh) {
            case '%': {
                char c = '%';
                WriteBuf(&pOut, &c, 1, &rem);
                break;
            }
            case '.': {
                state.prec    = 0;
                state.inPrec = true;
                state.inArg  = true;
                break;
            }
            case 'c': {
                char c = (char) va_arg(args, int);
                WriteBuf(&pOut, &c, 1, &rem);
                break;
            }
            case 's': {
                char * arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++)
                    WriteBuf(&pOut, arg, 1, &rem);
                break;
            }
            case 'l':
                state.longCnt++;
                state.inArg     = true;
                break;
            case 'o':
                state.base       = 8;
                state.isSigned  = false;
                state.doNumeric = true;
                break;
            case 'd':
                state.base       = 10;
                state.isSigned  = true;
                state.doNumeric = true;
                break;
            case 'u':
                state.base       = 10;
                state.isSigned  = false;
                state.doNumeric = true;
                break;
            case 'x':
                state.base       = 16;
                state.isUpper   = false;
                state.isSigned  = false;
                state.doNumeric = true;
                break;
            case 'X':
                state.base       = 16;
                state.isUpper   = true;
                state.isSigned  = false;
                state.doNumeric = true;
                break;
            case 'e': {
                double argD = (double)va_arg(args, double);
                // TODO: Limit precision to obtain 25 (or less) digits: (-x.xxxxxxxxxxxxxxxxxe+xxx)
                char tmpD[25];
                u32 cnt = DtoaE(tmpD, &state, argD);
                WriteBuf(&pOut, tmpD, cnt, &rem);
                break;
            }
            case 'f': {
                double argD = (double)va_arg(args, double);
                // TODO: Limit precision to obtain 25 (or less) digits: (-x.xxxxxxxxxxxxxxxxxe+xxx)
                char tmpD[25];
                u32 cnt = Dtoa(tmpD, &state, argD);
                WriteBuf(&pOut, tmpD, cnt, &rem);
                break;
            }
            case 'g': {
                double argD = (double)va_arg(args, double);
                // TODO: Limit precision to obtain 25 (or less) digits: (-x.xxxxxxxxxxxxxxxxxe+xxx)
                char tmpD[25];
                u32 cnt = DtoaG(tmpD, &state, argD);
                WriteBuf(&pOut, tmpD, cnt, &rem);
                break;
            }
            case 'p':
            case 'P': {
                s32 arg32 = (u32)va_arg(args, u32 *);
                char tmp32[8];
                state.base      = 16;
                state.isUpper  = (*pCh == 'P');
                state.isSigned = false;
                state.minWidth = 8;
                state.padChar  = '0';
                u32 cnt = Itoa(tmp32, &state, arg32);
                WriteBuf(&pOut, tmp32, cnt, &rem);
                break;
            }
            default:
                break;
            }
            /* Convert numeric types to text */
            if (state.doNumeric) {
                if (state.longCnt <= 1) {
                    s32 arg32 = va_arg(args, s32);
                    char tmp32[11];
                    u32 cnt = Itoa(tmp32, &state, arg32);
                    WriteBuf(&pOut, tmp32, cnt, &rem);
                } else {
                    s64 arg64 = va_arg(args, s64);
                    char tmp64[22];
                    u32 cnt = LLtoa(tmp64, &state, arg64);
                    WriteBuf(&pOut, tmp64, cnt, &rem);
                }
            }
        }
    }
    *pOut = '\0';
    return (s32)size - rem;
}

s32
MosSNPrintf(char * restrict pDest, mos_size size, const char * restrict pFmt, ...) {
    va_list args;
    va_start(args, pFmt);
    s32 cnt = MosVSNPrintf(pDest, size, pFmt, args);
    va_end(args);
    return cnt;
}
