
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
static u8         AppStack[1024];
static MosClient  AppClient;
static MosClient  AppClient2;

static MosContextMessage AppQueue[1];
static MosContextTimer   AppTimer;

static MosThread RunAppThread;
static u8        RunAppStack[512];

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
static bool ClientHandler(MosContextMessage * msg) {
    // Number of pings plus final StopContext
    const u16 max_count = 200 + 1;
    MosClient * client = msg->client;
    switch (msg->id) {
    case MosContextMessageID_StartClient:
        MosPrintf("Start %d\n", (u32)client->priv_data);
        if (client == &AppClient) {
            MosInitContextTimer(&AppTimer, &AppContext);
            MosSetContextMessage(msg, &AppClient, AppClientMessageID_SendBurst);
            MosSetContextTimer(&AppTimer, 500, msg);
        }
        break;
    case AppClientMessageID_Ping:
        MosPrintf("Ping %d: %d\n", (u32)client->priv_data, (u32)msg->data);
        break;
    case AppClientMessageID_SendBurst:
    case MosContextMessageID_ResumeClient: {
        static u32 count = 0;
        static u32 burst_count = 0;
        while (1) {
            // After pings are sent, context shuts itself down via broadcast
            if (count < max_count) MosSetContextMessage(msg, &AppClient2, AppClientMessageID_Ping);
            else MosSetContextBroadcastMessage(msg, MosContextMessageID_StopContext);
            MosSetContextMessageData(msg, (void *)count);
            if (MosTrySendMessageToContext(&AppContext, msg)) {
                if (count++ == max_count) return true;
                if (++burst_count == 5) {
                    burst_count = 0;
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
    MosInitContext(&AppContext, 2, AppStack, sizeof(AppStack), AppQueue, count_of(AppQueue));
    MosStartClient(&AppContext, &AppClient, ClientHandler, (void *) 1);
    MosStartClient(&AppContext, &AppClient2, ClientHandler, (void *) 2);
    MosStartContext(&AppContext);
    // In this case, context stops itself
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

    MosInitAndRunThread(&RunAppThread, 1, RunApp, 3, RunAppStack, sizeof(RunAppStack));

    // Initialize and Run test bench example Application.
    if (InitTestBench() == 0) {
        // Start multitasking, running App threads.
        MosRunScheduler();
    }
    return -1;
}
