
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS tracing facility and command shell support
//

#include <mos/static_kernel.h>
#include <mos/hal.h>

#include <mos/format_string.h>
#include <mos/internal/trace.h>
#include <mos/trace.h>

u32 mosTraceMask = 0;
static MosMutex TraceMutex;

static char PrintBuffer[MOS_PRINT_BUFFER_SIZE + 1];
static char RawPrintBuffer[MOS_PRINT_BUFFER_SIZE + 1];

void _mosPrintCh(char ch) {
    mosLockMutex(&TraceMutex);
    HalSendToTxUART(ch);
    mosUnlockMutex(&TraceMutex);
}

u32 _mosPrint(char * str) {
    u32 cnt = 0;
    for (char * ch = str; *ch != '\0'; ch++, cnt++) {
        if (*ch == '\n') HalSendToTxUART('\r');
        HalSendToTxUART(*ch);
    }
    return cnt;
}

static void mosRawVPrintfCallback(const char * fmt, va_list args) {
    u32 mask = mosDisableInterrupts();
    mosVSNPrintf(RawPrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    _mosPrint(RawPrintBuffer);
    mosEnableInterrupts(mask);
}

void mosInitTrace(u32 mask, bool enable_raw_vprintf_hook) {
    mosTraceMask = mask;
    mosInitMutex(&TraceMutex);
    PrintBuffer[MOS_PRINT_BUFFER_SIZE] = '\0';
    RawPrintBuffer[MOS_PRINT_BUFFER_SIZE] = '\0';
    if (enable_raw_vprintf_hook)
        mosRegisterRawVPrintfHook(mosRawVPrintfCallback,
                                  (char (*)[MOS_PRINT_BUFFER_SIZE])&RawPrintBuffer);
}

s32 mosPrint(char * str) {
    mosLockMutex(&TraceMutex);
    s32 cnt = _mosPrint(str);
    mosUnlockMutex(&TraceMutex);
    return cnt;
}

s32 mosPrintf(const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mosLockMutex(&TraceMutex);
    s32 cnt = mosVSNPrintf(PrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    _mosPrint(PrintBuffer);
    mosUnlockMutex(&TraceMutex);
    va_end(args);
    if (cnt > MOS_PRINT_BUFFER_SIZE) cnt = MOS_PRINT_BUFFER_SIZE;
    return cnt;
}

void mosLogTraceMessage(char * id, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mosLockMutex(&TraceMutex);
    _mosPrint(id);
    mosVSNPrintf(PrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    _mosPrint(PrintBuffer);
    mosUnlockMutex(&TraceMutex);
    va_end(args);
}

void mosLogHexDumpMessage(char * id, char * name,
                          const void * addr, mos_size size) {
    const u8 * restrict data = (const u8 *) addr;
    mosLockMutex(&TraceMutex);
    _mosPrint(id);
    _mosPrint(name);
    _mosPrint("\n");
    // 16 bytes per line
    for (u32 lines = (size >> 4) + 1; lines > 0; lines--) {
        char * buf = PrintBuffer;
        u32 bytes = 16;
        if (lines == 1) {
            bytes = size & 15;
            if (bytes == 0) break;
        }
        // Address
        buf += mosItoa(buf, (s32) data, 16, true, 8, '0', false);
        *buf++ = ' ';
        *buf++ = ' ';
        for (; bytes > 0; bytes--) {
            buf += mosItoa(buf, *data, 16, true, 2, '0', false);
            *buf++ = ' ';
            data++;
        }
        *buf++ = '\n';
        *buf++ = '\0';
        _mosPrint(PrintBuffer);
    }
    mosUnlockMutex(&TraceMutex);
}

void mosLockTraceMutex(void) {
    mosLockMutex(&TraceMutex);
}

bool mosTryTraceMutex(void) {
    return mosTryMutex(&TraceMutex);
}

void mosUnlockTraceMutex(void) {
    mosUnlockMutex(&TraceMutex);
}
