
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/format_string.h>

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

// TODO: Properly handle min_width for floats
// TODO: Limit digits from Dtoa to "25" or so?

// Use structure to limit stack depth
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
} Format;

static u32 MOS_NO_INLINE LLtoa(char * restrict out, Format * format, s64 in) {
    u64 adj = (u64)in;
    u8 shift = 0;
    switch (format->base) {
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
        if (format->is_upper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (format->is_signed && in < 0) adj = (u64) -in;
        // Determine digits (in reverse order)
        do {
            *out++ = LowerCaseDigits[adj % format->base];
            adj = adj / format->base;
            cnt++;
        } while (adj != 0);
        // Write sign
        if (format->is_signed && in < 0) {
            *out++ = '-';
            cnt++;
        }
    }
    // Pad to minimum number of digits
    for (; cnt < format->min_width; cnt++) *out++ = format->pad_char;
    // Reverse digit order in place
    for (s32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = out[idx - cnt];
        out[idx - cnt] = out[-idx - 1];
        out[-idx - 1] = tmp;
    }
    return cnt;
}

static u32 Itoa(char * restrict out, Format * format, s32 in) {
    u32 adj = (u32)in;
    u8 shift = 0;
    switch (format->base) {
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
        if (format->is_upper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (format->is_signed && in < 0) adj = (u32) -in;
        // Determine digits (in reverse order)
        do {
            *out++ = LowerCaseDigits[adj % format->base];
            adj = adj / format->base;
            cnt++;
        } while (adj != 0);
        // Write sign
        if (format->is_signed && in < 0) {
            *out++ = '-';
            cnt++;
        }
    }
    // Pad to minimum number of digits
    for (; cnt < format->min_width; cnt++) *out++ = format->pad_char;
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
    Format format = {
            .base = base, .is_upper = is_upper, .min_width = min_width,
            .pad_char = pad_char, .is_signed = is_signed
    };
    return Itoa(out, &format, input);
}

static u32 MOS_NO_INLINE Dtoa(char * restrict out, Format * format, double in) {
    u64 * _in_p = (u64 *) &in;
    u64 _in = *_in_p;
    // First evaluate special values (e.g.: NaN / Inf)
    u32 pfx = (u32) (_in >> 32);
    if ((pfx & 0x7ff7ffff) == 0x7ff00000) {
        u32 sfx = (u32) (_in & 0xffffffff);
        if (sfx == 0x0) {
            if (pfx == 0xfff00000) {
                out[0] = '-'; out[1] = 'I';
                out[2] = 'n'; out[3] = 'f';
                return 4;
            } else if (pfx == 0x7ff00000) {
                out[0] = '+'; out[1] = 'I';
                out[2] = 'n'; out[3] = 'f';
                return 4;
            }
        } else if (sfx == 0x1) {
            if (pfx == 0x7ff00000 || pfx == 0x7ff80000) {
                out[0] = 'N'; out[1] = 'a'; out[2] = 'N';
                return 3;
            }
        }
    }
    // Round
    bool negative = in < 0;
    // TODO: This is prone to precision errors, use decimal?
    double p = negative ? -0.5 : 0.5;
    for (u32 ix = 0; ix < format->prec; ix++) p *= 0.1;
    in += p;
    // Get integer part
    s32 int_part = (s32) in;
    in -= (double) int_part;
    if (negative) in = -in;
    format->base      = 10;
    format->is_upper  = false;
    format->is_signed = true;
    format->min_width = 0;
    format->pad_char  = '0';
    u32 cnt = Itoa(out, format, int_part);
    out += cnt;
    // Get fractional part
    if (format->prec) {
        *out++ = '.';
        u64 val = 1;
        for (u32 ix = 0; ix < format->prec; ix++) val *= 10;
        in *= (double)val;
        int_part = (s32)in;
        format->is_signed = false;
        format->min_width = format->prec;
        cnt += Itoa(out, format, int_part) + 1;
    }
    return cnt;
}

static void
WriteBuf(char * restrict * out, const char * restrict in, s32 len, s32 * buf_rem) {
    u32 cnt = (len < *buf_rem) ? len : *buf_rem;
    *buf_rem -= cnt;
    for (; cnt > 0; cnt--) {
        **out = *in++;
        (*out)++;
    }
}

s32
MosVSNPrintf(char * restrict buffer, mos_size sz, const char * restrict fmt, va_list args) {
    const char * restrict ch = fmt;
    char * restrict out = buffer;
    s32 buf_rem = (s32)sz - 1;
    Format format = { .in_arg = false };
    for (ch = fmt; *ch != '\0'; ch++) {
        if (!format.in_arg) {
            if (*ch == '%') {
                // Found argument, set default state
                format.min_width = 0;
                format.prec      = 6;
                format.pad_char  = ' ';
                format.in_arg    = true;
                format.in_prec   = false;
                format.long_cnt  = 0;
            } else WriteBuf(&out, ch, 1, &buf_rem);
        } else if (*ch >= '0' && *ch <= '9') {
            if (!format.in_prec) {
                if (format.min_width == 0 && *ch == '0') format.pad_char = '0';
                else format.min_width = (10 * format.min_width) + (*ch - '0');
            } else {
                format.prec = (10 * format.prec) + (*ch - '0');
            }
        } else {
            // Argument will be consumed (unless modifier found)
            format.in_arg     = false;
            format.do_numeric = false;
            switch (*ch) {
            case '%': {
                char c = '%';
                WriteBuf(&out, &c, 1, &buf_rem);
                break;
            }
            case '.': {
                format.prec    = 0;
                format.in_prec = true;
                format.in_arg  = true;
                break;
            }
            case 'c': {
                char c = (char) va_arg(args, int);
                WriteBuf(&out, &c, 1, &buf_rem);
                break;
            }
            case 's': {
                char * arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++)
                    WriteBuf(&out, arg, 1, &buf_rem);
                break;
            }
            case 'l':
                format.long_cnt++;
                format.in_arg     = true;
                break;
            case 'o':
                format.base       = 8;
                format.is_signed  = false;
                format.do_numeric = true;
                break;
            case 'd':
                format.base       = 10;
                format.is_signed  = true;
                format.do_numeric = true;
                break;
            case 'u':
                format.base       = 10;
                format.is_signed  = false;
                format.do_numeric = true;
                break;
            case 'x':
                format.base       = 16;
                format.is_upper   = false;
                format.is_signed  = false;
                format.do_numeric = true;
                break;
            case 'X':
                format.base       = 16;
                format.is_upper   = true;
                format.is_signed  = false;
                format.do_numeric = true;
                break;
            case 'f': {
                double argD = (double)va_arg(args, double);
                char tmpD[25];
                u32 cnt = Dtoa(tmpD, &format, argD);
                WriteBuf(&out, tmpD, cnt, &buf_rem);
                break;
            }
            case 'p':
            case 'P': {
                s32 arg32 = (u32)va_arg(args, u32 *);
                char tmp32[8];
                format.base      = 16;
                format.is_upper  = (*ch == 'P');
                format.is_signed = false;
                format.min_width = 8;
                format.pad_char  = '0';
                u32 cnt = Itoa(tmp32, &format, arg32);
                WriteBuf(&out, tmp32, cnt, &buf_rem);
                break;
            }
            default:
                break;
            }
            // Convert numeric types to text
            if (format.do_numeric) {
                if (format.long_cnt <= 1) {
                    s32 arg32 = va_arg(args, s32);
                    char tmp32[11];
                    u32 cnt = Itoa(tmp32, &format, arg32);
                    WriteBuf(&out, tmp32, cnt, &buf_rem);
                } else {
                    s64 arg64 = va_arg(args, s64);
                    char tmp64[22];
                    u32 cnt = LLtoa(tmp64, &format, arg64);
                    WriteBuf(&out, tmp64, cnt, &buf_rem);
                }
            }
        }
    }
    *out = '\0';
    return sz - buf_rem - 1;
}

s32
MosSNPrintf(char * restrict dest, mos_size size, const char * restrict fmt, ...) {
    va_list args;
    va_start(args, fmt);
    s32 cnt = MosVSNPrintf(dest, size, fmt, args);
    va_end(args);
    return cnt;
}
