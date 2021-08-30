
//  Copyright 2019-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Application Entry
//

#include <mos/hal.h>
#include <mos/kernel.h>
#include <mos/trace.h>
#include <mos/context.h>

#include <bsp_hal.h>

#include "tb.h"

static MosContext AppContext;
static MosClient  AppClient;
static MosClient  AppClient2;

static MosThread RunAppThread;
static u8 AppStack[1024];

enum {
    AppClientMessageID_Ping = MosContextMessageID_FirstUserMessage
};

//
// This client example sends 1500 messages to other thread which
//   would normally overwhelm the queue but uses the Resume feature
//   to throttle transmission and allow the other client to consume.
//

static bool SendMessages(MosContextMessage * msg) {
    static u32 count = 0;
    while (1) {
        if (count < 1501) MosSetContextMessage(msg, &AppClient2, AppClientMessageID_Ping);
        else MosSetContextBroadcastMessage(msg, MosContextMessageID_Stop);
        MosSetContextMessagePayload(msg, (void *) count);
        if (MosTrySendMessageToContext(&AppContext, msg)) {
            if (count++ == 1501) return true;
        } else break;
    }
    return false;
}

static bool ClientHandler(MosContextMessage * msg) {
    MosClient * client = msg->client;
    switch (msg->id) {
    case MosContextMessageID_Start:
        MosPrintf("Start %d\n", (u32)client->priv_data);
        if (client == &AppClient) return SendMessages(msg);
        break;
    case AppClientMessageID_Ping:
        MosPrintf("Ping %d: %d\n", (u32)client->priv_data, (u32)msg->payload);
        break;
    case MosContextMessageID_Resume:
        return SendMessages(msg);
    case MosContextMessageID_Stop:
        MosPrintf("Stop %d\n", (u32)client->priv_data);
        break;
    default:
        break;
    }
    return true;
}

static s32 RunApp(s32 arg) {
    MOS_UNUSED(arg);
    // Start and stop a background app with a few clients
    MosStartContext(&AppContext, 2, 1024, 1);
    MosStartClient(&AppContext, &AppClient, ClientHandler, (void *) 1);
    MosStartClient(&AppContext, &AppClient2, ClientHandler, (void *) 2);
    MosWaitForContextStop(&AppContext);
    MosPrintf("Application exit\n");
    return 0;
}

int main() {
    // Initialize hardware, set up SysTick, NVIC priority groups, etc.
    HalInit();

    // Run init before calling any MOS functions
    MosInit();

    // Init trace before calling any print functions
    MosInitTrace(TRACE_INFO | TRACE_ERROR | TRACE_FATAL, true);
    MosPrintf("\nMaintainable OS (Version %s)\n", MosGetParams()->version);
    MosPrint("Copyright 2019-2021, Matthew Needes  All Rights Reserved\n");

#if 0
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
        MosPrint("Debug Enabled\n");
    }
    u32 cpu_id = SCB->CPUID;
    MosPrintf("CPU ID(0x%08X) ARCH(%1X) PART_NO(%02X)\n", cpu_id, (cpu_id >> 16) & 0xF,
                  (cpu_id >> 4) & 0xFFF);
#endif

    MosInitAndRunThread(&RunAppThread, 0, RunApp, 0, AppStack, sizeof(AppStack));

    // Initialize and Run test bench example Application.
    if (InitTestBench() == 0) {
        // Start multitasking, running App threads.
        MosRunScheduler();
    }
    return -1;
}
