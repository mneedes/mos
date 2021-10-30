
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/kernel.h>
#include <mos/internal/arch.h>
#include <mos_s/format_string_s.h>

#if (MOS_ARM_RTOS_ON_SECURE_SIDE == true)

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

// Use structure to limit stack depth
typedef struct {
    // Format settings
    u8   base;
    u8   is_upper;
    u8   is_signed;
    u8   min_width;
    char pad_char;
    // State variables
    u8   do_numeric;
    u8   in_arg;
} Format;

static u32 S_Itoa(char * restrict out, Format * format, s32 in) {
    u32 adj = (u32)in;
    s32 cnt = 0;
    if (format->base == 16) {
        const char * restrict digits = LowerCaseDigits;
        if (format->is_upper) digits = UpperCaseDigits;
        const u32 mask = (1 << 4) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= 4;
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

static void
S_WriteBuf(char * restrict * out, const char * restrict in, s32 len, s32 * buf_rem) {
    u32 cnt = (len < *buf_rem) ? len : *buf_rem;
    *buf_rem -= cnt;
    for (; cnt > 0; cnt--) {
        **out = *in++;
        (*out)++;
    }
}

s32
S_MosVSNPrintf(char * restrict buffer, mos_size sz, const char * restrict fmt, va_list args) {
    const char * restrict ch = fmt;
    char * restrict out = buffer;
    s32 buf_rem = (s32)sz - 1;
    Format format = { .in_arg = false };
    for (ch = fmt; *ch != '\0'; ch++) {
        if (!format.in_arg) {
            if (*ch == '%') {
                // Found argument, set default state
                format.min_width = 0;
                format.pad_char  = ' ';
                format.in_arg    = true;
            } else S_WriteBuf(&out, ch, 1, &buf_rem);
        } else if (*ch >= '0' && *ch <= '9') {
            if (format.min_width == 0 && *ch == '0') format.pad_char = '0';
            else format.min_width = (10 * format.min_width) + (*ch - '0');
        } else {
            // Argument will be consumed (unless modifier found)
            format.in_arg     = false;
            format.do_numeric = false;
            switch (*ch) {
            case '%': {
                char c = '%';
                S_WriteBuf(&out, &c, 1, &buf_rem);
                break;
            }
            case 'c': {
                char c = (char) va_arg(args, int);
                S_WriteBuf(&out, &c, 1, &buf_rem);
                break;
            }
            case 's': {
                char * arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++)
                    S_WriteBuf(&out, arg, 1, &buf_rem);
                break;
            }
            case 'l':
                format.in_arg     = true;
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
            case 'p': {
                s32 arg32 = (u32)va_arg(args, u32 *);
                char tmp32[8];
                format.base       = 16;
                format.is_upper   = false;
                format.is_signed  = false;
                format.min_width  = 8;
                format.pad_char   = '0';
                u32 cnt = S_Itoa(tmp32, &format, arg32);
                S_WriteBuf(&out, tmp32, cnt, &buf_rem);
                break;
            }
            default:
                break;
            }
            // Convert numeric types to text
            if (format.do_numeric) {
                s32 arg32 = va_arg(args, s32);
                char tmp32[11];
                u32 cnt = S_Itoa(tmp32, &format, arg32);
                S_WriteBuf(&out, tmp32, cnt, &buf_rem);
            }
        }
    }
    *out = '\0';
    return sz - buf_rem - 1;
}

s32
S_MosSNPrintf(char * restrict dest, mos_size size, const char * restrict fmt, ...) {
    va_list args;
    va_start(args, fmt);
    s32 cnt = S_MosVSNPrintf(dest, size, fmt, args);
    va_end(args);
    return cnt;
}

#endif
