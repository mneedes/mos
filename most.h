
//  Copyright 2019 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS tracing facility
//   Mutex to synchronize printing from different threads
//   Lightweight format strings
//   Maskable trace messaging
//

#ifndef _MOST_H_
#define _MOST_H_

#include <stdarg.h>

// Display trace message
#define MostTrace(level, buf_p, args...) \
    if (MostTraceMask & (level)) \
        { MostTraceMessage((buf_p), __FILE__ "[" MOS__LINE__ "]:", args); }

// Display trace hex dump
#define MostHexDump(level, buf_p, name_p, addr_p, size) \
    if (MostTraceMask & (level)) \
        { MostHexDumpMessage((buf_p), __FILE__ "[" MOS__LINE__ "]:", (name_p), (addr_p), (size)); }

// Set the trace mask
#define MostSetMask(mask)          { MostTraceMask = (mask); }

// Trace mask
extern u32 MostTraceMask;

// Basic message printing
void MostPrint(char * str);
void MostPrintf(char * buffer, const char * fmt, ...);

// Parse format string and arguments into provided buffer
void MostTraceMessage(char * buffer, const char * id,
                      const char * fmt, ...);

// Create a hex dump into provided buffer
void MostHexDumpMessage(char * buffer, const char * id,
                        const char * name, const void * addr,
                        u32 size);

void MostItoa32(char * restrict * out,
                s32 input, u16 base,
                bool is_signed, bool is_upper,
                const u16 min_digits, char pad_char);

void MostItoa64(char * restrict * out,
                s64 input, u16 base,
                bool is_signed, bool is_upper,
                const u16 min_digits, char pad_char);

// Callers can take mutex for multi-line prints
void MostTakeMutex(void);
void MostGiveMutex(void);
void MostInit(u32 mask);

#endif
