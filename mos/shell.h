
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS command shell
//   Serial command shell
//   Up and down arrows can be used for command history
//   Dynamic command tables add/remove)
//   Double quote (") allows for embedded spaces in arguments
//

#ifndef _MOS_SHELL_H_
#define _MOS_SHELL_H_

#include <stdarg.h>

#include "mos/kernel.h"

// Command shell callback
typedef s32 (MosCmdFunc)(s32 argc, char * argv[]);

// Command entry
typedef struct MosCmd {
    MosCmdFunc * func;
    char * name;
    char * desc;
    char * usage;
    MosList list;
} MosCmd;

// Command List
typedef struct MosCmdList {
    MosList list;
    MosMutex mtx;
} MosCmdList;

typedef enum {
    MOSS_CMD_RECEIVED,
    MOSS_CMD_UP_ARROW,
    MOSS_CMD_DOWN_ARROW,
    //MOSS_CMD_TIMEOUT,
} MosCmdResult;

// Command shell support
void MosInitShell(void);
void MosInitCmdList(MosCmdList * cmd_list);
void MosAddCmd(MosCmdList * cmd_list, MosCmd * cmd);
void MosRemoveCmd(MosCmdList * cmd_list, MosCmd * cmd);
MosCmd * MosFindCmd(MosCmdList * cmd_list, char * name);
void MosPrintCmdHelp(MosCmdList * cmd_list);
//  Parser support quotes and escape character '\'
MosCmdResult MosGetNextCmd(char * prompt, char * cmd, u32 max_cmd_len);
//  NOTE: MosParseCmd modifies args in place like _strtok()
u32 MosParseCmd(char * argv[], char * args, u32 max_argc);

#endif
