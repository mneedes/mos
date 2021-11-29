
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS command shell
//   Serial command shell
//   Up and down arrows can be used for command history
//   Dynamic command tables add/remove)
//   Double quote (") allows for embedded spaces in arguments
//

#ifndef _MOS_SHELL_H_
#define _MOS_SHELL_H_

#include <mos/defs.h>

enum MosCommandStatus {
    CMD_ERR_OUT_OF_RANGE = -3,
    CMD_ERR_NOT_FOUND = -2,
    CMD_ERR = -1,
    CMD_OK,
    CMD_OK_NO_HISTORY,
};
typedef s32 MosCommandStatus;

// Command shell callback
typedef MOS_ISR_SAFE MosCommandStatus (MosCommandFunc)(s32 argc, char * argv[]);

// Command entry
typedef struct {
    MosCommandFunc * func;
    char * name;
    char * desc;
    char * usage;
    MosLink link;
} MosShellCommand;

typedef struct MosShell {
    MosMutex mtx;
    MosList cmd_list;
    void * cmd_buffer;
    u16 cmd_buffer_len;
    u16 max_cmd_line_size;
    u16 cmd_ix;
    s16 cmd_max_ix;
    s16 cmd_history_ix;
} MosShell;

typedef enum {
    MOS_CMD_RECEIVED,
    MOS_CMD_UP_ARROW,
    MOS_CMD_DOWN_ARROW
} MosCommandResult;

// Command shell support
void MosInitShell(MosShell * shell, u16 cmd_buffer_len, u16 max_cmd_line_size,
                  void * cmd_buffer, bool isSerialConsole);
void MosAddCommand(MosShell * shell, MosShellCommand * cmd);
void MosRemoveCommand(MosShell * shell, MosShellCommand * cmd);
MosShellCommand * MosFindCommand(MosShell * shell, char * name);
void MosPrintCommandHelp(MosShell * shell);
//  Parser support quotes and escape character '\'
MosCommandResult MosGetNextCommand(char * prompt, char * cmd, u32 max_cmd_len);
//  NOTE: MosParseCmd modifies args in place like strtok_r()
u32 MosParseCommand(char * argv[], char * args, u32 max_argc);
MosCommandStatus MosRunCommand(MosShell * shell, char * cmd_buf_in);
void MosRunShell(MosShell * shell);

#endif
