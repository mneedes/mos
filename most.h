
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS tracing facility and command shell
//   Mutex to synchronize printing from different threads
//   Lightweight format strings
//   Maskable trace messaging
//   Serial command shell
//

#ifndef _MOST_H_
#define _MOST_H_

#include <stdarg.h>

// Display trace message
#define MostTrace(level, args...) \
    if (MostTraceMask & (level)) \
        { MostTraceMessage(__FILE__ "[" MOS__LINE__ "]:", args); }

// Display trace hex dump
#define MostHexDump(level, name_p, addr_p, size) \
    if (MostTraceMask & (level)) \
        { MostHexDumpMessage(__FILE__ "[" MOS__LINE__ "]:", (name_p), (addr_p), (size)); }

// Set the trace mask
#define MostSetMask(mask)          { MostTraceMask = (mask); }

// Trace mask
extern u32 MostTraceMask;

// Command shell callback
typedef s32 (MostCmdFunc)(s32 argc, char * argv[]);

// Command list
typedef struct MostCmd {
    MostCmdFunc * func;
    char * name;
    char * desc;
    char * usage;
} MostCmd;

typedef enum {
    MOST_CMD_RECEIVED,
    MOST_CMD_UP_ARROW,
    MOST_CMD_DOWN_ARROW,
    //MOST_CMD_TIMEOUT,
} MostCmdResult;

void MostItoa(char * restrict * out, s32 input, u16 base,
              bool is_signed, bool is_upper,
              const u16 min_digits, char pad_char);

void MostItoa64(char * restrict * out, s64 input, u16 base,
                bool is_signed, bool is_upper,
                const u16 min_digits, char pad_char);

u32 MostPrint(char * str);
u32 MostPrintf(const char * fmt, ...);

// Parse format string and arguments into provided buffer
void MostTraceMessage(const char * id, const char * fmt, ...);

// Create a hex dump into provided buffer
void MostHexDumpMessage(const char * id, const char * name,
                        const void * addr, u32 size);

// Callers can take mutex for multi-line prints
void MostTakeMutex(void);
bool MostTryMutex(void);
void MostGiveMutex(void);

// Command shell support
MostCmdResult MostGetNextCmd(char * prompt, char * cmd, u32 max_cmd_len);
u32 MostParseCmd(char * argv[], char *args, u32 max_argc);
MostCmd * MostFindCmd(char * name, MostCmd * commands, u32 num_cmds);
void MostPrintHelp(MostCmd * commands, u32 num_cmds);

// Initialize module
void MostInit(u32 mask);

#endif
