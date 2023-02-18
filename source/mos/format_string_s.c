
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/kernel.h>
#include <mos/internal/arch.h>
#include <mos/format_string_s.h>

#if (MOS_ARM_RTOS_ON_SECURE_SIDE == true)

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

// This structure limits the stack depth
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
} State;

static u32 S_Itoa(char * restrict out, State * state, s32 in) {
    u32 adj = (u32)in;
    s32 cnt = 0;
    if (state->base == 16) {
        const char * restrict digits = LowerCaseDigits;
        if (state->is_upper) digits = UpperCaseDigits;
        const u32 mask = (1 << 4) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= 4;
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

static void
S_WriteBuf(char * restrict * out, const char * restrict in, s32 len, s32 * rem) {
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
S_mosVSNPrintf(char * restrict buffer, mos_size sz, const char * restrict fmt, va_list args) {
    const char * restrict ch = fmt;
    char * restrict out = buffer;
    s32 rem = (s32)sz;
    State state = { .in_arg = false };
    for (; *ch != '\0'; ch++) {
        if (!state.in_arg) {
            if (*ch == '%') {
                // Found argument, set default state
                state.min_width = 0;
                state.pad_char  = ' ';
                state.in_arg    = true;
            } else S_WriteBuf(&out, ch, 1, &rem);
        } else if (*ch >= '0' && *ch <= '9') {
            if (state.min_width == 0 && *ch == '0') state.pad_char = '0';
            else state.min_width = (10 * state.min_width) + (*ch - '0');
        } else {
            // Argument will be consumed (unless modifier found)
            state.in_arg     = false;
            state.do_numeric = false;
            switch (*ch) {
            case '%': {
                char c = '%';
                S_WriteBuf(&out, &c, 1, &rem);
                break;
            }
            case 'c': {
                char c = (char) va_arg(args, int);
                S_WriteBuf(&out, &c, 1, &rem);
                break;
            }
            case 's': {
                char * arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++)
                    S_WriteBuf(&out, arg, 1, &rem);
                break;
            }
            case 'l':
                state.in_arg     = true;
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
            case 'p': {
                s32 arg32 = (u32)va_arg(args, u32 *);
                char tmp32[8];
                state.base       = 16;
                state.is_upper   = false;
                state.is_signed  = false;
                state.min_width  = 8;
                state.pad_char   = '0';
                u32 cnt = S_Itoa(tmp32, &state, arg32);
                S_WriteBuf(&out, tmp32, cnt, &rem);
                break;
            }
            default:
                break;
            }
            // Convert numeric types to text
            if (state.do_numeric) {
                s32 arg32 = va_arg(args, s32);
                char tmp32[11];
                u32 cnt = S_Itoa(tmp32, &state, arg32);
                S_WriteBuf(&out, tmp32, cnt, &rem);
            }
        }
    }
    if (rem > 0) *out = '\0';
    return (s32)sz - rem;
}

s32
S_mosSNPrintf(char * restrict dest, mos_size size, const char * restrict fmt, ...) {
    va_list args;
    va_start(args, fmt);
    s32 cnt = S_mosVSNPrintf(dest, size, fmt, args);
    va_end(args);
    return cnt;
}

#endif
