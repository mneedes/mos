
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS tracing facility and command shell support
//

// TODO: Rotating logs

#include <string.h>

#include <mos/hal.h>
#include <mos/trace.h>
#include <mos/internal/trace.h>

#define MOS_PRINT_BUFFER_SIZE   128

u32 MosTraceMask = 0;

static MosMutex PrintMutex;

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

static char PrintBuffer[MOS_PRINT_BUFFER_SIZE];
static char RawPrintBuffer[MOS_PRINT_BUFFER_SIZE];

static void
WriteBuf(char * restrict * out, const char * restrict in,
         s16 len, s16 * buf_rem) {
    u32 cnt = (len < *buf_rem) ? len : *buf_rem;
    *buf_rem -= cnt;
    for (; cnt > 0; cnt--) {
        **out = *in++;
        (*out)++;
    }
}

static void
FormatString(char * restrict buffer, s16 sz,
             const char * restrict fmt, va_list args) {
    const char * restrict ch = fmt;
    char * restrict out = buffer;
    s16 buf_rem = (s16) sz - 1;
    bool do_numeric, is_signed, is_upper, in_arg = false;
    u8 base, long_cnt, min_digits;
    char pad_char;
    for (ch = fmt; *ch != '\0'; ch++) {
        if (!in_arg) {
            if (*ch == '%') {
                // Found argument, set default state
                in_arg = true;
                long_cnt = 0;
                pad_char = ' ';
                min_digits = 0;
            } else WriteBuf(&out, ch, 1, &buf_rem);
        } else if (*ch >= '0' && *ch <= '9') {
            if (min_digits == 0 && *ch == '0') pad_char = '0';
            else min_digits = (10 * min_digits) + (*ch - '0');
        } else {
            // Argument will be consumed (unless modifier found)
            in_arg = false;
            do_numeric = false;
            switch (*ch) {
            case '%':
            {
                char c = '%';
                WriteBuf(&out, &c, 1, &buf_rem);
                break;
            }
            case 'c':
            {
                char c = (char) va_arg(args, int);
                WriteBuf(&out, &c, 1, &buf_rem);
                break;
            }
            case 's':
            {
                char * arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++)
                    WriteBuf(&out, arg, 1, &buf_rem);
                break;
            }
            case 'l':
                long_cnt++;
                in_arg = true;
                break;
            case 'o':
                base = 8;
                do_numeric = true;
                is_signed = false;
                break;
            case 'd':
                base = 10;
                do_numeric = true;
                is_signed = true;
                break;
            case 'u':
                base = 10;
                do_numeric = true;
                is_signed = false;
                break;
            case 'x':
                base = 16;
                do_numeric = true;
                is_signed = false;
                is_upper = false;
                break;
            case 'X':
                base = 16;
                do_numeric = true;
                is_signed = false;
                is_upper = true;
                break;
            case 'p':
            {
                s32 arg32 = (u32) va_arg(args, u32 *);
                char tmp32[8];
                u32 cnt = MosItoa(tmp32, arg32, 16, false, 8, '0', false);
                WriteBuf(&out, tmp32, cnt, &buf_rem);
                break;
            }
            case 'P':
            {
                s32 arg32 = (u32) va_arg(args, u32 *);
                char tmp32[8];
                u32 cnt = MosItoa(tmp32, arg32, 16, true, 8, '0', false);
                WriteBuf(&out, tmp32, cnt, &buf_rem);
                break;
            }
            default:
                break;
            }
            // Convert numeric types to text
            if (do_numeric) {
                if (long_cnt <= 1) {
                    s32 arg32 = va_arg(args, s32);
                    char tmp32[11];
                    u32 cnt = MosItoa(tmp32, arg32, base, is_upper,
                                       min_digits, pad_char, is_signed);
                    WriteBuf(&out, tmp32, cnt, &buf_rem);
                } else {
                    s64 arg64 = va_arg(args, s64);
                    char tmp64[22];
                    u32 cnt = MosItoa64(tmp64, arg64, base, is_upper,
                                         min_digits, pad_char, is_signed);
                    WriteBuf(&out, tmp64, cnt, &buf_rem);
                }
            }
        }
    }
    *out = '\0';
}

void _MosPrintCh(char ch) {
    MosTakeMutex(&PrintMutex);
    HalSendToTxUART(ch);
    MosGiveMutex(&PrintMutex);
}

u32 _MosPrint(char * str) {
    u32 cnt = 0;
    for (char * ch = str; *ch != '\0'; ch++, cnt++) {
        if (*ch == '\n') HalSendToTxUART('\r');
        HalSendToTxUART(*ch);
    }
    return cnt;
}

static void MosRawPrintfCallback(const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    FormatString(RawPrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    va_end(args);
    _MosPrint(RawPrintBuffer);
}

void MosInitTrace(u32 mask, bool enable_raw_printf_hook) {
    MosTraceMask = mask;
    MosInitMutex(&PrintMutex);
    if (enable_raw_printf_hook)
        MosRegisterRawPrintfHook(MosRawPrintfCallback);
}

u32 MosItoa(char * restrict out, s32 in, u16 base, bool is_upper,
            u16 min_digits, char pad_char, bool is_signed) {
    u32 adj = (u32) in;
    u8 shift = 0;
    switch (base) {
    case 2:
        shift = 1;
        break;
    case 8:
        shift = 3;
        break;
    case 16:
        shift = 4;
        break;
    }
    u32 cnt = 0;
    if (shift) {
        const char * restrict digits = LowerCaseDigits;
        if (is_upper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (is_signed && in < 0) adj = (u32) -in;
        // Determine digits (in reverse order)
        do {
            *out++ = LowerCaseDigits[adj % base];
            adj = adj / base;
            cnt++;
        } while (adj != 0);
        // Write sign
        if (is_signed && in < 0) {
            *out++ = '-';
            cnt++;
        }
    }
    // Pad to minimum number of digits
    for (; cnt < min_digits; cnt++) *out++ = pad_char;
    // Reverse digit order in place
    for (u32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = out[idx - cnt];
        out[idx - cnt] = out[-idx - 1];
        out[-idx - 1] = tmp;
    }
    return cnt;
}

u32 MosItoa64(char * restrict out, s64 in, u16 base, bool is_upper,
              u16 min_digits, char pad_char, bool is_signed) {
    u64 adj = (u64) in;
    u32 cnt = 0;
    u8 shift = 0;
    switch (base) {
    case 2:
        shift = 1;
        break;
    case 8:
        shift = 3;
        break;
    case 16:
        shift = 4;
        break;
    }
    if (shift) {
        const char * restrict digits = LowerCaseDigits;
        if (is_upper) digits = UpperCaseDigits;
        u32 mask = (1 << shift) - 1;
        do {
            *out++ = digits[adj & mask];
            adj >>= shift;
            cnt++;
        } while (adj != 0);
    } else {
        if (is_signed && in < 0) adj = (u64) -in;
        // Determine digits (in reverse order)
        do {
            *out++ = LowerCaseDigits[adj % base];
            adj = adj / base;
            cnt++;
        } while (adj != 0);
        // Write sign
        if (is_signed && in < 0) {
            *out++ = '-';
            cnt++;
        }
    }
    // Pad to minimum number of digits
    for (; cnt < min_digits; cnt++) *out++ = pad_char;
    // Reverse digit order in place
    for (u32 idx = 0; idx < ((cnt + 1) >> 1); idx++) {
        char tmp = out[idx - cnt];
        out[idx - cnt] = out[-idx - 1];
        out[-idx - 1] = tmp;
    }
    return cnt;
}

u32 MosPrint(char * str) {
    MosTakeMutex(&PrintMutex);
    u32 cnt = _MosPrint(str);
    MosGiveMutex(&PrintMutex);
    return cnt;
}

u32 MosPrintf(const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MosTakeMutex(&PrintMutex);
    FormatString(PrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    u32 cnt = _MosPrint(PrintBuffer);
    MosGiveMutex(&PrintMutex);
    va_end(args);
    return cnt;
}

void MosLogTraceMessage(char * id, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MosTakeMutex(&PrintMutex);
    _MosPrint(id);
    FormatString(PrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    _MosPrint(PrintBuffer);
    MosGiveMutex(&PrintMutex);
    va_end(args);
}

void MosLogHexDumpMessage(char * id, char * name,
                          const void * addr, u32 size) {
    const u8 * restrict data = (const u8 *) addr;
    MosTakeMutex(&PrintMutex);
    _MosPrint(id);
    _MosPrint(name);
    _MosPrint("\n");
    // 16 bytes per line
    for (u32 lines = (size >> 4) + 1; lines > 0; lines--) {
        char * buf = PrintBuffer;
        u32 bytes = 16;
        if (lines == 1) {
            bytes = size & 15;
            if (bytes == 0) break;
        }
        // Address
        buf += MosItoa(buf, (s32) data, 16, true, 8, '0', false);
        *buf++ = ' ';
        *buf++ = ' ';
        for (; bytes > 0; bytes--) {
            buf += MosItoa(buf, *data, 16, true, 2, '0', false);
            *buf++ = ' ';
            data++;
        }
        *buf++ = '\n';
        *buf++ = '\0';
        _MosPrint(PrintBuffer);
    }
    MosGiveMutex(&PrintMutex);
}

void MosTakeTraceMutex(void) {
    MosTakeMutex(&PrintMutex);
}

bool MosTryTraceMutex(void) {
    return MosTryMutex(&PrintMutex);
}

void MosGiveTraceMutex(void) {
    MosGiveMutex(&PrintMutex);
}
