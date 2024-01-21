
// Copyright 2021-2024 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include "mos/defs.h"
#include "mos/format_string.h"

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

enum Flags {
    kIs64bit     = 0x2,
    kIsUpper     = 0x4,
    kIsSigned    = 0x8,
    kPadOnRight  = 0x10,
    kIsInteger   = 0x20,
    kIsBase16    = 0x40,
};

/* This structure reduces the required stack depth */
typedef struct {
    char * pOut;
    s16    rem;
    u8     flags;
    u8     minWidth;
    u8     prec;
    char   padChar;
    u8     inPrec;
    u8     inArg;
} State;

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE LLtoa(char * pOut, State * pState, s64 in) {
    u64 adj = (u64)in;
    s32 cnt = 0;
    if (pState->flags & kIsBase16) {
        const char * pDigits = LowerCaseDigits;
        if (pState->flags & kIsUpper) pDigits = UpperCaseDigits;
        do {
            *pOut++ = pDigits[adj & 0xf];
            adj >>= 4;
            cnt++;
        } while (adj != 0);
    } else {
        if ((pState->flags & kIsSigned) && in < 0) {
            adj = -(u64)in;
            *pOut++ = '-';
        } else pState->flags &= ~kIsSigned;
        /* Determine digits (in reverse order) */
        do {
            *pOut++ = LowerCaseDigits[adj % 10];
            adj = adj / 10;
            cnt++;
        } while (adj != 0);
    }
    /* Reverse digit order in place */
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = pOut[idx - cnt];
        pOut[idx - cnt] = pOut[-idx - 1];
        pOut[-idx - 1] = tmp;
    }
    return cnt + ((pState->flags & kIsSigned) > 0);
}

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE Ltoa(char * pOut, State * pState, s32 in) {
    u32 adj = (u32)in;
    s32 cnt = 0;
    if (pState->flags & kIsBase16) {
        const char * pDigits = LowerCaseDigits;
        if (pState->flags & kIsUpper) pDigits = UpperCaseDigits;
        do {
            *pOut++ = pDigits[adj & 0xf];
            adj >>= 4;
            cnt++;
        } while (adj != 0);
    } else {
        if ((pState->flags & kIsSigned) && in < 0) {
            adj = -(u32)in;
            *pOut++ = '-';
        } else pState->flags &= ~kIsSigned;
        /* Determine digits (in reverse order) */
        do {
            *pOut++ = LowerCaseDigits[adj % 10];
            adj = adj / 10;
            cnt++;
        } while (adj != 0);
    }
    /* Reverse digit order in place */
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = pOut[idx - cnt];
        pOut[idx - cnt] = pOut[-idx - 1];
        pOut[-idx - 1] = tmp;
    }
    return cnt + ((pState->flags & kIsSigned) > 0);
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
static u32 MOS_NO_INLINE DtoaE(char * pOut, State * pState, double in) {
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
    if (pState->flags & kIsSigned) {
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
    pOut += Ltoa(pOut, pState, nExp10);
    return pOut - pOut_;
}

/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE DtoaF(char * pOut, State * pState, double in) {
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
    pOut += LLtoa(pOut, pState, int_part);
    /* Get fractional part */
    if (pState->prec) {
        *pOut++ = '.';
        u64 val = 1;
        for (u32 ix = 0; ix < pState->prec; ix++) val *= 10;
        in *= (double)val;
        int_part = (s64)in;
        /* Run Ltoa to determine amount of padding, then pad and run Ltoa again */
        u32 cnt = LLtoa(pOut, pState, int_part);
        for (s32 padCnt = pState->prec - cnt; padCnt > 0; padCnt--) {
            *pOut++ = '0';
        }
        pOut += LLtoa(pOut, pState, int_part);
    }
    return pOut - pOut_;
}

#if 0
/* No-inline prevents inadvertent entry into lazy-stacking floating point modes */
static u32 MOS_NO_INLINE DtoaG(char * pOut, State * pState, double in) {
   /* Go with f unless total length of e is smaller.
   *  if go with e then truncate zeros and subtract 1 from precision */
   /* 1.3e10 -> 1.3e10,  3e0 -> 3 */
   return 0;
}
#endif

static void
WriteBuf(const char * pIn, State * pState, s32 len) {
    u16 cnt = len;
    if (len > pState->rem) {
        if (pState->rem < 0) cnt = 0;
        else cnt = pState->rem;
    }
    pState->rem -= len;
    for (; cnt > 0; cnt--) {
        *pState->pOut++ = *pIn++;
    }
}

s32
mosVSNPrintf(char * pDest, mos_size size, const char * pFmt, va_list args) {
    State state = {
        .pOut = pDest,
        .rem = (s16)--size,
        .inArg = false
    };
    for (const char * pCh = pFmt; *pCh != '\0'; pCh++) {
        if (!state.inArg) {
            if (*pCh == '%') {
                /* Found argument, set default state */
                state.flags    = 0;
                state.minWidth = 0;
                state.prec     = 6;
                state.padChar  = ' ';
                state.inArg    = true;
                state.inPrec   = false;
            } else WriteBuf(pCh, &state, 1);
        } else if (*pCh >= '0' && *pCh <= '9') {
            if (!state.inPrec) {
                if (state.minWidth == 0 && *pCh == '0' && !(state.flags & kPadOnRight)) {
                    state.padChar = '0';
                } else {
                    state.minWidth = (10 * state.minWidth) + (*pCh - '0');
                }
            } else {
                state.prec = (10 * state.prec) + (*pCh - '0');
            }
        } else {
            /* Argument will be consumed (unless modifier found) */
            state.inArg = false;
            switch (*pCh) {
            case '%': {
                char c = '%';
                WriteBuf(&c, &state, 1);
                break;
            }
            case '.': {
                state.prec   = 0;
                state.inPrec = true;
                state.inArg  = true;
                break;
            }
            case '-':
                state.flags |= kPadOnRight;
                state.inArg  = true;
                break;
            case '+':
                /* Ignore + */
                state.inArg  = true;
                break;
            case 'c': {
                char c = (char)va_arg(args, int);
                WriteBuf(&c, &state, 1);
                break;
            }
            case 's': {
                char * pArg = va_arg(args, char *);
                s32 cnt = state.minWidth;
                char * p = pArg;
                for (; *p != '\0'; p++, cnt--) {
                    if (state.flags & kPadOnRight) WriteBuf(p, &state, 1);
                }
                for (; cnt > 0; cnt--) WriteBuf(&state.padChar, &state, 1);
                if (!(state.flags & kPadOnRight)) WriteBuf(pArg, &state, p - pArg);
                break;
            }
            case 'l':
                state.flags++;
                state.inArg = true;
                break;
            case 'd':
                state.flags |= (kIsInteger | kIsSigned);
                break;
            case 'u':
                state.flags |= kIsInteger;
                break;
            case 'x':
                state.flags |= (kIsInteger | kIsBase16);
                break;
            case 'X':
                state.flags |= (kIsInteger | kIsBase16 | kIsUpper);
                break;
            case 'e': {
                double argD = (double)va_arg(args, double);
                char tmpD[20];
                if (state.prec > 12) state.prec = 12;
                u32 cnt = DtoaE(tmpD, &state, argD);
                WriteBuf(tmpD, &state, cnt);
                break;
            }
            case 'f': {
                double argD = (double)va_arg(args, double);
                char tmpD[25];
                if (state.prec > 17) state.prec = 17;
                u32 cnt = DtoaF(tmpD, &state, argD);
                WriteBuf(tmpD, &state, cnt);
                break;
            }
            case 'g': {
                double argD = (double)va_arg(args, double);
                char tmpD[20];
                if (state.prec > 13) state.prec = 12;
                else if (state.prec != 0) state.prec--; 
                state.flags = kIsSigned;
                u32 cnt = DtoaE(tmpD, &state, argD);
                WriteBuf(tmpD, &state, cnt);
                break;
            }
            case 'p':
            case 'P':
                state.flags |= (kIsInteger | kIsBase16);
                state.flags |= (*pCh == 'P' ? kIsUpper : 0);
#if (__SIZEOF_SIZE_T__ == 8)
                state.flags |= 2;
#endif
                break;
            default:
                break;
            }
            /* Convert numeric types to text */
            if (state.flags & kIsInteger) {
                char tmp[20];
                u32 cnt;
                if ((state.flags & 0x3) <= 1) {
                    s32 arg32 = va_arg(args, s32);
                    cnt = Ltoa(tmp, &state, arg32);
                } else {
                    s64 arg64 = va_arg(args, s64);
                    cnt = LLtoa(tmp, &state, arg64);
                }
                char * p = tmp;
                if (state.flags & kPadOnRight) {
                    WriteBuf(p, &state, cnt);
                }
                s32 padCnt = state.minWidth - cnt;
                if (state.padChar == '0' && tmp[0] == '-') {
                    WriteBuf(p++, &state, 1);
                    cnt--;
                }
                for (; padCnt > 0; padCnt--) {
                    WriteBuf(&state.padChar, &state, 1);
                }
                if (!(state.flags & kPadOnRight)) {
                    WriteBuf(p, &state, cnt);
                }
            }
        }
    }
    *state.pOut = '\0';
    return (s32)size - (s32)state.rem;
}

s32
mosSNPrintf(char * pDest, mos_size size, const char * pFmt, ...) {
    va_list args;
    va_start(args, pFmt);
    s32 cnt = mosVSNPrintf(pDest, size, pFmt, args);
    va_end(args);
    return cnt;
}
