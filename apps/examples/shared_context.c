
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Shared Context Example
//

#include <mos/hal.h>
#include <mos/static_kernel.h>
#include <mos/trace.h>
#include <mos/context.h>
#include <bsp_hal.h>

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
        mosPrintf("Client start %d\n", (u32)pClient->pPrivData);
        count = 0;
        burstCount = 0;
        if (pClient == &AppClient1) {
            mosInitContextTimer(&AppTimer, &AppContext);
            mosSetContextMessage(pMsg, &AppClient1, AppClientMessageID_SendBurst);
            mosSetContextTimer(&AppTimer, 500, pMsg);
        }
        break;
    case AppClientMessageID_Ping:
        mosPrintf("Ping %d: %d\n", (u32)pClient->pPrivData, (u32)pMsg->pData);
        break;
    case AppClientMessageID_SendBurst:
    case MosContextMessageID_ResumeClient: {
        while (1) {
            // After pings are sent, context shuts itself down via broadcast
            if (count < maxCount) mosSetContextMessage(pMsg, &AppClient2, AppClientMessageID_Ping);
            else mosSetContextBroadcastMessage(pMsg, MosContextMessageID_StopContext);
            mosSetContextMessageData(pMsg, (void *)count);
            if (mosTrySendMessageToContext(&AppContext, pMsg)) {
                if (count++ == maxCount) return true;
                if (++burstCount == 5) {
                    burstCount = 0;
                    mosResetContextTimer(&AppTimer);
                    // Done for now
                    return true;
                }
            } else return false; // request continuation
        }
        break;
    }
    case MosContextMessageID_StopClient:
        mosCancelContextTimer(&AppTimer);
        mosPrintf("Client stop %d\n", (u32)pClient->pPrivData);
        break;
    default:
        break;
    }
    return true;
}

static s32 RunApp(s32 arg) {
    MOS_UNUSED(arg);
    // Wait for the initially created context to stop
    mosWaitForContextStop(&AppContext);
    mosPrintf("Context stopped...\n");
    // Start clients again from this thread
    mosInitContext(&AppContext, 2, AppStack, sizeof(AppStack), AppQueue, count_of(AppQueue));
    mosAddClientToContext(&AppContext, &AppClient1, ClientHandler, (void *)AppClientID_1);
    mosAddClientToContext(&AppContext, &AppClient2, ClientHandler, (void *)AppClientID_2);
    mosStartContext(&AppContext);
    mosWaitForContextStop(&AppContext);
    mosPrintf("Context stopped again...done\n");
    return 0;
}

int main() {
    // Initialize hardware, set up SysTick, NVIC priority groups, etc.
    HalInit();

    // Run init before calling any MOS functions
    mosInit(0);

    // Init trace before calling any print functions
    mosInitTrace(0, true);
    mosPrintf("\nMaintainable OS (Version " MOS_VERSION_STRING ")\n");
    mosPrint("Copyright 2019-2023, Matthew Needes  All Rights Reserved\n");

    // Start a background app with a few clients
    mosInitContext(&AppContext, 2, AppStack, sizeof(AppStack), AppQueue, count_of(AppQueue));
    mosAddClientToContext(&AppContext, &AppClient1, ClientHandler, (void *)AppClientID_1);
    mosAddClientToContext(&AppContext, &AppClient2, ClientHandler, (void *)AppClientID_2);
    mosStartContext(&AppContext);

    /* Start application to monitor context */
    mosInitAndRunThread(&RunAppThread, 1, RunApp, 3, RunAppStack, sizeof(RunAppStack));
    mosRunScheduler();
    return -1;
}
