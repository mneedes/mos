
//  Copyright 2019 Matthew Christopher Needes
//  Licensed under the terms and conditions contained within the LICENSE
//  file (the "License") included under this distribution.  You may not use
//  this file except in compliance with the License.

//
// MOS Tracing Facility
//

// TODO: Limit to buffer size.

#if (MOST_USE_STDIO == true)
#include <stdio.h>
#endif

#include "hal.h"
#include "mos.h"
#include "most.h"

u32 MostTraceMask = 0;
static MosMutex MostMutex;

static const char lcDigits[] = "0123456789abcdef";
static const char ucDigits[] = "0123456789ABCDEF";

void MostItoa32(char * restrict * out,
                s32 input, u16 base,
                bool is_signed, bool is_upper,
                const u16 min_digits, char pad_char) {
    // Upper-case or lower-case
    const char * digits_p = lcDigits;
    if (is_upper) digits_p = ucDigits;
    // Negate input based on sign
    u32 adj = (u32) input;
    if (is_signed && input < 0) adj = (u32) -input;
    // Determine digits (in reverse order)
    u32 cnt = 0;
    do {
        cnt++;
        **out = digits_p[adj % base];
        (*out)++;
        adj = adj / base;
    } while (adj != 0);
    // Write sign
    if (is_signed && input < 0) {
        cnt++;
        **out = '-';
        (*out)++;
    }
    // Pad to minimum number of digits
    for (; cnt < min_digits; cnt++) {
        **out = pad_char;
        (*out)++;
    }
    // Reverse digit order in place
    for (u32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = (*out)[ idx - cnt];
        (*out)[ idx - cnt] = (*out)[-idx - 1];
        (*out)[-idx - 1] = tmp;
    }
}

void MostItoa64(char * restrict * out,
                s64 input, u16 base,
                bool is_signed, bool is_upper,
                const u16 min_digits, char pad_char) {
    // Upper-case or lower-case
    const char * digits_p = lcDigits;
    if (is_upper) digits_p = ucDigits;
    // Negate input based on sign
    u64 adj = (u64) input;
    if (is_signed && input < 0) adj = (u64) -input;
    // Determine digits (in reverse order)
    u32 cnt = 0;
    do {
        cnt++;
        **out = digits_p[adj % base];
        (*out)++;
        adj = adj / base;
    } while (adj != 0);
    // Write sign
    if (is_signed && input < 0) {
        cnt++;
        **out = '-';
        (*out)++;
    }
    // Pad to minimum number of digits
    for (; cnt < min_digits; cnt++) {
        **out = pad_char;
        (*out)++;
    }
    // Reverse digit order in place
    for (u32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = (*out)[ idx - cnt];
        (*out)[ idx - cnt] = (*out)[-idx - 1];
        (*out)[-idx - 1] = tmp;
    }
}

static void MostTraceFmt(char * buffer, const char * fmt, va_list args) {
    const char * ch = fmt;
    char * buf = buffer;
    char pad_char = ' ';
    char *arg;
    s64 arg64;
    s32 arg32, base, long_cnt = 0, min_digits = 0;
    bool is_numeric, is_signed, is_upper;
    bool in_arg = false;
    for (ch = fmt; *ch != '\0'; ch++) {
        if (!in_arg) {
            if (*ch == '%') {
                // Found argument, set default state
                in_arg = true;
                long_cnt = 0;
                pad_char = ' ';
                min_digits = 0;
            } else *buf++ = *ch;
        } else if (*ch >= '0' && *ch <= '9') {
            if (min_digits == 0 && *ch == '0') {
                // Found zero pad prefix
                pad_char = '0';
            } else {
                // Accumulate number of leading digits
                min_digits = (10 * min_digits) + (*ch - '0');
            }
        } else {
            // Argument will be consumed (unless modifier found)
            in_arg = false;
            is_numeric = true;
            switch (*ch) {
            case '%':
                *buf++ = '%';
                is_numeric = false;
                break;
            case 'c':
                // Char is promoted to int when passed through va args
                arg32 = va_arg(args, int);
                *buf++ = (char) arg32;
                is_numeric = false;
                break;
            case 's':
                arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++)
                    *buf++ = *arg;
                is_numeric = false;
                break;
            case 'l':
                long_cnt++;
                in_arg = true;
                is_numeric = false;
                break;
            case 'o':
                base = 8;
                is_signed = false;
                is_upper = false;
                break;
            case 'd':
                base = 10;
                is_signed = true;
                is_upper = false;
                break;
            case 'u':
                base = 10;
                is_signed = false;
                is_upper = false;
                break;
            case 'x':
                base = 16;
                is_signed = false;
                is_upper = false;
                break;
            case 'X':
                base = 16;
                is_signed = false;
                is_upper = false;
                break;
            case 'p':
                arg32 = (u32) va_arg(args, u32 *);
                MostItoa32(&buf, arg32, 16, false, false, 8, '0');
                is_numeric = false;
                break;
            case 'P':
                arg32 = (u32) va_arg(args, u32 *);
                MostItoa32(&buf, arg32, 16, false, true, 8, '0');
                is_numeric = false;
                break;
            default:
                is_numeric = false;
                break;
            }
            // Convert numeric types to text
            if (is_numeric) {
                if (long_cnt <= 1) {
                    arg32 = va_arg(args, s32);
                    MostItoa32(&buf, arg32, base, is_signed,
                               is_upper, min_digits, pad_char);
                } else {
                    arg64 = va_arg(args, s64);
                    MostItoa64(&buf, arg64, base, is_signed,
                               is_upper, min_digits, pad_char);
                }
            }
        }
    }
    *buf = '\0';
}

void MostPrint(char * str) {
    MosTakeMutex(&MostMutex);
#if (MOST_USE_STDIO == true)
    fputs(str, stdout);
#else
    HalPrintToConsole(str);
#endif
    MosGiveMutex(&MostMutex);
}

void MostPrintf(char * buffer, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MostTraceFmt(buffer, fmt, args);
    va_end(args);
    MostPrint(buffer);
}

void MostTraceMessage(char * buffer, const char * id,
                      const char * fmt, ...) {
    char * restrict buf = buffer;
    const char * restrict str = id;
    va_list args;
    while (*str != '\0') *buf++ = *str++;
    va_start(args, fmt);
    MostTraceFmt(buf, fmt, args);
    va_end(args);
    MosTakeMutex(&MostMutex);
    MostPrint(buffer);
}

void MostHexDumpMessage(char * buffer, const char * id, const char * name,
                        const void * addr, u32 size) {
    char * restrict buf = buffer;
    const char * restrict str = id;
    const u8 * restrict data = (const u8 *) addr;
    u32 lines, bytes;
    while (*str != '\0')
        *buf++ = *str++;
    str = name;
    while (*str != '\0')
        *buf++ = *str++;
    *buf++ = '\n';
    // 16 bytes per line
    for (lines = (size >> 4) + 1; lines > 0; lines--) {
        if (lines > 1) {
            bytes = 16;
        } else {
            bytes = size & 15;
            if (bytes == 0) break;
        }
        // Address
        MostItoa32(&buf, (s32) data, 16, false, true, 8, '0');
        *buf++ = ' ';
        *buf++ = ' ';
        for (; bytes > 0; bytes--) {
            MostItoa32(&buf, *data, 16, false, true, 2, '0');
            *buf++ = ' ';
            data++;
        }
        *buf++ = '\n';
    }
    *buf = '\0';
    MostPrint(buffer);
}

void MostTakeMutex(void) {
    MosTakeMutex(&MostMutex);
}

bool MostTryMutex(void) {
    return MosTryMutex(&MostMutex);
}

void MostGiveMutex(void) {
    MosGiveMutex(&MostMutex);
}

void MostInit(u32 mask) {
    MostTraceMask = mask;
    MosInitMutex(&MostMutex);
}
