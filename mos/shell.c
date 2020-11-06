
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS command shell support
//

#include <string.h>

#include <mos/hal.h>
#include <mos/trace.h>
#include <mos/internal/trace.h>
#include <mos/shell.h>

static MosQueue RxQueue;
static u32 RxQueueBuf[16];

// Callback must be ISR safe
static void MosRxCallback(char ch) {
    MosTrySendToQueue(&RxQueue, (u32)ch);
}

void MosInitShell() {
    MosInitQueue(&RxQueue, RxQueueBuf, count_of(RxQueueBuf));
    HalRegisterRxUARTCallback(MosRxCallback);
}

void MosInitCmdList(MosCmdList * cmd_list) {
    MosInitMutex(&cmd_list->mtx);
    MosInitList(&cmd_list->list);
}

void MosAddCmd(MosCmdList * cmd_list, MosCmd * cmd) {
    MosTakeMutex(&cmd_list->mtx);
    MosInitList(&cmd->list);
    MosAddToList(&cmd_list->list, &cmd->list);
    MosGiveMutex(&cmd_list->mtx);
}

void MosRemoveCmd(MosCmdList * cmd_list, MosCmd * cmd) {
    MosTakeMutex(&cmd_list->mtx);
    MosRemoveFromList(&cmd->list);
    MosGiveMutex(&cmd_list->mtx);
}

MosCmd * MosFindCmd(MosCmdList * cmd_list, char * name) {
    MosTakeMutex(&cmd_list->mtx);
    MosList * list = &cmd_list->list;
    for (MosList * elm = list->next; elm != list; elm = elm->next) {
        MosCmd * cmd = container_of(elm, MosCmd, list);
        if (strcmp(name, cmd->name) == 0) {
            MosGiveMutex(&cmd_list->mtx);
            return cmd;
        }
    }
    MosGiveMutex(&cmd_list->mtx);
    return NULL;
}

void MosPrintCmdHelp(MosCmdList * cmd_list) {
    MosList * list = &cmd_list->list;
    MosTakeMutex(&cmd_list->mtx);
    MosTakeTraceMutex();
    for (MosList * elm = list->next; elm != list; elm = elm->next) {
        MosCmd * cmd = container_of(elm, MosCmd, list);
        MosPrintf("%s %s: %s\n", cmd->name, cmd->usage, cmd->desc);
    }
    MosGiveTraceMutex();
    MosGiveMutex(&cmd_list->mtx);
}

MosCmdResult MosGetNextCmd(char * prompt, char * cmd, u32 max_cmd_len) {
    enum {
        KEY_NORMAL,
        KEY_ESCAPE,
        KEY_ESCAPE_PLUS_BRACKET
    };
    static u32 buf_ix = 0;
    static bool last_ch_was_arrow = false;
    MosTakeTraceMutex();
    if (buf_ix) {
        for (u32 ix = 0; ix < buf_ix; ix++) _MosPrint("\b \b");
    } else if (prompt && !last_ch_was_arrow) {
        _MosPrint(prompt);
    }
    buf_ix = _MosPrint(cmd);
    MosGiveTraceMutex();
    last_ch_was_arrow = false;
    u32 state = KEY_NORMAL;
    while (1) {
        char ch = (char)MosReceiveFromQueue(&RxQueue);
        switch (state) {
        default:
        case KEY_NORMAL:
            switch (ch) {
            default:
                if (buf_ix < max_cmd_len && ch > 31) {
                    _MosPrintCh(ch);
                    cmd[buf_ix++] = ch;
                }
                break;
            case 27:
                state = KEY_ESCAPE;
                break;
            case '\b':
            case 127:
                if (buf_ix) {
                    MosPrint("\b \b");
                    buf_ix--;
                }
                break;
            case '\r':
                MosPrint("\n");
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

u32 MosParseCmd(char * argv[], char * args, u32 max_argc) {
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
