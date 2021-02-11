
//  Copyright 2019-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS tracing facility and command shell support
//

// TODO: Rotating logs

#include <mos/kernel.h>
#include <mos/hal.h>

#include <mos/format_string.h>
#include <mos/internal/trace.h>
#include <mos/trace.h>

#define MOS_PRINT_BUFFER_SIZE   128

u32 MosTraceMask = 0;
static MosMutex PrintMutex;

static char PrintBuffer[MOS_PRINT_BUFFER_SIZE];
static char RawPrintBuffer[MOS_PRINT_BUFFER_SIZE];

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
    MosVSNPrintf(RawPrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    va_end(args);
    _MosPrint(RawPrintBuffer);
}

void MosInitTrace(u32 mask, bool enable_raw_printf_hook) {
    MosTraceMask = mask;
    MosInitMutex(&PrintMutex);
    if (enable_raw_printf_hook)
        MosRegisterRawPrintfHook(MosRawPrintfCallback);
}

s32 MosPrint(char * str) {
    MosTakeMutex(&PrintMutex);
    s32 cnt = _MosPrint(str);
    MosGiveMutex(&PrintMutex);
    return cnt;
}

s32 MosPrintf(const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MosTakeMutex(&PrintMutex);
    s32 cnt = MosVSNPrintf(PrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    _MosPrint(PrintBuffer);
    MosGiveMutex(&PrintMutex);
    va_end(args);
    return cnt;
}

void MosLogTraceMessage(char * id, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MosTakeMutex(&PrintMutex);
    _MosPrint(id);
    MosVSNPrintf(PrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
    _MosPrint(PrintBuffer);
    MosGiveMutex(&PrintMutex);
    va_end(args);
}

void MosLogHexDumpMessage(char * id, char * name,
                          const void * addr, mos_size size) {
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
