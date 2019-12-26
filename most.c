
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS tracing facility and command shell support
//

// TODO: Pass in size to prevent exceeding max length, ala snprintf()
// TODO: Parse Quotes and escape characters in command shell

#if (MOST_USE_STDIO == true)
#include <stdio.h>
#endif

#include <string.h>

#include "hal.h"
#include "mos.h"
#include "most.h"

u32 MostTraceMask = 0;
static MosMutex MostMutex;

static MosQueue RxQueue;
static u32 QueueBuf[32];

static const char LCDigits[] = "0123456789abcdef";
static const char UCDigits[] = "0123456789ABCDEF";

void MostItoa32(char * restrict * out, s32 input, u16 base,
                bool is_signed, bool is_upper,
                const u16 min_digits, char pad_char) {
    const char * digits_p = LCDigits;
    if (is_upper) digits_p = UCDigits;
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

void MostItoa64(char * restrict * out, s64 input, u16 base,
                bool is_signed, bool is_upper,
                const u16 min_digits, char pad_char) {
    const char * digits_p = LCDigits;
    if (is_upper) digits_p = UCDigits;
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

u32 MostPrint(char * str) {
    u32 cnt;
    MosTakeMutex(&MostMutex);
#if (MOST_USE_STDIO == true)
    fputs(str, stdout);
    cnt = strlen(str);
#else
    cnt = 0;
    for (char * ch = str; *ch != '\0'; ch++, cnt++) {
        if (*ch == '\n') HalSendToTxUART('\r');
        HalSendToTxUART(*ch);
    }
#endif
    MosGiveMutex(&MostMutex);
    return cnt;
}

static void MostPrintCh(char ch) {
    MosTakeMutex(&MostMutex);
    HalSendToTxUART(ch);
    MosGiveMutex(&MostMutex);
}

u32 MostPrintf(char * buffer, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MostTraceFmt(buffer, fmt, args);
    va_end(args);
    return MostPrint(buffer);
}

void MostTraceMessage(char * buffer, const char * id, const char * fmt, ...) {
    char * restrict buf = buffer;
    const char * restrict str = id;
    va_list args;
    while (*str != '\0') *buf++ = *str++;
    va_start(args, fmt);
    MostTraceFmt(buf, fmt, args);
    va_end(args);
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

/* Callback must be ISR safe */
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
    MosTakeMutex(&MostMutex);
    if (buf_ix) {
        for (u32 ix = 0; ix < buf_ix; ix++) MostPrintCh(127);
    } else if (prompt && !last_ch_was_arrow) {
        MostPrint(prompt);
    }
    buf_ix = MostPrint(cmd);
    MosGiveMutex(&MostMutex);
    last_ch_was_arrow = false;
    u32 state = KEY_NORMAL;
    while (1) {
        /* Obtain next key character and parse it */
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
                    MostPrintCh(ch);
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
    if (args == NULL) return 0;
    char *tmp = NULL;
    for (u32 argc = 0; argc < max_argc; ++argc) {
        argv[argc] = strtok_r((argc == 0) ? args : NULL, " ", &tmp);
        if (argv[argc] == NULL) return argc;
    }
    return max_argc;
}

MostCmd * MostFindCmd(char * name, MostCmd * commands, u32 num_cmds) {
    for (u32 ix = 0; ix < num_cmds; ix++) {
        if (strcmp(name, commands[ix].name) == 0)
            return &commands[ix];
    }
    return NULL;
}

void MostPrintHelp(char * buffer, MostCmd * commands, u32 num_cmds) {
    for (u32 ix = 0; ix < num_cmds; ix++) {
        MostPrintf(buffer, "%s %s: %s\n", commands[ix].name,
                   commands[ix].usage, commands[ix].desc);
    }
}

void MostInit(u32 mask) {
    MostTraceMask = mask;
    MosInitMutex(&MostMutex);
    MosInitQueue(&RxQueue, QueueBuf, count_of(QueueBuf));
    HalRegisterRxUARTCallback(MostRxCallback);
}
