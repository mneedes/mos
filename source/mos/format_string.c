
// Copyright 2021-2024 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include "mos/defs.h"
#include "mos/format_string.h"

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

/* This structure reduces the required stack depth */
typedef struct {
    /* Format settings */
    u8   base;
    u8   isUpper;
    u8   isSigned;
    u8   isOnLeft;
    u8   minWidth;
    u8   prec;
    char padChar;
    /* State variables */
    u8   doInteger;
    u8   inPrec;
    u8   inArg;
    u8   longCnt;
} State;

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE LLtoa(char * restrict pOut, State * pState, s64 in) {
    u64 adj = (u64)in;
    s32 cnt = 0;
    if (pState->base == 10) {
        if (pState->isSigned && in < 0) {
            adj = -(u64)in;
        }
        /* Determine digits (in reverse order) */
        do {
            *pOut++ = LowerCaseDigits[adj % pState->base];
            adj = adj / pState->base;
            cnt++;
        } while (adj != 0);
    } else {
        u32 shift = 4;
        if (pState->base == 8) shift = 3;
        const char * restrict pDigits = LowerCaseDigits;
        if (pState->isUpper) pDigits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *pOut++ = pDigits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    }
    if (pState->isSigned && in < 0) {
        *pOut++ = '-';
        cnt++;
    }
    /* Reverse digit order in place */
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = pOut[idx - cnt];
        pOut[idx - cnt] = pOut[-idx - 1];
        pOut[-idx - 1] = tmp;
    }
    return cnt;
}

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE Ltoa(char * restrict pOut, State * pState, s32 in) {
    u32 adj = (u32)in;
    s32 cnt = 0;
    if (pState->base == 10) {
        if (pState->isSigned && in < 0) {
            adj = -(u32)in;
        }
        /* Determine digits (in reverse order) */
        do {
            *pOut++ = LowerCaseDigits[adj % pState->base];
            adj = adj / pState->base;
            cnt++;
        } while (adj != 0);
    } else {
        u32 shift = 4;
        if (pState->base == 8) shift = 3;
        const char * restrict pDigits = LowerCaseDigits;
        if (pState->isUpper) pDigits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *pOut++ = pDigits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    }
    if (pState->isSigned && in < 0) {
        *pOut++ = '-';
        cnt++;
    }
    /* Reverse digit order in place */
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = pOut[idx - cnt];
        pOut[idx - cnt] = pOut[-idx - 1];
        pOut[-idx - 1] = tmp;
    }
    return cnt;
}

u32 mosItoa(char * restrict pOut, s32 input, u16 base, bool isUpper,
            u16 minWidth, char padChar, bool isSigned) {
    State format = {
        .base = base, .isUpper = isUpper, .minWidth = minWidth,
        .padChar = padChar, .isSigned = isSigned
    };
    return LLtoa(pOut, &format, input);
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
        for (u32 nIdx = 0; nIdx < pState->prec + 1u; nIdx++) round *= 0.1;
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
    if (pState->isSigned) {
        u32 nDigits = pState->prec + 6;
        if (nExp10 >= 100 || nExp10 <= -100) nDigits++;
        /* Back out trailing zeros (%g option) */
        while (1) {
            if (pOut[-1] == '0') {
                pOut--;
                nDigits--;
                continue;
            } else {
                if (pOut[-1] == '.') pOut--;
                break;
            }
        }
#if 0
        u32 nDigitsR;
        if (nExp10 >= 0) {
            if (nExp10 < nDigits - 6) nDigitsR = nDigits - 4;
            else nDigitsR = nExp10 + 1;
        } else {
            nDigitsR = nDigits - 4 - nExp10;
        }
#endif
    }
    /* Exponent */
    *pOut++ = 'e';
    if (nExp10 < 0) {
        *pOut++ = '-';
        nExp10 = -nExp10;
    } else {
        *pOut++ = '+';
    }
    if (nExp10 < 10) *pOut++ = '0';
    pState->base       = 10;
    pState->isSigned   = false;
    pState->minWidth   = 2;
    pState->padChar    = '0';
    pOut += LLtoa(pOut, pState, nExp10);
    return pOut - pOut_;
}

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE DtoaF(char * restrict pOut, State * pState, double in) {
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
    pState->base       = 10;
    pState->isSigned   = false;
    pState->minWidth   = 0;
    pState->padChar    = '0';
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

#if 0
/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE DtoaG(char * restrict pOut, State * pState, double in) {
   /* Go with f unless total length of e is smaller.
   *  if go with e then truncate zeros and subtract 1 from precision */
   /* 1.3e10 -> 1.3e10,  3e0 -> 3 */
   return 0;
}
#endif

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
mosVSNPrintf(char * restrict pDest, mos_size size, const char * restrict pFmt, va_list args) {
    const char * restrict pCh = pFmt;
    char * restrict pOut = pDest;
    s32 rem = (s32)--size;
    State state = { .inArg = false };
    for (; *pCh != '\0'; pCh++) {
        if (!state.inArg) {
            if (*pCh == '%') {
                /* Found argument, set default state */
                state.isOnLeft = false;
                state.minWidth = 0;
                state.prec     = 6;
                state.padChar  = ' ';
                state.inArg    = true;
                state.inPrec   = false;
                state.longCnt  = 0;
            } else WriteBuf(&pOut, pCh, 1, &rem);
        } else if (*pCh >= '0' && *pCh <= '9') {
            if (!state.inPrec) {
                if (state.minWidth == 0 && *pCh == '0' && !state.isOnLeft) state.padChar = '0';
                else state.minWidth = (10 * state.minWidth) + (*pCh - '0');
            } else {
                state.prec = (10 * state.prec) + (*pCh - '0');
            }
        } else {
            /* Argument will be consumed (unless modifier found) */
            state.inArg     = false;
            state.doInteger = false;
            switch (*pCh) {
            case '%': {
                char c = '%';
                WriteBuf(&pOut, &c, 1, &rem);
                break;
            }
            case '.': {
                state.prec   = 0;
                state.inPrec = true;
                state.inArg  = true;
                break;
            }
            case '-':
                state.isOnLeft = true;
                state.inArg    = true;
                break;
            case '+':
                /* Ignore + */
                state.inArg = true;
                break;
            case 'c': {
                char c = (char)va_arg(args, int);
                WriteBuf(&pOut, &c, 1, &rem);
                break;
            }
            case 's': {
                char * pArg = va_arg(args, char *);
                s32 cnt = state.minWidth;
                char * p = pArg;
                for (; *p != '\0'; p++, cnt--) {
                    if (state.isOnLeft) WriteBuf(&pOut, p, 1, &rem);
                }
                for (; cnt > 0; cnt--) WriteBuf(&pOut, &state.padChar, 1, &rem);
                if (!state.isOnLeft) WriteBuf(&pOut, pArg, p - pArg, &rem);
                break;
            }
            case 'l':
                state.longCnt++;
                state.inArg       = true;
                break;
            case 'o':
                state.base        = 8;
                state.isSigned    = false;
                state.doInteger   = true;
                break;
            case 'd':
                state.base        = 10;
                state.isSigned    = true;
                state.doInteger   = true;
                break;
            case 'u':
                state.base        = 10;
                state.isSigned    = false;
                state.doInteger   = true;
                break;
            case 'x':
                state.base        = 16;
                state.isUpper     = false;
                state.isSigned    = false;
                state.doInteger   = true;
                break;
            case 'X':
                state.base        = 16;
                state.isUpper     = true;
                state.isSigned    = false;
                state.doInteger   = true;
                break;
            case 'e': {
                double argD = (double)va_arg(args, double);
                char tmpD[20];
                if (state.prec > 12) state.prec = 12;
                state.isSigned = false;
                u32 cnt = DtoaE(tmpD, &state, argD);
                WriteBuf(&pOut, tmpD, cnt, &rem);
                break;
            }
            case 'f': {
                double argD = (double)va_arg(args, double);
                char tmpD[25];
                if (state.prec > 17) state.prec = 17;
                u32 cnt = DtoaF(tmpD, &state, argD);
                WriteBuf(&pOut, tmpD, cnt, &rem);
                break;
            }
            case 'g': {
                double argD = (double)va_arg(args, double);
                char tmpD[20];
                if (state.prec > 13) state.prec = 12;
                else if (state.prec != 0) state.prec--; 
                state.isSigned = true;
                u32 cnt = DtoaE(tmpD, &state, argD);
                WriteBuf(&pOut, tmpD, cnt, &rem);
                break;
            }
            case 'p':
            case 'P': {
                s64 arg64 = (u32)va_arg(args, u32 *);
                char tmp[8];
                state.base       = 16;
                state.isUpper    = (*pCh == 'P');
                state.isSigned   = false;
                state.minWidth   = 8;
                state.padChar    = '0';
                u32 cnt = LLtoa(tmp, &state, arg64);
                WriteBuf(&pOut, tmp, cnt, &rem);
                break;
            }
            default:
                break;
            }
            /* Convert numeric types to text */
            if (state.doInteger) {
                char tmp[22];
                u32 cnt;
                if (state.longCnt <= 1) {
                    s32 arg32 = va_arg(args, s32);
                    cnt = Ltoa(tmp, &state, arg32);
                } else {
                    s64 arg64 = va_arg(args, s64);
                    cnt = LLtoa(tmp, &state, arg64);
                }
                char * p = tmp;
                if (state.isOnLeft) {
                    WriteBuf(&pOut, p, cnt, &rem);
                }
                s32 padCnt = state.minWidth - cnt;
                if (state.padChar == '0' && tmp[0] == '-') {
                    WriteBuf(&pOut, p++, 1, &rem);
                    cnt--;
                }
                for (; padCnt > 0; padCnt--) {
                    WriteBuf(&pOut, &state.padChar, 1, &rem);
                }
                if (!state.isOnLeft) {
                    WriteBuf(&pOut, p, cnt, &rem);
                }
            }
        }
    }
    *pOut = '\0';
    return (s32)size - rem;
}

s32
mosSNPrintf(char * restrict pDest, mos_size size, const char * restrict pFmt, ...) {
    va_list args;
    va_start(args, pFmt);
    s32 cnt = mosVSNPrintf(pDest, size, pFmt, args);
    va_end(args);
    return cnt;
}
