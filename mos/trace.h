
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
#define MostLogTrace(level, args...) \
    if (MostTraceMask & (level)) \
        { MostLogTraceMessage(__FILE__ "[" MOS__LINE__ "]:", args); }

// Display trace hex dump
#define MostLogHexDump(level, name_p, addr_p, size) \
    if (MostTraceMask & (level)) \
        { MostLogHexDumpMessage(__FILE__ "[" MOS__LINE__ "]:", \
                               (name_p), (addr_p), (size)); }

// Set the trace mask
#define MostSetMask(mask) { MostTraceMask = (mask); }

// Trace mask
extern u32 MostTraceMask;

// Initialize module
//   if enable_raw_print_hook is true, then operate low-level prints
//   through this module.
void MostInit(u32 mask, bool enable_raw_print_hook);

u32 MostItoa(char * restrict out, s32 input, u16 base, bool is_upper,
             u16 min_digits, char pad_char, bool is_signed);

u32 MostItoa64(char * restrict out, s64 input, u16 base, bool is_upper,
               u16 min_digits, char pad_char, bool is_signed);

u32 MostPrint(char * str);
u32 MostPrintf(const char * fmt, ...);

// Parse format string and arguments into provided buffer
void MostLogTraceMessage(char * id, const char * fmt, ...);

// Create a hex dump into provided buffer
void MostLogHexDumpMessage(char * id, char * name,
                           const void * addr, u32 size);

// Callers can use mutex for multi-line prints
void MostTakeMutex(void);
bool MostTryMutex(void);
void MostGiveMutex(void);

#endif
