
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
typedef s32 (MossCmdFunc)(s32 argc, char * argv[]);

// Command entry
typedef struct MossCmd {
    MossCmdFunc * func;
    char * name;
    char * desc;
    char * usage;
    MosList list;
} MossCmd;

// Command List
typedef struct MossCmdList {
    MosList list;
    MosMutex mtx;
} MossCmdList;

typedef enum {
    MOSS_CMD_RECEIVED,
    MOSS_CMD_UP_ARROW,
    MOSS_CMD_DOWN_ARROW,
    //MOSS_CMD_TIMEOUT,
} MossCmdResult;

// Command shell support
void MossInit(void);
void MossInitCmdList(MossCmdList * cmd_list);
void MossAddCmd(MossCmdList * cmd_list, MossCmd * cmd);
void MossRemoveCmd(MossCmdList * cmd_list, MossCmd * cmd);
MossCmd * MossFindCmd(MossCmdList * cmd_list, char * name);
void MossPrintCmdHelp(MossCmdList * cmd_list);
//  Parser support quotes and escape character '\'
MossCmdResult MossGetNextCmd(char * prompt, char * cmd, u32 max_cmd_len);
//  NOTE: MossParseCmd modifies args in place like _strtok()
u32 MossParseCmd(char * argv[], char * args, u32 max_argc);

#endif
