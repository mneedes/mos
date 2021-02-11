
//  Copyright 2019-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Test Bench
//

#include <errno.h>
#include <string.h>

#include <mos/kernel.h>
#include <mos/heap.h>
#include <mos/thread_heap.h>
#include <mos/format_string.h>
#include <mos/trace.h>
#include <mos/shell.h>
#include <bsp_hal.h>

#include <bsp/hal_tb.h>
#include "tb.h"

#define DFT_STACK_SIZE           384
#define TEST_SHELL_STACK_SIZE    2048

#define MAX_APP_THREADS          6
#define TEST_SHELL_THREAD_ID     0
#define PIGEON_THREAD_ID         (MAX_APP_THREADS - 1)

typedef s32 TestStatus;
enum TestStatus {
    TEST_PASS         = 0xba5eba11,
    TEST_PASS_HANDLER = 0xba5eba12,
    TEST_FAIL         = 0xdeadbeef,
};

// Test thread stacks and heap
static MosThread StaticThreads[MAX_APP_THREADS];
static MosThread * Threads[MAX_APP_THREADS];
static u8 * Stacks[MAX_APP_THREADS];

static MosHeap TestThreadHeapDesc;
static u8 MOS_STACK_ALIGNED TestThreadHeap[8192];

// Heap for Heap testing
static u8 MOS_STACK_ALIGNED TestHeap[16384];

// Generic flag for tests
static volatile u32 TestFlag = 0;

// Generic histogram for tests
#define MAX_TEST_HISTO_ENTRIES   16
static volatile u32 TestHisto[MAX_TEST_HISTO_ENTRIES];

#define MAX_TICK_HISTO_ENTRIES  101
static volatile u32 TickHisto[MAX_TICK_HISTO_ENTRIES];

static volatile u32 SchedCount;

// Test Sem / Mutex / Mux
static MosSem TestSem;
static MosMutex TestMutex;
#if 0
static MosMux TestMux;
#endif

// Test Message Queue
static u32 queue[4];
static MosQueue TestQueue;

static void ClearHistogram(void) {
    for (u32 ix = 0; ix < count_of(TestHisto); ix++)
        TestHisto[ix] = 0;
}

static void DisplayHistogram(u32 cnt) {
    for (u32 ix = 0; ix < cnt; ix++)
        MosPrintf(" Histo[%u] = %u\n", ix, TestHisto[ix]);
}

void EXTI0_IRQHandler(void) {
    MosGiveSem(&TestSem);
    TestHisto[0]++;
}

void EXTI1_IRQHandler(void) {
    if (MosTrySendToQueue(&TestQueue, 1)) TestHisto[0]++;
}

void EventCallback(MosEvent evt, u32 val) {
    static u32 last_tick = 0;
    if (evt == MOS_EVENT_TICK) {
        u32 diff = (val - last_tick);
        if (diff > MAX_TICK_HISTO_ENTRIES - 1)
            TickHisto[MAX_TICK_HISTO_ENTRIES - 1]++;
        else
            TickHisto[diff]++;
        last_tick = val;
    } else if (evt == MOS_EVENT_SCHEDULER_EXIT) {
        SchedCount++;
    }
}

//
// Thread tests
//

static const u32 pri_test_delay = 50;

static s32 PriTestThread(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        TestHisto[arg]++;
        // NOTE: Non-blocking delay
        MosDelayMicroSec(pri_test_delay * 1000);
    }
    return TEST_PASS;
}

static s32 KillTestHandler(s32 arg) {
    MosPrint("KillTestHandler: Running Handler\n");
    if (MosIsMutexOwner(&TestMutex)) {
        MosPrint("KillTestHandler: I own mutex\n");
        MosRestoreMutex(&TestMutex);
    }
    return arg;
}

static s32 KillTestThread(s32 arg) {
    if (arg) {
        MosSetStopHandler(MosGetThread(), KillTestHandler, TEST_PASS_HANDLER);
        // Take mutex a couple times... need to release it in handler
        MosTakeMutex(&TestMutex);
        MosTakeMutex(&TestMutex);
    } else MosSetStopArg(MosGetThread(), TEST_PASS_HANDLER);
    MosLogTrace(TRACE_INFO, "KillTestThread: Blocking\n");
    MosTakeSem(&TestSem);
    return TEST_FAIL;
}

static s32 KillSelfTestThread(s32 arg) {
    if (arg) {
        MosSetStopHandler(MosGetThread(), KillTestHandler, TEST_PASS_HANDLER);
        // Take mutex a couple times... need to release it in handler
        MosTakeMutex(&TestMutex);
        MosTakeMutex(&TestMutex);
    } else MosSetStopArg(MosGetThread(), TEST_PASS_HANDLER);
    MosLogTrace(TRACE_INFO, "KillSelfTestThread: Killing Self\n");
    MosKillThread(MosGetThread());
    return TEST_FAIL;
}

static s32 ExcTestThread(s32 arg) {
    MosSetStopArg(MosGetThread(), TEST_PASS_HANDLER + 1);
    MosDelayThread(50);
    MosCrash();
    return TEST_FAIL;
}

static s32 AssertTestThread(s32 arg) {
    MosSetStopArg(MosGetThread(), TEST_PASS_HANDLER);
    MosAssert(arg == 0x1234);
    return TEST_FAIL;
}

static s32 FPTestThread(s32 arg) {
    float x = 0.0;
    for (;;) {
        TestHisto[arg]++;
        x = x + 1.0;
        if (arg > 1 && (TestHisto[arg] == 1000)) {
            // Create an integer div-by-0 exception in FP thread
            MosSetStopArg(MosGetThread(), TEST_PASS_HANDLER + 1);
            volatile u32 y = (20 / (arg - 2));
            (void)y;
            return TEST_FAIL;
        }
        if (MosIsStopRequested()) break;
    }
    if ((float)TestHisto[arg] != x) return TEST_FAIL;
    else return TEST_PASS;
}

static bool ThreadTests(void) {
    const u32 test_time = 5000;
    u32 exp_iter = test_time / pri_test_delay;
    bool tests_all_pass = true;
    bool test_pass;
#if 1
    //
    // Highest priorities must starve lowest
    //
    test_pass = true;
    MosPrint("Priority Test 1\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, PriTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[1] != 0) test_pass = false;
    if (TestHisto[2] != 0) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Change of priority
    //
    test_pass = true;
    MosPrint("Priority Test 2\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, PriTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosChangeThreadPriority(Threads[1], 2);
    MosChangeThreadPriority(Threads[2], 1);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[1] < exp_iter || TestHisto[1] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[2] != 0) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Timeout on wait for thread
    //
    test_pass = true;
    MosPrint("Wait For Thread Stop with Timeout\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    s32 rtn_val;
    if (MosWaitForThreadStopOrTO(Threads[1], &rtn_val, test_time) != false) test_pass = false;
    MosRequestThreadStop(Threads[1]);
    if (MosWaitForThreadStopOrTO(Threads[1], &rtn_val, test_time) != true) test_pass = false;
    if (rtn_val != TEST_PASS) test_pass = false;
    DisplayHistogram(1);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // InitThread / RunThread
    //

    //
    // Set and Restore errno
    //
#endif
    //
    // Dynamic threads
    //
    test_pass = true;
    MosPrint("Dynamic Threads\n");
    ClearHistogram();
    MosThread * thd[2] = {0};
    MosAllocAndRunThread(&thd[0], 1, PriTestThread, 0, DFT_STACK_SIZE);
    MosAllocAndRunThread(&thd[1], 1, PriTestThread, 1, DFT_STACK_SIZE);
    if (thd[0] && thd[1]) {
        MosDelayThread(2 * test_time);
        MosRequestThreadStop(thd[0]);
        MosRequestThreadStop(thd[1]);
        if (MosWaitForThreadStop(thd[0]) != TEST_PASS) test_pass = false;
        if (MosWaitForThreadStop(thd[1]) != TEST_PASS) test_pass = false;
        MosDecThreadRefCount(&thd[0]);
        MosDecThreadRefCount(&thd[1]);
        DisplayHistogram(3);
        if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
            test_pass = false;
        if (TestHisto[1] < exp_iter || TestHisto[1] > exp_iter + 1)
            test_pass = false;
    }
    else {
        MosPrint("Cannot create threads!\n");
        test_pass = false;
    }
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
#if 1
    //
    // Kill Thread using Default Handler
    //
    test_pass = true;
    MosPrint("Kill Test 1\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(Threads[1], 1, KillTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosDelayThread(10);
    MosKillThread(Threads[1]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Kill Thread using Supplied Handler
    //
    test_pass = true;
    MosPrint("Kill Test 2\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(Threads[1], 1, KillTestThread, 1, Stacks[1], DFT_STACK_SIZE);
    MosDelayThread(10);
    MosKillThread(Threads[1]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    if (TestMutex.owner != NULL) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Thread killing self
    //
    test_pass = true;
    MosPrint("Kill Test 3\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(Threads[1], 1, KillSelfTestThread, 1, Stacks[1], DFT_STACK_SIZE);
    MosDelayThread(10);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    if (TestMutex.owner != NULL) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
#if 1
    //
    // Thread exception handler
    //
    test_pass = true;
    MosPrint("Exception Test\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, ExcTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER + 1) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
#endif
    //
    // Assertion test
    //
    test_pass = true;
    MosPrint("Assertion Test\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, AssertTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    MosInitAndRunThread(Threads[1], 1, AssertTestThread, 0x1234, Stacks[1], DFT_STACK_SIZE);
    if (MosWaitForThreadStop(Threads[1]) != TEST_FAIL) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try some floating point
    //
    if (MosGetParams()->fp_support_en == true) {
        test_pass = true;
        MosPrint("FP Test\n");
        ClearHistogram();
        MosInitAndRunThread(Threads[1], 1, FPTestThread, 0, Stacks[1], DFT_STACK_SIZE);
        MosInitAndRunThread(Threads[2], 1, FPTestThread, 1, Stacks[2], DFT_STACK_SIZE);
        MosInitAndRunThread(Threads[3], 1, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
        MosDelayThread(test_time / 2);
        MosRequestThreadStop(Threads[1]);
        MosRequestThreadStop(Threads[2]);
        MosRequestThreadStop(Threads[3]);
        if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
        if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
        if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
        DisplayHistogram(3);
        if (test_pass) MosPrint(" Passed\n");
        else {
            MosPrint(" Failed\n");
            tests_all_pass = false;
        }
#if 1
        test_pass = true;
        MosPrint("Exception in FP thread\n");
        ClearHistogram();
        MosInitAndRunThread(Threads[1], 1, FPTestThread, 2, Stacks[1], DFT_STACK_SIZE);
        MosSetThreadName(Threads[1], "fp_thread");
        if (MosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER + 1) test_pass = false;
        if (test_pass) MosPrint(" Passed\n");
        else {
            MosPrint(" Failed\n");
            tests_all_pass = false;
        }
#endif
    }
#endif
    return tests_all_pass;
}

// Make delay a multiple of 4
static const u32 timer_test_delay = 100;

static s32 ThreadTimerTestThread(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestThread2(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay / 2);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestThread4(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay / 4);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestThreadOdd(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(arg & 0xFFFF);
        TestHisto[arg >> 16]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestBusyThread(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 MessageTimerTestThread(s32 arg) {
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosTimer self_timer;
    MosInitTimer(&self_timer, &TestQueue);
    u32 cnt = 0xdeadbeef;
    for (;;) {
        if (MosIsStopRequested()) break;
        MosSetTimer(&self_timer, timer_test_delay, cnt);
        u32 val = MosReceiveFromQueue(&TestQueue);
        if (val != cnt) return TEST_FAIL;
        cnt++;
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

//
// Timer Tests
//
static bool TimerTests(void) {
    const u32 test_time = 5000;
    u32 exp_iter = test_time / timer_test_delay;
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Run uniform timers
    //
    test_pass = true;
    MosPrint("Thread Timer Test 1\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, ThreadTimerTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, ThreadTimerTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter) test_pass = false;
    if (TestHisto[2] != exp_iter) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run "harmonic" timers
    //
    test_pass = true;
    MosPrint("Thread Timer Test 2\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, ThreadTimerTestThread2, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, ThreadTimerTestThread4, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter * 2) test_pass = false;
    if (TestHisto[2] != exp_iter * 4) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run odd timers
    //
    test_pass = true;
    MosPrint("Thread Timer Test 3\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, ThreadTimerTestThreadOdd, 13, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, ThreadTimerTestThreadOdd, 33 | 0x10000, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, ThreadTimerTestThreadOdd, 37 | 0x20000, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != (test_time / 13) + 1) test_pass = false;
    if (TestHisto[1] != (test_time / 33) + 1) test_pass = false;
    if (TestHisto[2] != (test_time / 37) + 1) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run two timers over busy thread
    //
    test_pass = true;
    MosPrint("Thread Timer Test 4\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 1, ThreadTimerTestThread2, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 2, ThreadTimerTestBusyThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter * 2) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run timer over two busy threads
    //
    test_pass = true;
    MosPrint("Thread Timer Test 5\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, ThreadTimerTestBusyThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 2, ThreadTimerTestBusyThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run message timers
    //
    test_pass = true;
    MosPrint("Message Timer Test 1\n");
    ClearHistogram();
    MosInitAndRunThread(Threads[1], 1, MessageTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    DisplayHistogram(1);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

//
// Semaphore Testing
//

static const u32 sem_test_delay = 50;

static s32 SemTestPendIRQ(s32 arg) {
    for (;;) {
        // Fire Software Interrupt
        NVIC_SetPendingIRQ(EXTI0_IRQn);
        TestHisto[arg]++;
        MosDelayThread(sem_test_delay);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadTx(s32 arg) {
    for (;;) {
        MosGiveSem(&TestSem);
        TestHisto[arg]++;
        MosDelayThread(sem_test_delay);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadTxFast(s32 arg) {
    for (;;) {
        MosGiveSem(&TestSem);
        MosDelayMicroSec(10);
        TestHisto[arg]++;
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadRx(s32 arg) {
    for (;;) {
        MosTakeSem(&TestSem);
        TestHisto[arg]++;
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadRxTimeout(s32 arg) {
    for (;;) {
        if (MosTakeSemOrTO(&TestSem, sem_test_delay / 2 + 2)) {
            TestHisto[arg]++;
        } else {
            TestHisto[arg + 1]++;
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadRxTry(s32 arg) {
    for (;;) {
        if (MosTrySem(&TestSem)) {
            TestHisto[arg]++;
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestThreadTx(s32 arg) {
    for (;;) {
        //if (arg) MosDelayThread(arg);
        MosRaiseSignal(&TestSem, 1 << arg);
        TestHisto[arg]++;
        MosDelayThread(sem_test_delay);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestThreadRx(s32 arg) {
    for (;;) {
        u32 flags = MosWaitForSignal(&TestSem);
        MosAssert(flags > 0);
        MosAssert(flags <= 3);
        if (flags & 0x1) TestHisto[arg]++;
        if (flags & 0x2) TestHisto[arg + 1]++;
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestThreadRxTimeout(s32 arg) {
    for (;;) {
        u32 flags = MosWaitForSignalOrTO(&TestSem, 100);
        if (flags) {
            MosAssert(flags <= 3);
            if (flags & 0x1) TestHisto[arg]++;
            if (flags & 0x2) TestHisto[arg + 1]++;
        } else MosAssert(0);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestPoll(s32 arg) {
    for (;;) {
        u32 flags = MosPollForSignal(&TestSem);
        if (flags) {
            if (flags & 0x1) TestHisto[arg]++;
            if (flags & 0x2) TestHisto[arg + 1]++;
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static bool SemTests(void) {
    const u32 test_time = 5000;
    u32 exp_cnt = test_time / sem_test_delay;
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Validate counts
    //
    test_pass = true;
    MosPrint("Sem Test 1\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(Threads[1], 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, SemTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosGiveSem(&TestSem);  // Unblock thread to stop
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5 + 1)
        test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Post from interrupt and threads
    //
    test_pass = true;
    MosPrint("Sem Test 2\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(Threads[1], 1, SemTestPendIRQ, 1, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, SemTestThreadRx, 3, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosGiveSem(&TestSem); // Unblock thread to stop
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[3] != TestHisto[0] + TestHisto[2] + 1)
        test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Take Sem with Timeouts
    //
    test_pass = true;
    MosPrint("Sem Test 3\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(Threads[1], 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, SemTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5)
        test_pass = false;
    // Add one for last timeout before thread stop
    if (TestHisto[3] != exp_cnt + 1) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    //  Take lots of semaphores
    //
    test_pass = true;
    MosPrint("Sem Test 4\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(Threads[1], 2, SemTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, SemTestThreadTxFast, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 2, SemTestThreadTxFast, 1, Stacks[2], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosDelayThread(5);
    MosRequestThreadStop(Threads[1]);
    MosGiveSem(&TestSem);  // Unblock thread to stop
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5 + 1)
        test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // TrySem
    //
    test_pass = true;
    MosPrint("Try Sem\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(Threads[1], 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, SemTestThreadRxTry, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5)
        test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Signals
    //
    test_pass = true;
    MosPrint("Signals\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(Threads[1], 1, SignalTestThreadRx, 2, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, SignalTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 2, SignalTestThreadTx, 0, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosRaiseSignal(&TestSem, 2);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1] + 1) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Signals with Timeout
    //   TODO: needs some work -- since timeout isn't tested yet
    //
    test_pass = true;
    MosPrint("Signals With Timeout\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(Threads[1], 1, SignalTestThreadRxTimeout, 2, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, SignalTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 2, SignalTestThreadTx, 0, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosRaiseSignal(&TestSem, 2);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1] + 1) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Poll Signals
    //
    test_pass = true;
    MosPrint("Signal Polling\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(Threads[1], 1, SignalTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, SignalTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, SignalTestPoll, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1]) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

//
// Queue Testing
//

static const u32 queue_test_delay = 50;

static s32 QueueTestPendIRQ(s32 arg) {
    for (;;) {
        // Fire Software Interrupt
        NVIC_SetPendingIRQ(EXTI1_IRQn);
        MosDelayThread(queue_test_delay);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadTx(s32 arg) {
    for (;;) {
        MosSendToQueue(&TestQueue, arg);
        TestHisto[arg]++;
        MosDelayThread(queue_test_delay);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadTxTimeout(s32 arg) {
    for (;;) {
        if (MosSendToQueueOrTO(&TestQueue, 2,
                               queue_test_delay / 2 + 2)) {
            TestHisto[arg]++;
        } else {
            TestHisto[arg + 1]++;
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRx(s32 arg) {
    for (;;) {
        u32 val = MosReceiveFromQueue(&TestQueue);
        TestHisto[arg + val]++;
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRxTry(s32 arg) {
    for (;;) {
        u32 val;
        if (MosTryReceiveFromQueue(&TestQueue, &val)) {
            TestHisto[arg + val]++;
            if (MosIsStopRequested()) break;
        }
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRxSlow(s32 arg) {
    for (;;) {
        u32 val = MosReceiveFromQueue(&TestQueue);
        TestHisto[arg + val]++;
        if (MosIsStopRequested()) break;
        MosDelayThread(queue_test_delay);
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRxTimeout(s32 arg) {
    for (;;) {
        u32 val;
        if (MosReceiveFromQueueOrTO(&TestQueue, &val,
                                    queue_test_delay / 2 + 2)) {
            TestHisto[arg + val]++;
        } else {
            TestHisto[arg + 3]++;
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static bool QueueTests(void) {
    const u32 test_time = 5000;
    u32 exp_cnt = test_time / queue_test_delay;
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Post from interrupts and threads
    //
    test_pass = true;
    MosPrint("Queue Test 1\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, QueueTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosSendToQueue(&TestQueue, 2); // Unblock thread to stop
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2] + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Receive From Queue Timeout
    //
    test_pass = true;
    MosPrint("Queue Test 2\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, QueueTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(6);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2]) test_pass = false;
    if (TestHisto[5] != exp_cnt + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Send to Queue Timeout
    //   NOTE: The interrupt will only be able to queue the first entry since the
    //   thread will hog the queue.
    //
    test_pass = true;
    MosPrint("Queue Test 3\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, QueueTestThreadTxTimeout, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, QueueTestThreadRxSlow, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    // Give Thread 3 extra time to drain the queue
    MosDelayThread(queue_test_delay * (count_of(queue) + 1));
    MosRequestThreadStop(Threads[3]);
    MosSendToQueue(&TestQueue, 2);
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[2] != exp_cnt) test_pass = false;
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[1] + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Queue
    //
    test_pass = true;
    MosPrint("Queue Test 4\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, QueueTestThreadRxTry, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosSendToQueue(&TestQueue, 2); // Unblock thread to stop
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2] + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

#if 0

//
// Mux Testing
//

static const u32 mux_test_delay = 50;

static s32 MuxTestThreadTx(s32 arg) {
    for (;;) {
        if (arg == 0) MosGiveSem(&TestSem);
        else if (arg == 1) MosSendToQueue(&TestQueue, 1);
        TestHisto[arg]++;
        MosDelayThread(mux_test_delay);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 MuxTestThreadRx(s32 arg) {
    MosMuxEntry mux[2];
    mux[0].type = MOS_WAIT_SEM;
    mux[0].ptr.sem = &TestSem;
    mux[1].type = MOS_WAIT_RECV_QUEUE;
    mux[1].ptr.q = &TestQueue;
    MosInitMux(&TestMux);
    MosSetActiveMux(&TestMux, mux, count_of(mux));
    for (;;) {
        u32 idx = MosWaitOnMux(&TestMux);
        if (idx == 0) {
            if (!MosTrySem(&TestSem)) return TEST_FAIL;
            TestHisto[arg]++;
        } else if (idx == 1) {
            u32 val;
            if (!MosTryReceiveFromQueue(&TestQueue, &val))
                return TEST_FAIL;
            TestHisto[arg + val]++;
        } else return TEST_FAIL;
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 MuxTestThreadRxTimeout(s32 arg) {
    MosMuxEntry mux[2];
    mux[0].type = MOS_WAIT_SEM;
    mux[0].ptr.sem = &TestSem;
    mux[1].type = MOS_WAIT_RECV_QUEUE;
    mux[1].ptr.q = &TestQueue;
    MosInitMux(&TestMux);
    MosSetActiveMux(&TestMux, mux, count_of(mux));
    for (;;) {
        u32 idx;
        if (MosWaitOnMuxOrTO(&TestMux, &idx, mux_test_delay / 2 + 2)) {
            if (idx == 0) {
                if (!MosTrySem(&TestSem)) return TEST_FAIL;
                TestHisto[arg]++;
            } else if (idx == 1) {
                u32 val;
                if (!MosTryReceiveFromQueue(&TestQueue, &val))
                    return TEST_FAIL;
                TestHisto[arg + val]++;
            } else return TEST_FAIL;
        } else {
            TestHisto[4]++;
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static bool MuxTests(void) {
    const u32 test_time = 5000;
    u32 exp_cnt = test_time / mux_test_delay;
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Wait on Mux, thread and semaphore
    //
    test_pass = true;
    MosPrint("Mux Test 1\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(Threads[1], 1, MuxTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, MuxTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, MuxTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    MosSendToQueue(&TestQueue, 2); // Unblock thread to stop
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1]) test_pass = false;
    if (TestHisto[4] != 1) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Wait on Mux with Timeout
    //
    test_pass = true;
    MosPrint("Mux Test 2\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(Threads[1], 1, MuxTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, MuxTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, MuxTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1]) test_pass = false;
    if (TestHisto[4] != exp_cnt + 1) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

#endif

//
// Mutex Tests
//

static s32 MutexRecursion(u32 depth) {
    const u32 max_test_depth = 4;
    TestStatus status = TEST_PASS;
    MosTakeMutex(&TestMutex);
    if (TestMutex.depth != depth) {
        status = TEST_FAIL;
    } else if (depth < max_test_depth) {
        if (MutexRecursion(depth + 1) == TEST_FAIL)
            status = TEST_FAIL;
    }
    MosGiveMutex(&TestMutex);
    return status;
}

#define MUTEX_TEST_PRIO_INHER     5

static s32 MutexTestThread(s32 arg) {
    TestStatus status = TEST_PASS;
    for (;;) {
        if (arg == MUTEX_TEST_PRIO_INHER) {
            static u32 count = 0;
            if ((count++ & 0xFFF) == 0) {
                // Give low priority thread chance to acquire mutex
                MosTrySendToQueue(&TestQueue, 0);
                MosDelayThread(5);
            }
        }
        MosTakeMutex(&TestMutex);
        if (TestFlag == 1) {
            status = TEST_FAIL;
            goto EXIT_MTT;
        }
        TestFlag = 1;
        if (MosIsStopRequested())
            goto EXIT_MTT;
        if (MutexRecursion(2) == TEST_FAIL) {
            status = TEST_FAIL;
            goto EXIT_MTT;
        }
        TestHisto[arg]++;
        TestFlag = 0;
        MosGiveMutex(&TestMutex);
    }
EXIT_MTT:
    TestFlag = 0;
    MosGiveMutex(&TestMutex);
    return status;
}

static s32 MutexTryTestThread(s32 arg) {
    TestStatus status = TEST_PASS;
    for (;;) {
        if (MosTryMutex(&TestMutex)) {
            if (TestFlag == 1) {
                status = TEST_FAIL;
                goto EXIT_MTT;
            }
            TestFlag = 1;
            if (MosIsStopRequested())
                goto EXIT_MTT;
            if (MutexRecursion(2) == TEST_FAIL) {
                status = TEST_FAIL;
                goto EXIT_MTT;
            }
            TestHisto[arg]++;
            TestFlag = 0;
            MosGiveMutex(&TestMutex);
        }
    }
EXIT_MTT:
    TestFlag = 0;
    MosGiveMutex(&TestMutex);
    return status;
}

// Dummy thread is used for priority inheritance tests and
//   runs at the middle priority.
static s32 MutexDummyThread(s32 arg) {
    for (;;) {
        u32 dummy;
        if (MosTryReceiveFromQueue(&TestQueue, &dummy)) {
            // Give low priority thread chance to acquire mutex
            MosDelayThread(2);
        }
        TestHisto[arg]++;
        if (MosIsStopRequested())
            break;
    }
    return TEST_PASS;
}

static bool MutexTests(void) {
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Contention / Depth 1
    //
    test_pass = true;
    MosPrint("Mutex Test 1\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(Threads[1], 3, MutexTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, MutexTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Contention / Depth 2
    //
    test_pass = true;
    MosPrint("Mutex Test 2\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(Threads[1], 3, MutexTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 3, MutexTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, MutexTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Priority Inheritance (1 level)
    //
    test_pass = true;
    MosPrint("Mutex Test 3\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(Threads[1], 1, MutexTestThread, MUTEX_TEST_PRIO_INHER,
                        Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, MutexDummyThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 3, MutexTestThread, 0, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    DisplayHistogram(6);
    // It's possible scheduler wakes threads when lowest priority one doesn't hold mutex
    if (TestHisto[MUTEX_TEST_PRIO_INHER] <= 4096) test_pass = false;
    // Make sure thread priorities are restored
    if (MosGetThreadPriority(Threads[1]) != 1) test_pass = false;
    if (MosGetThreadPriority(Threads[2]) != 2) test_pass = false;
    if (MosGetThreadPriority(Threads[3]) != 3) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Mutex (NOTE: may exhibit some non-deterministic behavior)
    //
    test_pass = true;
    MosPrint("Mutex Test 4\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(Threads[1], 2, MutexTryTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, MutexTryTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Mutex 2 (NOTE: may exhibit some non-deterministic behavior)
    //
    test_pass = true;
    MosPrint("Mutex Test 5\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(Threads[1], 2, MutexTryTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[2], 2, MutexTryTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(Threads[3], 2, MutexTryTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(Threads[1]);
    MosRequestThreadStop(Threads[2]);
    MosRequestThreadStop(Threads[3]);
    if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

static bool HeapTests(void) {
    bool tests_all_pass = true;
    bool test_pass;
    MosHeap TestHeapDesc;
    //u32 rem;

#if 0
    //
    // Allocate and Free of blocks with reserved block sizes
    //
    test_pass = true;
    MosPrint("Heap Test 1: Reserved block sizes\n");
    MosInitHeap(&TestHeapDesc, TestHeap, sizeof(TestHeap), 3);
    if (TestHeapDesc.max_bs != 0) test_pass = false;
    MosReserveBlockSize(&TestHeapDesc, 128);
    if (TestHeapDesc.max_bs != 128) test_pass = false;
    MosReserveBlockSize(&TestHeapDesc, 16);
    if (TestHeapDesc.max_bs != 128) test_pass = false;
    MosReserveBlockSize(&TestHeapDesc, 256);
    if (TestHeapDesc.max_bs != 256) test_pass = false;
    {
        u32 * blocks[10];
        // Start off with a double-free for good measure
        blocks[0] = MosAlloc(&TestHeapDesc, 4);
        MosFree(&TestHeapDesc, blocks[0]);
        MosFree(&TestHeapDesc, blocks[0]);
        for (u32 ix = 0; ix < count_of(blocks); ix++) {
            blocks[ix] = MosAlloc(&TestHeapDesc, 4);
            if (blocks[ix] == NULL) test_pass = false;
            if ((u32)blocks[ix] % MOS_HEAP_ALIGNMENT != 0) test_pass = false;
            *(blocks[ix]) = ix + 1;
        }
        for (u32 ix = 0; ix < count_of(blocks); ix++) {
            MosFree(&TestHeapDesc, blocks[ix]);
        }
        for (u32 ix = 0; ix < count_of(blocks); ix++) {
            blocks[ix] = MosAlloc(&TestHeapDesc, 4);
            if (blocks[ix] == NULL) test_pass = false;
            if ((u32)blocks[ix] % MOS_HEAP_ALIGNMENT != 0) test_pass = false;
            *(blocks[ix]) = ix;
        }
        for (u32 ix = 0; ix < count_of(blocks); ix++) {
            blocks[ix] = MosReAlloc(&TestHeapDesc, blocks[ix], 8);
            if (blocks[ix] == NULL) test_pass = false;
            if ((u32)blocks[ix] % MOS_HEAP_ALIGNMENT != 0) test_pass = false;
        }
        for (u32 ix = 0; ix < count_of(blocks); ix++) {
            if (*blocks[ix] != ix) test_pass = false;
        }
        for (u32 ix = 0; ix < count_of(blocks); ix++) {
            blocks[ix] = MosReAlloc(&TestHeapDesc, blocks[ix], 32);
            if (blocks[ix] == NULL) test_pass = false;
            if ((u32)blocks[ix] % MOS_HEAP_ALIGNMENT != 0) test_pass = false;
        }
        for (u32 ix = 0; ix < count_of(blocks); ix++) {
            if (*blocks[ix] != ix) test_pass = false;
        }
    }
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Allocate and Free of odd sized blocks
    //
    test_pass = true;
    MosPrint("Heap Test 2: Odd blocks\n");
    MosInitHeap(&TestHeapDesc, TestHeap, sizeof(TestHeap), 3);
    MosReserveBlockSize(&TestHeapDesc, 64);
    MosReserveBlockSize(&TestHeapDesc, 128);
    MosReserveBlockSize(&TestHeapDesc, 256);
    const u16 num_blocks = 5;
    void * blocks[num_blocks];
    rem = TestHeapDesc.bot - TestHeapDesc.pit + 16;
    MosPrintf("  Starting: %u bytes\n", rem);
    // Start off with a double-free for good measure
    blocks[0] = MosAlloc(&TestHeapDesc, 257);
    MosFree(&TestHeapDesc, blocks[0]);
    MosFree(&TestHeapDesc, blocks[0]);
    if (TestHeapDesc.max_bs != 256) test_pass = false;
    for (u32 ix = 0; ix < num_blocks; ix++) {
        blocks[ix] = MosAlloc(&TestHeapDesc, 257);
        if (blocks[ix] == NULL) test_pass = false;
        if ((u32)blocks[ix] % MOS_HEAP_ALIGNMENT != 0) test_pass = false;
        if (!MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    }
    rem = TestHeapDesc.bot - TestHeapDesc.pit + 16;
    MosPrintf(" Remaining: %u bytes\n", rem);
    if (rem != sizeof(TestHeap) - (264 + 16) * num_blocks - 3 * 24)
        test_pass = false;
    for (u32 ix = 0; ix < num_blocks; ix++) {
        MosFree(&TestHeapDesc, blocks[ix]);
        if (MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    }
    for (u32 ix = 0; ix < num_blocks; ix++) {
        if (MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
        blocks[ix] = MosAlloc(&TestHeapDesc, 257);
        if (blocks[ix] == NULL) test_pass = false;
        if ((u32)blocks[ix] % MOS_HEAP_ALIGNMENT != 0) test_pass = false;
    }
    if (!MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    rem = TestHeapDesc.bot - TestHeapDesc.pit + 16;
    MosPrintf(" Remaining: %u bytes\n", rem);
    if (rem != sizeof(TestHeap) - (264 + 16) * num_blocks - 3 * 24)
        test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Allocation of several odd size blocks of differing sizes
    //
    test_pass = true;
    MosPrint("Heap Test 3: Odd blocks Only - different sizes\n");
    MosInitHeap(&TestHeapDesc, TestHeap, sizeof(TestHeap), 0);
    if (MosAllocBlock(&TestHeapDesc, 16) != NULL) test_pass = false;
    for (u32 ix = 0; ix < num_blocks; ix++) {
        blocks[ix] = MosAlloc(&TestHeapDesc, 57 << ix);
        if (blocks[ix] == NULL) test_pass = false;
        if ((u32)blocks[ix] % MOS_HEAP_ALIGNMENT != 0) test_pass = false;
        if (!MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    }
    for (u32 ix = 0; ix < num_blocks; ix++) {
        // ReAlloc with 0 to free
        if (MosReAlloc(&TestHeapDesc, blocks[ix], 0) != NULL) test_pass = false;
        if (MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    }
    if (MosAllocBlock(&TestHeapDesc, 32) != NULL) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Allocate and Free of short-lived blocks
    //    NOTE:  Uses TestThread Heap !
    //
    test_pass = true;
    MosPrint("Heap Test 4: Short-lived blocks\n");
    u8 * fun[8];
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MosAllocShortLived(&TestThreadHeapDesc, 64);
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        MosFree(&TestThreadHeapDesc, fun[ix]);
    }
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
#endif
    //
    // Reallocation
    //
    test_pass = true;
    MosPrint("Heap Test 5: Reallocation\n");
    u32 alignment = 8;
    MosInitHeap(&TestHeapDesc, TestHeap, sizeof(TestHeap), alignment);
    u8 * fun[8];
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MosAlloc(&TestHeapDesc, 400);
        if (fun[ix] == NULL) test_pass = false;
        else memset(fun[ix], ix, 400);
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MosReAlloc(&TestHeapDesc, fun[ix], 600);
        if (fun[ix] == NULL) test_pass = false;
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 400; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MosReAlloc(&TestHeapDesc, fun[ix], 400);
        if (fun[ix] == NULL) test_pass = false;
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 400; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MosReAlloc(&TestHeapDesc, fun[ix], 100);
        if (fun[ix] == NULL) test_pass = false;
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 100; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MosReAlloc(&TestHeapDesc, fun[ix], 128);
        if (fun[ix] == NULL) test_pass = false;
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 100; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u8 ix = 0; ix < count_of(fun); ix++) {
        MosFree(&TestHeapDesc, fun[ix]);
    }
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Exhaustion
    //
    test_pass = true;
    MosPrint("Heap Test 6: Exhaustion\n");
    {
        const u32 bs1 = 64;
        u32 ctr = 0;
        void * block, * last_block = NULL;
        while (1) {
             block = MosAlloc(&TestHeapDesc, bs1);
             if (block) last_block = block;
             else break;
             if (((u32)block & (alignment - 1)) != 0) test_pass = false;
             ctr++;
        } while (block);
        MosFree(&TestHeapDesc, last_block);
        if (MosAlloc(&TestHeapDesc, bs1) != last_block) test_pass = false;
        MosPrintf("Allocated up to %u blocks\n", ctr);
        if (ctr != sizeof(TestHeap) / (bs1 + 12 + 4)) test_pass = false;
    }
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

static s32 StackPrintThread(s32 arg) {
    u64 e = 0xdeadbeeffeebdaed;
    MosPrintf("DEADBEEFFEEBDAED == %llX\n", e);
    return TEST_PASS;
}

static bool MiscTests(void) {
    bool tests_all_pass = true;
    bool test_pass;
    //
    // 64-bit print test (stack alignment)
    //
    test_pass = true;
    MosPrint("Misc Test 1: Stack and 64-bit print alignment\n");
    for (u32 ix = 0; ix < 8; ix++) {
        MosInitAndRunThread(Threads[1], 3, StackPrintThread, 1, Stacks[2], DFT_STACK_SIZE + ix);
        if (MosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    }
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    MosPrint("Misc Test 2: MosSNPrintf()\n");
    char * dummy = "bummy_dummy_mummy_";
    char buf[128];
    if (MosSNPrintf(buf, 32, "%s%s%s", dummy, dummy, dummy) != 31) test_pass = false;
    if (MosSNPrintf(buf, sizeof(buf), "%s", dummy) != strlen(dummy)) test_pass = false;
    if (strcmp(buf, dummy)) test_pass = false;
    if (MosSNPrintf(buf, 11, "%010llx", 0xdeadbee90) != 10) test_pass = false;
    if (strcmp(buf, "0deadbee90")) test_pass = false;
    if (MosSNPrintf(buf, 8, "%c%%%d%%d%c%", '*', -1, '$') != 7) test_pass = false;
    if (strcmp(buf, "*%-1%d$")) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else {
        MosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

typedef enum {
    CMD_ERR_OUT_OF_RANGE = -3,
    CMD_ERR_NOT_FOUND = -2,
    CMD_ERR = -1,
    CMD_OK,
    CMD_OK_NO_HISTORY,
} CmdStatus;

static s32 CmdTest(s32 argc, char * argv[]) {
    bool test_pass = true;
    if (argc >= 2 && strcmp(argv[1], "hal") == 0) {
	    if (HalTests(argc - 2, argv + 2)) return CMD_OK;
        else return CMD_ERR;
    } else if (argc == 2) {
        if (strcmp(argv[1], "main") == 0) {
            if (ThreadTests() == false) test_pass = false;
            if (TimerTests() == false) test_pass = false;
            if (SemTests() == false) test_pass = false;
            if (QueueTests() == false) test_pass = false;
#if 0
            if (MuxTests() == false) test_pass = false;
#endif
            if (MutexTests() == false) test_pass = false;
            if (HeapTests() == false) test_pass = false;
            if (MiscTests() == false) test_pass = false;
        } else if (strcmp(argv[1], "thread") == 0) {
            test_pass = ThreadTests();
        } else if (strcmp(argv[1], "timer") == 0) {
            test_pass = TimerTests();
        } else if (strcmp(argv[1], "sem") == 0) {
            test_pass = SemTests();
        } else if (strcmp(argv[1], "queue") == 0) {
            test_pass = QueueTests();
#if 0
        } else if (strcmp(argv[1], "mux") == 0) {
            test_pass = MuxTests();
#endif
        } else if (strcmp(argv[1], "mutex") == 0) {
            test_pass = MutexTests();
        } else if (strcmp(argv[1], "heap") == 0) {
            test_pass = HeapTests();
        } else if (strcmp(argv[1], "misc") == 0) {
            test_pass = MiscTests();
        } else return CMD_ERR_NOT_FOUND;
        if (test_pass) {
            MosPrint("Tests Passed\n");
            return CMD_OK;
        } else {
            MosPrint("Tests FAILED\n");
            return CMD_ERR;
        }
    }
    return CMD_ERR_NOT_FOUND;
}

static volatile bool PigeonFlag = false;

static s32 PigeonThread(s32 arg) {
    u32 cnt = 0;
    while (1) {
        MosDelayThread(877);
        MosPrintf("Incoming ---- .. .. %u %08X.. ------\n", cnt,
                       MosGetStackDepth(MosGetStackBottom(NULL) + DFT_STACK_SIZE));
        cnt++;
    }
    return 0;
}

static s32 CmdPigeon(s32 argc, char * argv[]) {
    if (!PigeonFlag) {
        MosThread * thd = Threads[PIGEON_THREAD_ID];
        MosInitAndRunThread(thd, 0, PigeonThread, 0, MosGetStackBottom(thd),
                            MosGetStackSize(thd));
        PigeonFlag = 1;
    } else {
        // TODO: This could run into issue with mutex
        MosKillThread(Threads[PIGEON_THREAD_ID]);
        PigeonFlag = 0;
    }
    return CMD_OK;
}

static s32 CmdClearTickHisto(s32 argc, char * argv[]) {
    for (u32 ix = 0; ix < MAX_TICK_HISTO_ENTRIES; ix++) TickHisto[ix] = 0;
    SchedCount = 0;
    return CMD_OK;
}

static MosCmdList CmdList;

#define MAX_CMD_ARGUMENTS       10
#define MAX_CMD_BUFFER_LENGTH   10
#define MAX_CMD_LINE_SIZE       128

static char CmdBuffers[MAX_CMD_BUFFER_LENGTH][MAX_CMD_LINE_SIZE] = {{ 0 }};
static u32 CmdIx = 0;
static u32 CmdMaxIx = 0;
static u32 CmdHistoryIx = 0;

// Calculate a valid command index at the offset from the provided index
static u32 CalcOffsetCmdIx(s32 ix, s32 max_ix, s32 offset) {
    s32 new_ix = (ix + offset) % (max_ix + 1);
    if (new_ix < 0) new_ix += (max_ix + 1);
    return (u32)new_ix;
}

static CmdStatus RunCmd(char * cmd_buf_in) {
    u32 argc;
    char * argv[MAX_CMD_ARGUMENTS];
    char cmd_buf[MAX_CMD_LINE_SIZE];
    strncpy(cmd_buf, cmd_buf_in, sizeof(cmd_buf));
    argc = MosParseCmd(argv, cmd_buf, MAX_CMD_ARGUMENTS);
    if (argc == 0) return CMD_OK_NO_HISTORY;
    MosCmd * cmd = MosFindCmd(&CmdList, argv[0]);
    if (cmd) {
        return (CmdStatus)cmd->func(argc, argv);
    } else if (argv[0][0] == '!') {
        if (argv[0][1] == '!') {
            if (CmdMaxIx > 0) {
                u32 run_cmd_ix = CalcOffsetCmdIx(CmdIx, CmdMaxIx, -1);
                strcpy(CmdBuffers[CmdIx], CmdBuffers[run_cmd_ix]);
                return (CmdStatus)RunCmd(CmdBuffers[CmdIx]);
            } else return CMD_ERR_OUT_OF_RANGE;
        } else if (argv[0][1] == '-') {
            if (argv[0][2] >= '1' && argv[0][2] <= '9') {
                s8 offset = argv[0][2] - '0';
                if (offset <= CmdMaxIx) {
                    u32 run_cmd_ix = CalcOffsetCmdIx(CmdIx, CmdMaxIx, -offset);
                    strcpy(CmdBuffers[CmdIx], CmdBuffers[run_cmd_ix]);
                    return (CmdStatus)RunCmd(CmdBuffers[CmdIx]);
                } else return CMD_ERR_OUT_OF_RANGE;
            }
        }
    } else if (strcmp(argv[0], "?") == 0 || strcmp(argv[0], "help") == 0) {
        MosPrintCmdHelp(&CmdList);
        MosPrint("!!: Repeat prior command\n");
        MosPrint("!-#: Repeat #th prior command\n");
        MosPrint("h -or- history: Display command history\n");
        MosPrint("? -or- help: Display command help\n");\
        return CMD_OK_NO_HISTORY;
    } else if (strcmp(argv[0], "h") == 0 || strcmp(argv[0], "history") == 0) {
        for (s32 ix = CmdMaxIx; ix > 0; ix--) {
            u32 hist_cmd_ix = CalcOffsetCmdIx(CmdIx, CmdMaxIx, -ix);
            MosTakeTraceMutex();
            MosPrintf("%2d: ", -ix);
            MosPrint(CmdBuffers[hist_cmd_ix]);
            MosPrint("\n");
            MosGiveTraceMutex();
        }
        return CMD_OK_NO_HISTORY;
    } else if (argv[0][0] == '\0') {
        return CMD_OK_NO_HISTORY;
    }
    return CMD_ERR_NOT_FOUND;
}

static s32 TestShell(s32 arg) {

    static MosCmd list_cmds[] = {
        { CmdTest,           "run", "Run Test", "[TEST]", {0} },
        { CmdPigeon,         "p",   "Toggle Pigeon Printing", "", {0} },
        { CmdClearTickHisto, "cth", "Clear tick histogram", "", {0} },
    };

    MosInitShell();
    MosInitCmdList(&CmdList);
    MosAddCmd(&CmdList, &list_cmds[0]);
    MosAddCmd(&CmdList, &list_cmds[1]);
    MosAddCmd(&CmdList, &list_cmds[2]);

    while (1) {
        MosCmdResult result;
        CmdStatus status;
        result = MosGetNextCmd("# ", CmdBuffers[CmdIx], MAX_CMD_LINE_SIZE);
        switch (result) {
        case MOS_CMD_RECEIVED:
            status = RunCmd(CmdBuffers[CmdIx]);
            switch (status) {
            case CMD_OK_NO_HISTORY:
                break;
            case CMD_ERR_NOT_FOUND:
                MosPrint("[ERR] Command not found...\n");
                break;
            case CMD_ERR_OUT_OF_RANGE:
                MosPrint("[ERR] Index out of range...\n");
                break;
            case CMD_OK:
                MosPrint("[OK]\n");
                if (++CmdIx == MAX_CMD_BUFFER_LENGTH) CmdIx = 0;
                if (CmdIx > CmdMaxIx) CmdMaxIx = CmdIx;
                break;
            default:
            case CMD_ERR:
                MosPrint("[ERR]\n");
                if (++CmdIx == MAX_CMD_BUFFER_LENGTH) CmdIx = 0;
                if (CmdIx > CmdMaxIx) CmdMaxIx = CmdIx;
                break;
            }
            CmdHistoryIx = CmdIx;
            CmdBuffers[CmdIx][0] = '\0';
            break;
        case MOS_CMD_UP_ARROW:
            // Rotate history back one, skipping over current index
            CmdHistoryIx = CalcOffsetCmdIx(CmdHistoryIx, CmdMaxIx, -1);
            if (CmdHistoryIx == CmdIx)
                CmdHistoryIx = CalcOffsetCmdIx(CmdHistoryIx, CmdMaxIx, -1);
            strcpy(CmdBuffers[CmdIx], CmdBuffers[CmdHistoryIx]);
            break;
        case MOS_CMD_DOWN_ARROW:
            // Rotate history forward one, skipping over current index
            CmdHistoryIx = CalcOffsetCmdIx(CmdHistoryIx, CmdMaxIx, 1);
            if (CmdHistoryIx == CmdIx)
                CmdHistoryIx = CalcOffsetCmdIx(CmdHistoryIx, CmdMaxIx, 1);
            strcpy(CmdBuffers[CmdIx], CmdBuffers[CmdHistoryIx]);
            break;
        default:
            break;
        }
    }
    return 0;
}

int InitTestBench() {
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);

    MosRegisterEventHook(EventCallback);

    MosInitHeap(&TestThreadHeapDesc, TestThreadHeap, sizeof(TestThreadHeap), MOS_STACK_ALIGNMENT);
#if 0
    MosReserveBlockSize(&TestThreadHeapDesc, 1024);
    MosReserveBlockSize(&TestThreadHeapDesc, 512);
    MosReserveBlockSize(&TestThreadHeapDesc, 256);
    MosReserveBlockSize(&TestThreadHeapDesc, 128);
    MosReserveBlockSize(&TestThreadHeapDesc, 64);
#endif

    MosInitThreadHeap(&TestThreadHeapDesc);

    if (!MosAllocAndRunThread(&Threads[TEST_SHELL_THREAD_ID], 0, TestShell,
                              0, TEST_SHELL_STACK_SIZE)) {
        MosPrint("Thread allocation error\n");
        return -1;
    }

    if (!MosAllocThread(&Threads[PIGEON_THREAD_ID], DFT_STACK_SIZE)) {
        MosPrint("Thread allocation error\n");
        return -1;
    }

    // Static threads with stacks allocated from the heap
    for (u32 id = 1; id < (MAX_APP_THREADS - 1); id++) {
        Threads[id] = &StaticThreads[id];
        Stacks[id] = MosAlloc(&TestThreadHeapDesc, DFT_STACK_SIZE);
        if (Stacks[id] == NULL) {
            MosPrint("Stack allocation error\n");
            return -1;
        }
    }

    return 0;
}
