
// Copyright 2019-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Shared Context Example
//

#include <mos/hal.h>
#include <mos/kernel.h>
#include <mos/trace.h>
#include <mos/context.h>
#include <bsp_hal.h>

#include "tb.h"

static MosContext AppContext;
static u8         AppStack[1024];
static MosClient  AppClient1;
static MosClient  AppClient2;

static MosContextMessage AppQueue[3];
static MosContextTimer   AppTimer;

static MosThread RunAppThread;
static u8        RunAppStack[512];

enum AppClientID {
    AppClientID_1 = 1,
    AppClientID_2
};

enum AppClientMessageID {
    AppClientMessageID_Ping = MosContextMessageID_FirstUserMessage,
    AppClientMessageID_SendBurst
};

//
// This example sends many ping messages in bursts of 5 from one client to another.
//   Ping bursts can potentially overwhelm the queue but the Resume feature is used
//   to guarantee retransmission and allow the other client to consume messages
//   before sending the next message. This code works even if the queue length
//   is 1. In this example the two clients share same handler but that is not
//   required.
//
static bool ClientHandler(MosContextMessage * pMsg) {
    // Number of pings plus final StopContext
    const u16 maxCount = 100 + 1;
    MosClient * pClient = pMsg->pClient;
    static u32 count = 0;
    static u32 burstCount = 0;
    switch (pMsg->id) {
    case MosContextMessageID_StartClient:
        MosPrintf("Start %d\n", (u32)pClient->pPrivData);
        count = 0;
        burstCount = 0;
        if (pClient == &AppClient1) {
            MosInitContextTimer(&AppTimer, &AppContext);
            MosSetContextMessage(pMsg, &AppClient1, AppClientMessageID_SendBurst);
            MosSetContextTimer(&AppTimer, 500, pMsg);
        }
        break;
    case AppClientMessageID_Ping:
        MosPrintf("Ping %d: %d\n", (u32)pClient->pPrivData, (u32)pMsg->pData);
        break;
    case AppClientMessageID_SendBurst:
    case MosContextMessageID_ResumeClient: {
        while (1) {
            // After pings are sent, context shuts itself down via broadcast
            if (count < maxCount) MosSetContextMessage(pMsg, &AppClient2, AppClientMessageID_Ping);
            else MosSetContextBroadcastMessage(pMsg, MosContextMessageID_StopContext);
            MosSetContextMessageData(pMsg, (void *)count);
            if (MosTrySendMessageToContext(&AppContext, pMsg)) {
                if (count++ == maxCount) return true;
                if (++burstCount == 5) {
                    burstCount = 0;
                    MosResetContextTimer(&AppTimer);
                    // Done for now
                    return true;
                }
            } else return false; // request continuation
        }
        break;
    }
    case MosContextMessageID_StopClient:
        MosCancelContextTimer(&AppTimer);
        MosPrintf("Stop %d\n", (u32)pClient->pPrivData);
        break;
    default:
        break;
    }
    return true;
}

static s32 RunApp(s32 arg) {
    MOS_UNUSED(arg);
    // In this case, context stops itself
    MosWaitForContextStop(&AppContext);
    MosPrintf("Application exit\n");
    MosInitContext(&AppContext, 2, AppStack, sizeof(AppStack), AppQueue, count_of(AppQueue));
    MosStartClient(&AppContext, &AppClient1, ClientHandler, (void *)AppClientID_1);
    MosStartClient(&AppContext, &AppClient2, ClientHandler, (void *)AppClientID_2);
    MosStartContext(&AppContext);
    MosPrintf("Application exit 2\n");
    return 0;
}

int main() {
    // Initialize hardware, set up SysTick, NVIC priority groups, etc.
    HalInit();

    // Run init before calling any MOS functions
    MosInit();

    // Init trace before calling any print functions
    MosInitTrace(TRACE_INFO | TRACE_ERROR | TRACE_FATAL, true);
    MosPrintf("\nMaintainable OS (Version " MOS_VERSION_STRING ")\n");
    MosPrint("Copyright 2019-2022, Matthew Needes  All Rights Reserved\n");

    // Start a background app with a few clients
    MosInitContext(&AppContext, 2, AppStack, sizeof(AppStack), AppQueue, count_of(AppQueue));
    MosStartClient(&AppContext, &AppClient1, ClientHandler, (void *)AppClientID_1);
    MosStartClient(&AppContext, &AppClient2, ClientHandler, (void *)AppClientID_2);
    MosStartContext(&AppContext);

    /* Start application to monitor context */
    MosInitAndRunThread(&RunAppThread, 1, RunApp, 3, RunAppStack, sizeof(RunAppStack));
    MosRunScheduler();
    return -1;
}
