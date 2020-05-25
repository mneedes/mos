
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS tracing facility and command shell support
//

// TODO: Split shell out into another file?
// TODO: Install/Remove commands?
// TODO: Rotating logs
// TODO: Minimize stack requirements?

#include <string.h>

#include "hal.h"
#include "mos.h"
#include "most.h"

#define MOST_PRINT_BUFFER_SIZE   128

u32 MostTraceMask = 0;

static MosMutex Mutex;
static MosQueue RxQueue;
static u32 RxQueueBuf[16];

static const char LowerCaseDigits[] = "0123456789abcdef";
static const char UpperCaseDigits[] = "0123456789ABCDEF";

static char PrintBuffer[MOST_PRINT_BUFFER_SIZE];
static char RawPrintBuffer[MOST_PRINT_BUFFER_SIZE];

static u32 PadAndReverse(char * restrict out, u16 min_digits,
                          char pad_char, u32 cnt) {
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

static u32 ItoaPow2Div(char * restrict out, u32 in, u32 shift,
                       const char * restrict digits, u16 min_digits,
                       char pad_char) {
    u32 mask = (1 << shift) - 1;
    u32 cnt = 0;
    do {
        *out++ = digits[in & mask];
        in >>= shift;
        cnt++;
    } while (in != 0);
    return PadAndReverse(out, min_digits, pad_char, cnt);
}

static u32 ItoaPow2Div64(char * restrict out, u64 in, u32 shift,
                         const char * restrict digits, u16 min_digits,
                         char pad_char) {
    u32 mask = (1 << shift) - 1;
    u32 cnt = 0;
    do {
        *out++ = digits[in & mask];
        in >>= shift;
        cnt++;
    } while (in != 0);
    return PadAndReverse(out, min_digits, pad_char, cnt);
}

u32 MostItoa(char * restrict out, s32 in, u16 base, bool is_upper,
             u16 min_digits, char pad_char, bool is_signed) {
    const char * restrict digits = LowerCaseDigits;
    if (is_upper) digits = UpperCaseDigits;
    switch (base) {
    case 2:
        return ItoaPow2Div(out, in, 1, digits, min_digits, pad_char);
    case 8:
        return ItoaPow2Div(out, in, 3, digits, min_digits, pad_char);
    case 16:
        return ItoaPow2Div(out, in, 4, digits, min_digits, pad_char);
    default:
        break;
    }
    u32 adj = (u32) in;
    if (is_signed && in < 0) adj = (u32) -in;
    // Determine digits (in reverse order)
    u32 cnt = 0;
    do {
        *out++ = digits[adj % base];
        adj = adj / base;
        cnt++;
    } while (adj != 0);
    // Write sign
    if (is_signed && in < 0) {
        *out++ = '-';
        cnt++;
    }
    return PadAndReverse(out, min_digits, pad_char, cnt);
}

u32 MostItoa64(char * restrict out, s64 input, u16 base, bool is_upper,
               u16 min_digits, char pad_char, bool is_signed) {
    const char * restrict digits = LowerCaseDigits;
    if (is_upper) digits = UpperCaseDigits;
    switch (base) {
    case 2:
        return ItoaPow2Div64(out, input, 1, digits, min_digits, pad_char);
    case 8:
        return ItoaPow2Div64(out, input, 3, digits, min_digits, pad_char);
    case 16:
        return ItoaPow2Div64(out, input, 4, digits, min_digits, pad_char);
    default:
        break;
    }
    u64 adj = (u64) input;
    if (is_signed && input < 0) adj = (u64) -input;
    // Determine digits (in reverse order)
    u32 cnt = 0;
    do {
        *out++ = digits[adj % base];
        adj = adj / base;
        cnt++;
    } while (adj != 0);
    // Write sign
    if (is_signed && input < 0) {
        *out++ = '-';
        cnt++;
    }
    return PadAndReverse(out, min_digits, pad_char, cnt);
}

static void
WriteBuf(char * restrict * out, const char * restrict in,
         u32 len, s32 * buf_rem) {
    u32 cnt = (len < *buf_rem) ? len : *buf_rem;
    *buf_rem -= cnt;
    for (; cnt > 0; cnt--) {
        **out = *in++;
        (*out)++;
    }
}

static void
FormatString(char * restrict buffer, size_t sz,
             const char * restrict fmt, va_list args) {
    const char * restrict ch = fmt;
    char * restrict out = buffer, * arg;
    s32 buf_rem = (s32) sz - 1;
    s32 arg32, base, long_cnt, min_digits;
    bool do_numeric, is_signed, is_upper, in_arg = false;
    char tmp32[32], pad_char;
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
                arg = va_arg(args, char *);
                for (; *arg != '\0'; arg++) WriteBuf(&out, arg, 1, &buf_rem);
                break;
            case 'l':
                long_cnt++;
                in_arg = true;
                break;
            case 'b':
                base = 2;
                do_numeric = true;
                is_signed = false;
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
                arg32 = (u32) va_arg(args, u32 *);
                u32 cnt = MostItoa(tmp32, arg32, 16, false, 8, '0', false);
                WriteBuf(&out, tmp32, cnt, &buf_rem);
                break;
            }
            case 'P':
            {
                arg32 = (u32) va_arg(args, u32 *);
                u32 cnt = MostItoa(tmp32, arg32, 16, true, 8, '0', false);
                WriteBuf(&out, tmp32, cnt, &buf_rem);
                break;
            }
            default:
                break;
            }
            // Convert numeric types to text
            if (do_numeric) {
                if (long_cnt <= 1) {
                    arg32 = va_arg(args, s32);
                    u32 cnt = MostItoa(tmp32, arg32, base, is_upper,
                                       min_digits, pad_char, is_signed);
                    WriteBuf(&out, tmp32, cnt, &buf_rem);
                } else {
                    s64 arg64 = va_arg(args, s64);
                    char tmp64[64];
                    u32 cnt = MostItoa64(tmp64, arg64, base, is_upper,
                                         min_digits, pad_char, is_signed);
                    WriteBuf(&out, tmp64, cnt, &buf_rem);
                }
            }
        }
    }
    *out = '\0';
}

static void MostPrintCh(char ch) {
    MosTakeMutex(&Mutex);
    HalSendToTxUART(ch);
    MosGiveMutex(&Mutex);
}

static u32 _MostPrint(char * str) {
    u32 cnt = 0;
    for (char * ch = str; *ch != '\0'; ch++, cnt++) {
        if (*ch == '\n') HalSendToTxUART('\r');
        HalSendToTxUART(*ch);
    }
    return cnt;
}

u32 MostPrint(char * str) {
    MosTakeMutex(&Mutex);
    u32 cnt = _MostPrint(str);
    MosGiveMutex(&Mutex);
    return cnt;
}

u32 MostPrintf(const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MosTakeMutex(&Mutex);
    FormatString(PrintBuffer, MOST_PRINT_BUFFER_SIZE, fmt, args);
    u32 cnt = _MostPrint(PrintBuffer);
    MosGiveMutex(&Mutex);
    va_end(args);
    return cnt;
}

void MostLogTraceMessage(char * id, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MosTakeMutex(&Mutex);
    _MostPrint(id);
    FormatString(PrintBuffer, MOST_PRINT_BUFFER_SIZE, fmt, args);
    _MostPrint(PrintBuffer);
    MosGiveMutex(&Mutex);
    va_end(args);
}

void MostLogHexDumpMessage(char * id, char * name,
                           const void * addr, u32 size) {
    const u8 * restrict data = (const u8 *) addr;
    MosTakeMutex(&Mutex);
    _MostPrint(id);
    _MostPrint(name);
    _MostPrint("\n");
    // 16 bytes per line
    for (u32 lines = (size >> 4) + 1; lines > 0; lines--) {
        char * buf = PrintBuffer;
        u32 bytes = 16;
        if (lines == 1) {
            bytes = size & 15;
            if (bytes == 0) break;
        }
        // Address
        buf += MostItoa(buf, (s32) data, 16, true, 8, '0', false);
        *buf++ = ' ';
        *buf++ = ' ';
        for (; bytes > 0; bytes--) {
            buf += MostItoa(buf, *data, 16, true, 2, '0', false);
            *buf++ = ' ';
            data++;
        }
        *buf++ = '\n';
        *buf++ = '\0';
        _MostPrint(PrintBuffer);
    }
    MosGiveMutex(&Mutex);
}

static void MostRawPrintfCallback(const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    FormatString(RawPrintBuffer, MOST_PRINT_BUFFER_SIZE, fmt, args);
    va_end(args);
    _MostPrint(RawPrintBuffer);
}

void MostTakeMutex(void) {
    MosTakeMutex(&Mutex);
}

bool MostTryMutex(void) {
    return MosTryMutex(&Mutex);
}

void MostGiveMutex(void) {
    MosGiveMutex(&Mutex);
}

// Callback must be ISR safe
static void MostRxCallback(char ch) {
    MosSendToQueue(&RxQueue, (u32)ch);
}

MostCmdResult MostGetNextCmd(char * prompt, char * cmd, u32 max_cmd_len) {
    enum {
        KEY_NORMAL,
        KEY_ESCAPE,
        KEY_ESCAPE_PLUS_BRACKET
    };
    static u32 buf_ix = 0;
    static bool last_ch_was_arrow = false;
    MosTakeMutex(&Mutex);
    if (buf_ix) {
        for (u32 ix = 0; ix < buf_ix; ix++) _MostPrint("\b \b");
    } else if (prompt && !last_ch_was_arrow) {
        _MostPrint(prompt);
    }
    buf_ix = _MostPrint(cmd);
    MosGiveMutex(&Mutex);
    last_ch_was_arrow = false;
    u32 state = KEY_NORMAL;
    while (1) {
        char ch = (char)MosReceiveFromQueue(&RxQueue);
        switch (state) {
        default:
        case KEY_NORMAL:
            switch (ch) {
            default:
                if (buf_ix < max_cmd_len && ch > 31) {
                    MostPrintCh(ch);
                    cmd[buf_ix++] = ch;
                }
                break;
            case 27:
                state = KEY_ESCAPE;
                break;
            case '\b':
            case 127:
                if (buf_ix) {
                    MostPrint("\b \b");
                    buf_ix--;
                }
                break;
            case '\r':
                MostPrint("\n");
                cmd[buf_ix] = '\0';
                buf_ix = 0;
                return MOST_CMD_RECEIVED;
            }
            break;
        case KEY_ESCAPE:
            if (ch == '[') state = KEY_ESCAPE_PLUS_BRACKET;
            else state = KEY_NORMAL;
            break;
        case KEY_ESCAPE_PLUS_BRACKET:
            if (ch == 'A') {
                last_ch_was_arrow = true;
                cmd[buf_ix] = '\0';
                return MOST_CMD_UP_ARROW;
            } else if (ch == 'B') {
                last_ch_was_arrow = true;
                cmd[buf_ix] = '\0';
                return MOST_CMD_DOWN_ARROW;
            } else {
                state = KEY_NORMAL;
            }
            break;
        }
    }
}

u32 MostParseCmd(char * argv[], char * args, u32 max_argc) {
    if (args == NULL || args[0] == '\0') return 0;
    char * ch_in = args, * ch_out = args;
    bool in_arg = false, in_quote = false;
    u32 argc = 0;
    while (*ch_in != '\0') {
        switch (*ch_in) {
        case ' ':
        case '\t':
            if (!in_quote) {
                if (in_arg) {
                    in_arg = false;
                    *ch_out++ = '\0';
                }
                ch_in++;
                continue;
            }
            break;
        case '"':
            in_quote = !in_quote;
            ch_in++;
            continue;
        case '\\':
            ch_in++;
            break;
        default:
            break;
        }
        if (!in_arg) {
            if (argc < max_argc) argv[argc++] = ch_out;
            in_arg = true;
        }
        *ch_out++ = *ch_in++;
    }
    *ch_out = '\0';
    return argc;
}

MostCmd * MostFindCmd(char * name, MostCmd * commands, u32 num_cmds) {
    for (u32 ix = 0; ix < num_cmds; ix++) {
        if (strcmp(name, commands[ix].name) == 0) return &commands[ix];
    }
    return NULL;
}

void MostPrintHelp(MostCmd * commands, u32 num_cmds) {
    MosTakeMutex(&Mutex);
    for (u32 ix = 0; ix < num_cmds; ix++) {
        MostPrintf("%s %s: %s\n", commands[ix].name,
                   commands[ix].usage, commands[ix].desc);
    }
    MosGiveMutex(&Mutex);
}

void MostInit(u32 mask, bool enable_raw_printf_hook) {
    MostTraceMask = mask;
    MosInitMutex(&Mutex);
    MosInitQueue(&RxQueue, RxQueueBuf, count_of(RxQueueBuf));
    if (enable_raw_printf_hook)
        MosRegisterRawPrintfHook(MostRawPrintfCallback);
    HalRegisterRxUARTCallback(MostRxCallback);
}
