
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS command shell support
//

#include <string.h>

#include <mos/static_kernel.h>
#include <mos/queue.h>
#include <mos/hal.h>
#include <mos/trace.h>
#include <mos/internal/trace.h>

#include <mos/shell.h>

static MosQueue RxQueue;
static u32 RxQueueBuf[16];

MOS_ISR_SAFE static void mosRxCallback(char ch) {
    mosTrySendToQueue32(&RxQueue, (u32) ch);
}

void mosInitShell(MosShell * shell, u16 cmd_buffer_len, u16 max_cmd_line_size,
                  void * cmd_buffer, bool isSerialConsole) {
    shell->cmd_buffer_len = cmd_buffer_len;
    shell->max_cmd_line_size = max_cmd_line_size;
    shell->cmd_buffer = cmd_buffer;
    shell->cmd_ix = 0;
    shell->cmd_max_ix = 0;
    shell->cmd_history_ix = 0;
    mosInitMutex(&shell->mtx);
    mosInitList(&shell->cmd_list);
    if (isSerialConsole) {
        mosInitQueue32(&RxQueue, RxQueueBuf, count_of(RxQueueBuf));
        HalRegisterRxUARTCallback(mosRxCallback);
    }
}

void mosAddCommand(MosShell * shell, MosShellCommand * cmd) {
    mosLockMutex(&shell->mtx);
    mosAddToEndOfList(&shell->cmd_list, &cmd->link);
    mosUnlockMutex(&shell->mtx);
}

void mosRemoveCommand(MosShell * shell, MosShellCommand * cmd) {
    mosLockMutex(&shell->mtx);
    mosRemoveFromList(&cmd->link);
    mosUnlockMutex(&shell->mtx);
}

MosShellCommand * mosFindCommand(MosShell * shell, char * name) {
    mosLockMutex(&shell->mtx);
    MosList * list = &shell->cmd_list;
    for (MosLink * elm = list->pNext; elm != list; elm = elm->pNext) {
        MosShellCommand * cmd = container_of(elm, MosShellCommand, link);
        if (strcmp(name, cmd->name) == 0) {
            mosUnlockMutex(&shell->mtx);
            return cmd;
        }
    }
    mosUnlockMutex(&shell->mtx);
    return NULL;
}

void mosPrintCommandHelp(MosShell * shell) {
    MosList * list = &shell->cmd_list;
    mosLockMutex(&shell->mtx);
    mosLockTraceMutex();
    for (MosLink * elm = list->pNext; elm != list; elm = elm->pNext) {
        MosShellCommand * cmd = container_of(elm, MosShellCommand, link);
        mosPrintf("%s %s: %s\n", cmd->name, cmd->usage, cmd->desc);
    }
    mosUnlockTraceMutex();
    mosUnlockMutex(&shell->mtx);
}

MosCommandResult mosGetNextCommand(char * prompt, char * cmd, u32 max_cmd_len) {
    enum {
        KEY_NORMAL,
        KEY_ESCAPE,
        KEY_ESCAPE_PLUS_BRACKET
    };
    static u32 buf_ix = 0;
    static bool last_ch_was_arrow = false;
    mosLockTraceMutex();
    if (buf_ix) {
        for (u32 ix = 0; ix < buf_ix; ix++) _mosPrint("\b \b");
    } else if (prompt && !last_ch_was_arrow) {
        _mosPrint(prompt);
    }
    buf_ix = _mosPrint(cmd);
    mosUnlockTraceMutex();
    last_ch_was_arrow = false;
    u32 state = KEY_NORMAL;
    while (1) {
        char ch = (char) mosReceiveFromQueue32(&RxQueue);
        switch (state) {
        default:
        case KEY_NORMAL:
            switch (ch) {
            default:
                if (buf_ix < max_cmd_len && ch > 31) {
                    _mosPrintCh(ch);
                    cmd[buf_ix++] = ch;
                }
                break;
            case 27:
                state = KEY_ESCAPE;
                break;
            case '\b':
            case 127:
                if (buf_ix) {
                    mosPrint("\b \b");
                    buf_ix--;
                }
                break;
            case '\r':
                mosPrint("\n");
                cmd[buf_ix] = '\0';
                buf_ix = 0;
                return MOS_CMD_RECEIVED;
            }
            break;
        case KEY_ESCAPE:
            if (ch == '[') state = KEY_ESCAPE_PLUS_BRACKET;
            else state = KEY_NORMAL;
            break;
        case KEY_ESCAPE_PLUS_BRACKET:
            if (ch == 'A') {
                last_ch_was_arrow = true;
                cmd[buf_ix] = '\0';
                return MOS_CMD_UP_ARROW;
            } else if (ch == 'B') {
                last_ch_was_arrow = true;
                cmd[buf_ix] = '\0';
                return MOS_CMD_DOWN_ARROW;
            } else {
                state = KEY_NORMAL;
            }
            break;
        }
    }
}

u32 mosParseCommand(char * argv[], char * args, u32 max_argc) {
    if (args == NULL || args[0] == '\0') return 0;
    char * ch_in = args, * ch_out = args;
    bool in_arg = false, in_quote = false;
    u32 argc = 0;
    while (*ch_in != '\0') {
        switch (*ch_in) {
        case ' ':
        case '\t':
            if (!in_quote) {
                if (in_arg) {
                    in_arg = false;
                    *ch_out++ = '\0';
                }
                ch_in++;
                continue;
            }
            break;
        case '"':
            in_quote = !in_quote;
            ch_in++;
            continue;
        case '\\':
            ch_in++;
            break;
        default:
            break;
        }
        if (!in_arg) {
            if (argc < max_argc) argv[argc++] = ch_out;
            in_arg = true;
        }
        *ch_out++ = *ch_in++;
    }
    *ch_out = '\0';
    return argc;
}

// Calculate a valid command index at the offset from the provided index
static u32 CalcOffsetCommandIx(s32 ix, s32 max_ix, s32 offset) {
    s32 new_ix = (ix + offset) % (max_ix + 1);
    if (new_ix < 0) new_ix += (max_ix + 1);
    return (u32)new_ix;
}

// NOTE: this function is one-level recursive when running commands out of history.
MosCommandStatus mosRunCommand(MosShell * shell, char * cmd_buf_in) {
    const u32 max_cmd_args = 10;
    u32 argc;
    char * argv[max_cmd_args];
    char cmd_buf[shell->max_cmd_line_size];
    char (* CmdBuffer)[shell->cmd_buffer_len][shell->max_cmd_line_size] = shell->cmd_buffer;
    strncpy(cmd_buf, cmd_buf_in, sizeof(cmd_buf));
    argc = mosParseCommand(argv, cmd_buf, max_cmd_args);
    if (argc == 0) return CMD_OK_NO_HISTORY;
    MosShellCommand * cmd = mosFindCommand(shell, argv[0]);
    if (cmd) {
        return cmd->func(argc, argv);
    } else if (argv[0][0] == '!') {
        if (argv[0][1] == '!') {
            if (shell->cmd_max_ix > 0) {
                u32 run_cmd_ix = CalcOffsetCommandIx(shell->cmd_ix, shell->cmd_max_ix, -1);
                strcpy((*CmdBuffer)[shell->cmd_ix], (*CmdBuffer)[run_cmd_ix]);
                return mosRunCommand(shell, (*CmdBuffer)[shell->cmd_ix]);
            } else return CMD_ERR_OUT_OF_RANGE;
        } else if (argv[0][1] == '-') {
            if (argv[0][2] >= '1' && argv[0][2] <= '9') {
                s8 offset = argv[0][2] - '0';
                if (offset <= shell->cmd_max_ix) {
                    u32 run_cmd_ix = CalcOffsetCommandIx(shell->cmd_ix,
                                                         shell->cmd_max_ix, -offset);
                    strcpy((*CmdBuffer)[shell->cmd_ix], (*CmdBuffer)[run_cmd_ix]);
                    return mosRunCommand(shell, (*CmdBuffer)[shell->cmd_ix]);
                } else return CMD_ERR_OUT_OF_RANGE;
            }
        }
    } else if (strcmp(argv[0], "?") == 0 || strcmp(argv[0], "help") == 0) {
        mosPrintCommandHelp(shell);
        mosPrint("!!: Repeat prior command\n");
        mosPrint("!-#: Repeat #th prior command\n");
        mosPrint("h -or- history: Display command history\n");
        mosPrint("? -or- help: Display command help\n");\
        return CMD_OK_NO_HISTORY;
    } else if (strcmp(argv[0], "h") == 0 || strcmp(argv[0], "history") == 0) {
        for (s32 ix = shell->cmd_max_ix; ix > 0; ix--) {
            u32 hist_cmd_ix = CalcOffsetCommandIx(shell->cmd_ix, shell->cmd_max_ix, -ix);
            mosLockTraceMutex();
            mosPrintf("%2d: ", -ix);
            mosPrint((*CmdBuffer)[hist_cmd_ix]);
            mosPrint("\n");
            mosUnlockTraceMutex();
        }
        return CMD_OK_NO_HISTORY;
    } else if (argv[0][0] == '\0') {
        return CMD_OK_NO_HISTORY;
    }
    return CMD_ERR_NOT_FOUND;
}

void mosRunShell(MosShell * shell) {
    char (* CmdBuffer)[shell->cmd_buffer_len][shell->max_cmd_line_size] = shell->cmd_buffer;
    while (1) {
        MosCommandResult result;
        MosCommandStatus status;
        result = mosGetNextCommand("# ", (*CmdBuffer)[shell->cmd_ix], shell->max_cmd_line_size);
        switch (result) {
        case MOS_CMD_RECEIVED:
            status = mosRunCommand(shell, (*CmdBuffer)[shell->cmd_ix]);
            switch (status) {
            case CMD_OK_NO_HISTORY:
                break;
            case CMD_ERR_NOT_FOUND:
                mosPrint("[ERR] Command not found...\n");
                break;
            case CMD_ERR_OUT_OF_RANGE:
                mosPrint("[ERR] Index out of range...\n");
                break;
            case CMD_OK:
                mosPrint("[OK]\n");
                if (++shell->cmd_ix == shell->cmd_buffer_len) shell->cmd_ix = 0;
                if (shell->cmd_ix > shell->cmd_max_ix) shell->cmd_max_ix = shell->cmd_ix;
                break;
            default:
            case CMD_ERR:
                mosPrint("[ERR]\n");
                if (++shell->cmd_ix == shell->cmd_buffer_len) shell->cmd_ix = 0;
                if (shell->cmd_ix > shell->cmd_max_ix) shell->cmd_max_ix = shell->cmd_ix;
                break;
            }
            shell->cmd_history_ix = shell->cmd_ix;
            (*CmdBuffer)[shell->cmd_ix][0] = '\0';
            break;
        case MOS_CMD_UP_ARROW:
            // Rotate history back one, skipping over current index
            shell->cmd_history_ix = CalcOffsetCommandIx(shell->cmd_history_ix, shell->cmd_max_ix, -1);
            if (shell->cmd_history_ix == shell->cmd_ix)
                shell->cmd_history_ix = CalcOffsetCommandIx(shell->cmd_history_ix, shell->cmd_max_ix, -1);
            strcpy((*CmdBuffer)[shell->cmd_ix], (*CmdBuffer)[shell->cmd_history_ix]);
            break;
        case MOS_CMD_DOWN_ARROW:
            // Rotate history forward one, skipping over current index
            shell->cmd_history_ix = CalcOffsetCommandIx(shell->cmd_history_ix, shell->cmd_max_ix, 1);
            if (shell->cmd_history_ix == shell->cmd_ix)
                shell->cmd_history_ix = CalcOffsetCommandIx(shell->cmd_history_ix, shell->cmd_max_ix, 1);
            strcpy((*CmdBuffer)[shell->cmd_ix], (*CmdBuffer)[shell->cmd_history_ix]);
            break;
        default:
            break;
        }
    }
}
