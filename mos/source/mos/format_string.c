
// Copyright 2021-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/format_string.h>

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

// %f -> precision is number of digits past decimal point, displayed even if they are zero
//         if precision IS zero, then decimal point is NOT printed
// %g -> precision is max number of significant digits, either side of decimal
//         zeros and decimal point are trimmed.
//         This implementation does not handle scientific notation.

// This structure limits the stack depth
typedef struct {
    // Format settings
    u8   base;
    u8   is_upper;
    u8   is_signed;
    u8   min_width;
    u8   prec;
    char pad_char;
    // State variables
    u8   do_numeric;
    u8   in_prec;
    u8   in_arg;
    u8   long_cnt;
} State;

// No-inline prevents inadvertent entry into lazy-stacking floating point modes
static u32 MOS_NO_INLINE LLtoa(char * restrict out, State * state, s64 in) {
    u64 adj = (u64)in;
    u8 shift = 0;
    switch (state->base) {
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
        if (state->is_upper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (state->is_signed && in < 0) adj = (u64)-in;
        // Determine digits (in reverse order)
        do {
            *out++ = LowerCaseDigits[adj % state->base];
            adj = adj / state->base;
            cnt++;
        } while (adj != 0);
        // Write sign
        if (state->is_signed && in < 0) {
            *out++ = '-';
            cnt++;
        }
    }
    // Pad to minimum number of digits
    for (; cnt < state->min_width; cnt++) *out++ = state->pad_char;
    // Reverse digit order in place
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = out[idx - cnt];
        out[idx - cnt] = out[-idx - 1];
        out[-idx - 1] = tmp;
    }
    return cnt;
}

static u32 Itoa(char * restrict out, State * state, s32 in) {
    u32 adj = (u32)in;
    u8 shift = 0;
    switch (state->base) {
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
        if (state->is_upper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (state->is_signed && in < 0) adj = (u32)-in;
        // Determine digits (in reverse order)
        do {
            *out++ = LowerCaseDigits[adj % state->base];
            adj = adj / state->base;
            cnt++;
        } while (adj != 0);
        // Write sign
        if (state->is_signed && in < 0) {
            *out++ = '-';
            cnt++;
        }
    }
    // Pad to minimum number of digits
    for (; cnt < state->min_width; cnt++) *out++ = state->pad_char;
    // Reverse digit order in place
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = out[idx - cnt];
        out[idx - cnt] = out[-idx - 1];
        out[-idx - 1] = tmp;
    }
    return cnt;
}

u32 MosItoa(char * restrict out, s32 input, u16 base, bool is_upper,
            u16 min_width, char pad_char, bool is_signed) {
    State format = {
        .base = base, .is_upper = is_upper, .min_width = min_width,
        .pad_char = pad_char, .is_signed = is_signed
    };
    return Itoa(out, &format, input);
}

// No-inline prevents inadvertent entry into lazy-stacking floating point modes
static u32 MOS_NO_INLINE Dtoa(char * restrict out, State * state, double in) {
    u64 * _in_p = (u64 *)&in;
    u64 _in = *_in_p;
    // First evaluate special values (e.g.: NaN / Inf)
    u32 pfx = (u32)(_in >> 32);
    if ((pfx & 0x7ff00000) == 0x7ff00000) {
        u32 sfx = (u32)(_in & 0xffffffff);
        if (sfx == 0x0) {
            if (pfx == 0xfff00000) {
                out[0] = '-'; out[1] = 'i';
                out[2] = 'n'; out[3] = 'f';
                return 4;
            } else if (pfx == 0x7ff00000) {
                out[0] = 'i'; out[1] = 'n'; out[2] = 'f';
                return 3;
            } else {
                out[0] = 'n'; out[1] = 'a'; out[2] = 'n';
                return 3;
            }
        } else {
            out[0] = 'n'; out[1] = 'a'; out[2] = 'n';
            return 3;
        }
    }
    // Round
    bool negative = in < 0;
    double p = negative ? -0.5 : 0.5;
    for (u32 ix = 0; ix < state->prec; ix++) p *= 0.1;
    in += p;
    // Get integer part
    s64 int_part = (s64)in;
    in -= (double)int_part;
    if (negative) in = -in;
    state->base      = 10;
    state->is_upper  = false;
    state->is_signed = true;
    state->min_width = 0;
    state->pad_char  = '0';
    u32 cnt = LLtoa(out, state, int_part);
    out += cnt;
    // Get fractional part
    if (state->prec) {
        *out++ = '.';
        u64 val = 1;
        for (u32 ix = 0; ix < state->prec; ix++) val *= 10;
        in *= (double)val;
        int_part = (s64)in;
        state->is_signed = false;
        state->min_width = state->prec;
        cnt += LLtoa(out, state, int_part) + 1;
    }
    return cnt;
}

static void
WriteBuf(char * restrict * out, const char * restrict in, s32 len, s32 * rem) {
    u32 cnt = len;
    if (len > *rem) {
        if (*rem < 0) cnt = 0;
        else cnt = *rem;
    }
    *rem -= len;
    for (; cnt > 0; cnt--) {
        **out = *in++;
        (*out)++;
    }
}

s32
MosVSNPrintf(char * restrict buffer, mos_size sz, const char * restrict fmt, va_list args) {
    const char * restrict ch = fmt;
    char * restrict out = buffer;
    s32 rem = (s32)--sz;
    State state = { .in_arg = false };
    for (; *ch != '\0'; ch++) {
        if (!state.in_arg) {
            if (*ch == '%') {
                // Found argument, set default state
                state.min_width = 0;
                state.prec      = 6;
                state.pad_char  = ' ';
                state.in_arg    = true;
                state.in_prec   = false;
                state.long_cnt  = 0;
            } else WriteBuf(&out, ch, 1, &rem);
        } else if (*ch >= '0' && *ch <= '9') {
            if (!state.in_prec) {
                if (state.min_width == 0 && *ch == '0') state.pad_char = '0';
                else state.min_width = (10 * state.min_width) + (*ch - '0');
            } else {
                state.prec = (10 * state.prec) + (*ch - '0');
            }
        } else {
            // Argument will be consumed (unless modifier found)
            state.in_arg     = false;
            state.do_numeric = false;
            switch (*ch) {
            case '%': {
                char c = '%';
                WriteBuf(&out, &c, 1, &rem);
                break;
            }
            case '.': {
                state.prec    = 0;
                state.in_prec = true;
                state.in_arg  = true;
                break;
            }
            case 'c': {
                char c = (char) va_arg(args, int);
                WriteBuf(&out, &c, 1, &rem);
                break;
            }
            case 's': {
                char * arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++)
                    WriteBuf(&out, arg, 1, &rem);
                break;
            }
            case 'l':
                state.long_cnt++;
                state.in_arg     = true;
                break;
            case 'o':
                state.base       = 8;
                state.is_signed  = false;
                state.do_numeric = true;
                break;
            case 'd':
                state.base       = 10;
                state.is_signed  = true;
                state.do_numeric = true;
                break;
            case 'u':
                state.base       = 10;
                state.is_signed  = false;
                state.do_numeric = true;
                break;
            case 'x':
                state.base       = 16;
                state.is_upper   = false;
                state.is_signed  = false;
                state.do_numeric = true;
                break;
            case 'X':
                state.base       = 16;
                state.is_upper   = true;
                state.is_signed  = false;
                state.do_numeric = true;
                break;
            case 'f': {
                double argD = (double)va_arg(args, double);
                char tmpD[25];
                u32 cnt = Dtoa(tmpD, &state, argD);
                WriteBuf(&out, tmpD, cnt, &rem);
                break;
            }
            case 'p':
            case 'P': {
                s32 arg32 = (u32)va_arg(args, u32 *);
                char tmp32[8];
                state.base      = 16;
                state.is_upper  = (*ch == 'P');
                state.is_signed = false;
                state.min_width = 8;
                state.pad_char  = '0';
                u32 cnt = Itoa(tmp32, &state, arg32);
                WriteBuf(&out, tmp32, cnt, &rem);
                break;
            }
            default:
                break;
            }
            // Convert numeric types to text
            if (state.do_numeric) {
                if (state.long_cnt <= 1) {
                    s32 arg32 = va_arg(args, s32);
                    char tmp32[11];
                    u32 cnt = Itoa(tmp32, &state, arg32);
                    WriteBuf(&out, tmp32, cnt, &rem);
                } else {
                    s64 arg64 = va_arg(args, s64);
                    char tmp64[22];
                    u32 cnt = LLtoa(tmp64, &state, arg64);
                    WriteBuf(&out, tmp64, cnt, &rem);
                }
            }
        }
    }
    *out = '\0';
    return (s32)sz - rem;
}

s32
MosSNPrintf(char * restrict dest, mos_size size, const char * restrict fmt, ...) {
    va_list args;
    va_start(args, fmt);
    s32 cnt = MosVSNPrintf(dest, size, fmt, args);
    va_end(args);
    return cnt;
}
