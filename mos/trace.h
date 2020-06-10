
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS tracing facility
//   Mutex to synchronize printing from different threads
//   Lightweight format strings
//   Maskable trace messaging
//

#ifndef _MOS_TRACE_H_
#define _MOS_TRACE_H_

#include <stdarg.h>

#include "mos/kernel.h"

// Display trace message
#define MosLogTrace(level, args...) \
    if (MosTraceMask & (level)) \
        { MosLogTraceMessage(__FILE__ "[" MOS__LINE__ "]:", args); }

// Display trace hex dump
#define MosLogHexDump(level, name_p, addr_p, size) \
    if (MosTraceMask & (level)) \
        { MosLogHexDumpMessage(__FILE__ "[" MOS__LINE__ "]:", \
                               (name_p), (addr_p), (size)); }

// Set the trace mask
#define MosSetMask(mask) { MosTraceMask = (mask); }

// Trace mask
extern u32 MosTraceMask;

// Initialize module
//   if enable_raw_print_hook is true, then operate low-level prints
//   through this module.
void MosInitTrace(u32 mask, bool enable_raw_print_hook);

u32 MosItoa(char * restrict out, s32 input, u16 base, bool is_upper,
            u16 min_digits, char pad_char, bool is_signed);

u32 MosItoa64(char * restrict out, s64 input, u16 base, bool is_upper,
              u16 min_digits, char pad_char, bool is_signed);

u32 MosPrint(char * str);
u32 MosPrintf(const char * fmt, ...);

// Parse format string and arguments into provided buffer
void MosLogTraceMessage(char * id, const char * fmt, ...);

// Create a hex dump into provided buffer
void MosLogHexDumpMessage(char * id, char * name,
                          const void * addr, u32 size);

// Callers can use mutex for multi-line prints
void MosTakeTraceMutex(void);
bool MosTryTraceMutex(void);
void MosGiveTraceMutex(void);

#endif
