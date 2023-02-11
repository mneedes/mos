
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS tracing facility
//   Mutex to synchronize printing from different threads
//   Maskable trace messaging
//

#ifndef _MOS_TRACE_H_
#define _MOS_TRACE_H_

#include <mos/defs.h>

// Display trace message
#define mosLogTrace(level, args...) \
    if (mosTraceMask & (level)) \
        { mosLogTraceMessage(__FILE__ "[" MOS__LINE__ "]:", args); }

// Display trace hex dump
#define mosLogHexDump(level, name_p, addr_p, size) \
    if (mosTraceMask & (level)) \
        { mosLogHexDumpMessage(__FILE__ "[" MOS__LINE__ "]:", \
                               (name_p), (addr_p), (size)); }

// Set the trace mask
#define mosSetMask(mask) { mosTraceMask = (mask); }

// Trace mask
extern u32 mosTraceMask;

// Initialize module
//   if enable_raw_print_hook is true, then operate low-level prints
//   through this module.
void mosInitTrace(u32 mask, bool enable_raw_vprintf_hook);

s32 mosPrint(char * str);
s32 mosPrintf(const char * fmt, ...);

// Parse format string and arguments into provided buffer
void mosLogTraceMessage(char * id, const char * fmt, ...);

// Create a hex dump into provided buffer
void mosLogHexDumpMessage(char * id, char * name,
                          const void * addr, mos_size size);

// Callers can use mutex for multi-line prints
void mosLockTraceMutex(void);
bool mosTryTraceMutex(void);
void mosUnlockTraceMutex(void);

#endif
