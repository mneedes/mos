
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Test Bench
//

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <mos/kernel.h>
#include <mos/queue.h>

#include <mos/format_string.h>
#include <mos/trace.h>
#include <mos/shell.h>
#include <mos/security.h>

#include <mos/experimental/slab.h>
#include <mos/experimental/registry.h>

#include <bsp_hal.h>
#include <hal_tb.h>

#include "tb.h"

#define DFT_STACK_SIZE           512
#define TEST_SHELL_STACK_SIZE    2048

#define MAX_APP_THREADS          6
#define TEST_SHELL_THREAD_ID     0
#define PIGEON_THREAD_ID         (MAX_APP_THREADS - 1)

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
//static MosMutex TestMutex2;

// Test Message Queue
static u32 queue[4];
static MosQueue TestQueue;

#if MOS_HANG_ON_EXCEPTIONS != false
#error This test bench requires MOS_HANG_ON_EXCEPTIONS == false
#endif

bool IsStopRequested() {
    return (bool)mosGetRunningThread()->pUser;
}

void RequestThreadStop(MosThread * pThd) {
    pThd->pUser = (void *)1;
}

static void ClearHistogram(void) {
    for (u32 ix = 0; ix < count_of(TestHisto); ix++)
        TestHisto[ix] = 0;
}

static void DisplayHistogram(u32 cnt) {
    for (u32 ix = 0; ix < cnt; ix++)
        mosPrintf(" Histo[%u] = %u\n", ix, TestHisto[ix]);
}

void MOS_ISR_SAFE IRQ0_Callback(void) {
    mosIncrementSem(&TestSem);
    TestHisto[0]++;
}

void MOS_ISR_SAFE IRQ1_Callback(void) {
    if (mosTrySendToQueue32(&TestQueue, 1)) TestHisto[0]++;
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
        if (IsStopRequested()) break;
        TestHisto[arg]++;
        // NOTE: Non-blocking delay
        mosDelayMicroseconds(pri_test_delay * 1000);
    }
    return TEST_PASS;
}

static s32 KillTestHandler(s32 arg) {
    mosPrint("KillTestHandler: Running Handler\n");
    if (mosIsMutexOwner(&TestMutex)) {
        mosPrint("KillTestHandler: I own mutex\n");
        mosRestoreMutex(&TestMutex);
    }
    return arg;
}

static s32 KillTestThread(s32 arg) {
    if (arg) {
        mosSetTermHandler(mosGetRunningThread(), KillTestHandler, TEST_PASS_HANDLER);
        // Lock mutex a couple times... need to release it in handler
        mosLockMutex(&TestMutex);
        mosLockMutex(&TestMutex);
    } else mosSetTermArg(mosGetRunningThread(), TEST_PASS_HANDLER);
    mosLogTrace(TRACE_INFO, "KillTestThread: Blocking\n");
    mosWaitForSem(&TestSem);
    return TEST_FAIL;
}

static s32 KillSelfTestThread(s32 arg) {
    if (arg) {
        mosSetTermHandler(mosGetRunningThread(), KillTestHandler, TEST_PASS_HANDLER);
        // Lock mutex a couple times... need to release it in handler
        mosLockMutex(&TestMutex);
        mosLockMutex(&TestMutex);
    } else mosSetTermArg(mosGetRunningThread(), TEST_PASS_HANDLER);
    mosLogTrace(TRACE_INFO, "KillSelfTestThread: Killing Self\n");
    mosKillThread(mosGetRunningThread());
    return TEST_FAIL;
}

static s32 AssertTestThread(s32 arg) {
    mosSetTermArg(mosGetRunningThread(), TEST_PASS_HANDLER);
    mosAssert(arg == 0x1234);
    return TEST_FAIL;
}

static s32 FPTestThread(s32 arg) {
    float x = 0.0;
    for (;;) {
        TestHisto[arg]++;
        x = x + 1.0;
        if (arg > 1 && (TestHisto[arg] == 1000)) {
            // Create an integer div-by-0 exception in FP thread
            mosSetTermArg(mosGetRunningThread(), TEST_PASS_HANDLER + 1);
            mosAssert(0);
            return TEST_FAIL;
        }
        if (IsStopRequested()) break;
    }
    if ((float)TestHisto[arg] != x) return TEST_FAIL;
    else return TEST_PASS;
}

/* Simulate two "libraries" using an index (libNum) */
static u32 storageID[2];

static void LibraryInit(u32 libNum) {
    static bool isInit[2] = { false, false };
    /* In "real-life" something like isInit here probably needs a mutex :-) */
    /* In this example it isn't required */
    if (!isInit[libNum]) {
        storageID[libNum] = mosGetUniqueID();
        isInit[libNum] = true;
    }
}

static void LibraryFreeCallback(void * pData) {
    mosFree(&TestThreadHeapDesc, pData);
    mosPrintf("  Free %p\n", pData);
}

static s32 LibraryRun(u32 libNum, s32 arg) {
    s32 val = 0;
    void * pData = mosGetThreadStorage(mosGetRunningThread(), storageID[libNum]);
    if (!pData) {
        pData = mosAlloc(&TestThreadHeapDesc, 100);
        mosPrintf("  %u: Alloc %p\n", libNum, pData);
        *((s32 *)pData) = arg;
        if (pData) {
            if (!mosSetThreadStorage(mosGetRunningThread(), storageID[libNum], pData, LibraryFreeCallback)) {
                mosFree(&TestThreadHeapDesc, pData);
                pData = NULL;
            }
        }
    }
    if (pData) val = *((s32 *)pData);
    return val;
}

static s32 StorageThread(s32 arg) {
    s32 retVal = TEST_PASS;
    /* The scenario that is simulated here is there are multiple threads that share library APIs,
     *  and those library implementations wish to store opaque data on a per-thread basis. */
    for (;;) {
        if (LibraryRun(0, arg) != arg) retVal = TEST_FAIL;
        if (LibraryRun(1, arg + 100) != arg + 100) retVal = TEST_FAIL;
        if (IsStopRequested()) break;
    }
    return retVal;
}

static bool ThreadTests(void) {
    const u32 test_time = 5000;
    u32 exp_iter = test_time / pri_test_delay;
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Highest priorities must starve lowest
    //
    test_pass = true;
    mosPrint("Priority Test 1\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, PriTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[1] != 0) test_pass = false;
    if (TestHisto[2] != 0) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Change of priority
    //
    test_pass = true;
    mosPrint("Priority Test 2\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, PriTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    mosChangeThreadPriority(Threads[1], 2);
    mosChangeThreadPriority(Threads[2], 1);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[1] < exp_iter || TestHisto[1] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[2] != 0) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Timeout on wait for thread
    //
    test_pass = true;
    mosPrint("Wait For Thread Stop with Timeout\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    s32 rtn_val;
    if (mosWaitForThreadStopOrTO(Threads[1], &rtn_val, test_time) != false) test_pass = false;
    RequestThreadStop(Threads[1]);
    if (mosWaitForThreadStopOrTO(Threads[1], &rtn_val, test_time) != true) test_pass = false;
    if (rtn_val != TEST_PASS) test_pass = false;
    DisplayHistogram(1);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // TODO: InitThread / RunThread
    // TODO: Set and Restore errno
    //
    //
    // Dynamic threads
    //
    test_pass = true;
    mosPrint("Dynamic Threads\n");
    ClearHistogram();
    MosThread * thd[2] = {0};
    mosAllocAndRunThread(&thd[0], 1, PriTestThread, 0, DFT_STACK_SIZE);
    mosAllocAndRunThread(&thd[1], 1, PriTestThread, 1, DFT_STACK_SIZE);
    if (thd[0] && thd[1]) {
        mosDelayThread(2 * test_time);
        RequestThreadStop(thd[0]);
        RequestThreadStop(thd[1]);
        if (mosWaitForThreadStop(thd[0]) != TEST_PASS) test_pass = false;
        if (mosWaitForThreadStop(thd[1]) != TEST_PASS) test_pass = false;
        mosDecThreadRefCount(&thd[0]);
        mosDecThreadRefCount(&thd[1]);
        DisplayHistogram(3);
        if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
            test_pass = false;
        if (TestHisto[1] < exp_iter || TestHisto[1] > exp_iter + 1)
            test_pass = false;
    } else {
        mosPrint("Cannot create threads!\n");
        test_pass = false;
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Thread Storage
    //
    test_pass = true;
    mosPrint("Thread Storage\n");
    ClearHistogram();
    thd[0] = NULL;
    thd[1] = NULL;
    LibraryInit(0);
    LibraryInit(1);
    mosAllocAndRunThread(&thd[0], 1, StorageThread, 0, DFT_STACK_SIZE);
    mosAllocAndRunThread(&thd[1], 1, StorageThread, 1, DFT_STACK_SIZE);
    if (thd[0] && thd[1]) {
        mosDelayThread(test_time);
        RequestThreadStop(thd[0]);
        RequestThreadStop(thd[1]);
        if (mosWaitForThreadStop(thd[0]) != TEST_PASS) test_pass = false;
        if (mosWaitForThreadStop(thd[1]) != TEST_PASS) test_pass = false;
        mosDecThreadRefCount(&thd[0]);
        mosDecThreadRefCount(&thd[1]);
    } else {
        mosPrint("Cannot create threads!\n");
        test_pass = false;
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Kill Thread using Default Handler
    //
    test_pass = true;
    mosPrint("Kill Test 1\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitSem(&TestSem, 0);
    mosInitAndRunThread(Threads[1], 1, KillTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosDelayThread(10);
    mosKillThread(Threads[1]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Kill Thread using Supplied Handler
    //
    test_pass = true;
    mosPrint("Kill Test 2\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitSem(&TestSem, 0);
    mosInitAndRunThread(Threads[1], 1, KillTestThread, 1, Stacks[1], DFT_STACK_SIZE);
    mosDelayThread(10);
    mosKillThread(Threads[1]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    if (TestMutex.pOwner != NULL) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Thread killing self
    //
    test_pass = true;
    mosPrint("Kill Test 3\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitSem(&TestSem, 0);
    mosInitAndRunThread(Threads[1], 1, KillSelfTestThread, 1, Stacks[1], DFT_STACK_SIZE);
    mosDelayThread(10);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    if (TestMutex.pOwner != NULL) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Assertion/Exception test
    //
    test_pass = true;
    mosPrint("Assertion/Exception Test\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, AssertTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER) test_pass = false;
    mosInitAndRunThread(Threads[1], 1, AssertTestThread, 0x1234, Stacks[1], DFT_STACK_SIZE);
    if (mosWaitForThreadStop(Threads[1]) != TEST_FAIL) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try some floating point
    //
    test_pass = true;
    mosPrint("FP Test\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, FPTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 1, FPTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 1, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time / 2);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    test_pass = true;
    mosPrint("Exception in FP thread\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, FPTestThread, 2, Stacks[1], DFT_STACK_SIZE);
    mosSetThreadName(Threads[1], "fp_thread");
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER + 1) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

// Make delay a multiple of 4
static const u32 timer_test_delay = 100;

static s32 ThreadTimerTestThread(s32 arg) {
    for (;;) {
        if (IsStopRequested()) break;
        mosDelayThread(timer_test_delay);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestThread2(s32 arg) {
    for (;;) {
        if (IsStopRequested()) break;
        mosDelayThread(timer_test_delay / 2);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestThread4(s32 arg) {
    for (;;) {
        if (IsStopRequested()) break;
        mosDelayThread(timer_test_delay / 4);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestThreadOdd(s32 arg) {
    for (;;) {
        if (IsStopRequested()) break;
        mosDelayThread(arg & 0xFFFF);
        TestHisto[arg >> 16]++;
    }
    return TEST_PASS;
}

static s32 ThreadTimerTestBusyThread(s32 arg) {
    for (;;) {
        if (IsStopRequested()) break;
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static MosTimer self_timer;

static bool MOS_ISR_SAFE ThreadTimerCallback(MosTimer * tmr) {
    return mosTrySendToQueue32(&TestQueue, (u32)tmr->pUser);
}

static s32 MessageTimerTestThread(s32 arg) {
    mosInitQueue32(&TestQueue, queue, count_of(queue));
    mosInitTimer(&self_timer, &ThreadTimerCallback);
    u32 cnt = 0xdeadbeef;
    for (;;) {
        if (IsStopRequested()) break;
        mosSetTimer(&self_timer, timer_test_delay, (void *)cnt);
        u32 val = mosReceiveFromQueue32(&TestQueue);
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
    // Zero timers
    //
    test_pass = true;
    mosPrint("Thread Timer Test 0\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, ThreadTimerTestThreadOdd, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 1, ThreadTimerTestThreadOdd, 37 | 0x10000, Stacks[2], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (TestHisto[1] != (test_time / 37) + 1) test_pass = false;
    // Bad time checks
    mosDelayThread(0);
    mosDelayThread((u32)(-4));
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run uniform timers
    //
    test_pass = true;
    mosPrint("Thread Timer Test 1\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, ThreadTimerTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, ThreadTimerTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter) test_pass = false;
    if (TestHisto[2] != exp_iter) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run "harmonic" timers
    //
    test_pass = true;
    mosPrint("Thread Timer Test 2\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, ThreadTimerTestThread2, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, ThreadTimerTestThread4, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter * 2) test_pass = false;
    if (TestHisto[2] != exp_iter * 4) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run odd timers
    //
    test_pass = true;
    mosPrint("Thread Timer Test 3\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, ThreadTimerTestThreadOdd, 13, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, ThreadTimerTestThreadOdd, 33 | 0x10000, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, ThreadTimerTestThreadOdd, 37 | 0x20000, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != (test_time / 13) + 1) test_pass = false;
    if (TestHisto[1] != (test_time / 33) + 1) test_pass = false;
    if (TestHisto[2] != (test_time / 37) + 1) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run two timers over busy thread
    //
    test_pass = true;
    mosPrint("Thread Timer Test 4\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 1, ThreadTimerTestThread2, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, ThreadTimerTestBusyThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter * 2) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run timer over two busy threads
    //
    test_pass = true;
    mosPrint("Thread Timer Test 5\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, ThreadTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, ThreadTimerTestBusyThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, ThreadTimerTestBusyThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // User timers 1
    //
    test_pass = true;
    mosPrint("User Timer Test 1\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, MessageTimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    DisplayHistogram(1);
    if (TestHisto[0] != exp_iter) test_pass = false;
#if 0
    //
    // User timers 2
    //
    test_pass = true;
    mosPrint("User Timer Test 2\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 1, MessageTimerTestThread2, 0, Stacks[1], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    DisplayHistogram(1);
    if (TestHisto[0] != exp_iter) test_pass = false;
#endif
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
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
        HalTestsTriggerInterrupt(0);
        TestHisto[arg]++;
        mosDelayThread(sem_test_delay);
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadTx(s32 arg) {
    for (;;) {
        mosIncrementSem(&TestSem);
        TestHisto[arg]++;
        mosDelayThread(sem_test_delay);
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadTxFast(s32 arg) {
    for (;;) {
        mosIncrementSem(&TestSem);
        mosDelayMicroseconds(10);
        TestHisto[arg]++;
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadRx(s32 arg) {
    for (;;) {
        mosWaitForSem(&TestSem);
        TestHisto[arg]++;
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadRxTimeout(s32 arg) {
    for (;;) {
        if (mosWaitForSemOrTO(&TestSem, sem_test_delay / 2 + 10)) {
            TestHisto[arg]++;
        } else {
            TestHisto[arg + 1]++;
        }
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SemTestThreadRxTry(s32 arg) {
    for (;;) {
        if (mosTrySem(&TestSem)) {
            TestHisto[arg]++;
        }
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestThreadTx(s32 arg) {
    for (;;) {
        mosRaiseSignal(&TestSem, 1 << arg);
        TestHisto[arg]++;
        mosDelayThread(sem_test_delay);
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestThreadRx(s32 arg) {
    for (;;) {
        u32 flags = mosWaitForSignal(&TestSem);
        mosAssert(flags > 0);
        mosAssert(flags <= 3);
        if (flags & 0x1) TestHisto[arg]++;
        if (flags & 0x2) TestHisto[arg + 1]++;
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestThreadRxTimeout(s32 arg) {
    for (;;) {
        u32 flags = mosWaitForSignalOrTO(&TestSem, 100);
        if (flags) {
            mosAssert(flags <= 3);
            if (flags & 0x1) TestHisto[arg]++;
            if (flags & 0x2) TestHisto[arg + 1]++;
        } else mosAssert(0);
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 SignalTestPoll(s32 arg) {
    for (;;) {
        u32 flags = mosPollSignal(&TestSem);
        if (flags) {
            if (flags & 0x1) TestHisto[arg]++;
            if (flags & 0x2) TestHisto[arg + 1]++;
        }
        if (IsStopRequested()) break;
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
    mosPrint("Sem Test 1\n");
    ClearHistogram();
    mosInitSem(&TestSem, 5);
    mosInitAndRunThread(Threads[1], 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, SemTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    mosIncrementSem(&TestSem);  // Unblock thread to stop
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5 + 1)
        test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Post from interrupt and threads
    //
    test_pass = true;
    mosPrint("Sem Test 2\n");
    ClearHistogram();
    mosInitSem(&TestSem, 0);
    mosInitAndRunThread(Threads[1], 1, SemTestPendIRQ, 1, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, SemTestThreadRx, 3, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    mosIncrementSem(&TestSem); // Unblock thread to stop
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[3] != TestHisto[0] + TestHisto[2] + 1)
        test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Sem with Timeouts
    //
    test_pass = true;
    mosPrint("Sem Test 3\n");
    ClearHistogram();
    mosInitSem(&TestSem, 5);
    mosInitAndRunThread(Threads[1], 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, SemTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5)
        test_pass = false;
    // Add one for last timeout before thread stop
    if (TestHisto[3] != exp_cnt + 1) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Lots of semaphores
    //
    test_pass = true;
    mosPrint("Sem Test 4\n");
    ClearHistogram();
    mosInitSem(&TestSem, 5);
    mosInitAndRunThread(Threads[1], 2, SemTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, SemTestThreadTxFast, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, SemTestThreadTxFast, 1, Stacks[2], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    mosDelayThread(5);
    RequestThreadStop(Threads[1]);
    mosIncrementSem(&TestSem);  // Unblock thread to stop
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5 + 1)
        test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // TrySem
    //
    test_pass = true;
    mosPrint("Try Sem\n");
    ClearHistogram();
    mosInitSem(&TestSem, 5);
    mosInitAndRunThread(Threads[1], 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, SemTestThreadRxTry, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5)
        test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Signals
    //
    test_pass = true;
    mosPrint("Signals\n");
    ClearHistogram();
    mosInitSem(&TestSem, 0);
    mosInitAndRunThread(Threads[1], 1, SignalTestThreadRx, 2, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, SignalTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, SignalTestThreadTx, 0, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    mosRaiseSignal(&TestSem, 2);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1] + 1) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Signals with Timeout
    //
    test_pass = true;
    mosPrint("Signals With Timeout\n");
    ClearHistogram();
    mosInitSem(&TestSem, 0);
    mosInitAndRunThread(Threads[1], 1, SignalTestThreadRxTimeout, 2, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, SignalTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, SignalTestThreadTx, 0, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    mosRaiseSignal(&TestSem, 2);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1] + 1) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Poll Signals
    //
    test_pass = true;
    mosPrint("Signal Polling\n");
    ClearHistogram();
    mosInitSem(&TestSem, 0);
    mosInitAndRunThread(Threads[1], 1, SignalTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, SignalTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, SignalTestPoll, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1]) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

//
// Queue Testing
//

static const u32 queue_test_delay = 50;

static s32 QueueTestPendIRQ(s32 arg) {
    MOS_UNUSED(arg);
    for (;;) {
        // Fire Software Interrupt
        HalTestsTriggerInterrupt(1);
        mosDelayThread(queue_test_delay);
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadTx(s32 arg) {
    for (;;) {
        mosSendToQueue32(&TestQueue, arg);
        TestHisto[arg]++;
        mosDelayThread(queue_test_delay);
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadTxTimeout(s32 arg) {
    for (;;) {
        if (mosSendToQueue32OrTO(&TestQueue, 2, queue_test_delay / 2 + 10)) {
            TestHisto[arg]++;
        } else {
            TestHisto[arg + 1]++;
        }
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRx(s32 arg) {
    for (;;) {
        u32 val = mosReceiveFromQueue32(&TestQueue);
        TestHisto[arg + val]++;
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRxTry(s32 arg) {
    for (;;) {
        u32 val;
        if (mosTryReceiveFromQueue32(&TestQueue, &val)) {
            TestHisto[arg + val]++;
            if (IsStopRequested()) break;
        }
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRxSlow(s32 arg) {
    for (;;) {
        u32 val = mosReceiveFromQueue32(&TestQueue);
        TestHisto[arg + val]++;
        if (IsStopRequested()) break;
        mosDelayThread(queue_test_delay);
    }
    return TEST_PASS;
}

static s32 QueueTestThreadRxTimeout(s32 arg) {
    for (;;) {
        u32 val;
        if (mosReceiveFromQueue32OrTO(&TestQueue, &val, queue_test_delay / 2 + 2)) {
            TestHisto[arg + val]++;
        } else {
            TestHisto[arg + 3]++;
        }
        if (IsStopRequested()) break;
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
    mosPrint("Queue Test 1\n");
    ClearHistogram();
    mosInitQueue32(&TestQueue, queue, count_of(queue));
    mosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, QueueTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    RequestThreadStop(Threads[3]);
    mosSendToQueue32(&TestQueue, 2); // Unblock thread to stop
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2] + 1) test_pass = false;
    if (TestQueue.pHead != TestQueue.pTail) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Receive From Queue Timeout
    //
    test_pass = true;
    mosPrint("Queue Test 2\n");
    ClearHistogram();
    mosInitQueue32(&TestQueue, queue, count_of(queue));
    mosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, QueueTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(6);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2]) test_pass = false;
    if (TestHisto[5] != exp_cnt + 1) test_pass = false;
    if (TestQueue.pHead != TestQueue.pTail) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Send to Queue Timeout
    //   NOTE: The interrupt will only be able to queue the first entry since the
    //   thread will hog the queue.
    //
    test_pass = true;
    mosPrint("Queue Test 3\n");
    ClearHistogram();
    mosInitQueue32(&TestQueue, queue, count_of(queue));
    mosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, QueueTestThreadTxTimeout, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, QueueTestThreadRxSlow, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    // Give Thread 3 extra time to drain the queue
    mosDelayThread(queue_test_delay * (count_of(queue) + 1));
    RequestThreadStop(Threads[3]);
    mosSendToQueue32(&TestQueue, 2);
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[2] != exp_cnt) test_pass = false;
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[1] + 1) test_pass = false;
    if (TestQueue.pHead != TestQueue.pTail) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Queue
    //
    test_pass = true;
    mosPrint("Queue Test 4\n");
    ClearHistogram();
    mosInitQueue32(&TestQueue, queue, count_of(queue));
    mosInitAndRunThread(Threads[1], 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, QueueTestThreadRxTry, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(test_time);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    mosSendToQueue32(&TestQueue, 2); // Unblock thread to stop
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2] + 1) test_pass = false;
    if (TestQueue.pHead != TestQueue.pTail) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

s32 MultiTestThreadTx(s32 arg) {
    return TEST_PASS;
}

s32 MultiTestThreadRx(s32 arg) {
    return TEST_PASS;
}

//
// Multiple Queue / Semaphore Tests
//
static bool MultiTests(void) {
    bool tests_all_pass = true;
    bool test_pass;

    // Three priority queues and shared signal
    MosQueue  queue[3];
    u32       queueBuf[3][4];
    MosSignal signal;

    test_pass = true;
    mosPrint("Multi Queue Test 1\n");
    mosInitSem(&signal, 0);
    for (u16 chan = 0; chan < count_of(queue); chan++) {
        mosInitQueue32(&queue[chan], queueBuf[chan], count_of(queueBuf[chan]));
        mosSetMultiQueueChannel(&queue[chan], &signal, chan);
    }
    mosSendToQueue32(&queue[0], 0);
    mosSendToQueue32(&queue[1], 1);
    mosSendToQueue32(&queue[2], 2);
    u32 flags = 0;
    u32 cleared_flags = 0;
    u32 received_flags = 0;
    do {
        u32 val;
        s16 chan = mosWaitOnMultiQueue(&signal, &flags);
        if (mosTryReceiveFromQueue32(&queue[chan], &val)) {
            if (val != (u16)chan) test_pass = false;
            received_flags |= (1 << chan);
        } else {
            mosClearChannelFlag(&flags, chan);
            cleared_flags |= (1 << chan);
        }
    } while (flags);
    if (received_flags != 0x7) test_pass = false;
    if (cleared_flags != 0x7) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

//
// Mutex Tests
//

static s32 MutexRecursion(s32 depth) {
    const s32 max_test_depth = 4;
    TestStatus status = TEST_PASS;
    mosLockMutex(&TestMutex);
    if (TestMutex.depth != depth) {
        status = TEST_FAIL;
    } else if (depth < max_test_depth) {
        if (MutexRecursion(depth + 1) == TEST_FAIL)
            status = TEST_FAIL;
    }
    mosUnlockMutex(&TestMutex);
    return status;
}

#define MUTEX_TEST_PRIO_INHER     5

static s32 MutexTestThread(s32 arg) {
    TestStatus status = TEST_PASS;
    for (;;) {
        if (arg == MUTEX_TEST_PRIO_INHER) {
            static u32 count = 0;
            if ((count++ & 0xfff) == 0) {
                // Give low priority thread chance to acquire mutex
                mosTrySendToQueue32(&TestQueue, 0);
                mosDelayThread(5);
            }
        }
        mosLockMutex(&TestMutex);
        if (TestFlag == 1) {
            status = TEST_FAIL;
            goto EXIT_MTT;
        }
        TestFlag = 1;
        if (IsStopRequested())
            goto EXIT_MTT;
        if (MutexRecursion(2) == TEST_FAIL) {
            status = TEST_FAIL;
            goto EXIT_MTT;
        }
        TestHisto[arg]++;
        TestFlag = 0;
        mosUnlockMutex(&TestMutex);
    }
EXIT_MTT:
    TestFlag = 0;
    mosUnlockMutex(&TestMutex);
    return status;
}

static s32 MutexTryTestThread(s32 arg) {
    TestStatus status = TEST_PASS;
    for (;;) {
        if (mosTryMutex(&TestMutex)) {
            if (TestFlag == 1) {
                status = TEST_FAIL;
                goto EXIT_MTT;
            }
            TestFlag = 1;
            if (IsStopRequested())
                goto EXIT_MTT;
            if (MutexRecursion(2) == TEST_FAIL) {
                status = TEST_FAIL;
                goto EXIT_MTT;
            }
            TestHisto[arg]++;
            TestFlag = 0;
            mosUnlockMutex(&TestMutex);
        }
    }
EXIT_MTT:
    TestFlag = 0;
    mosUnlockMutex(&TestMutex);
    return status;
}

#if 0
static s32 MutexNestedPrioInversionThread(s32 arg) {
	for (;;) {
		mosLockMutex(&TestMutex);
		mosLockMutex(&TestMutex2);


		mosUnlockMutex(&TestMutex2);
		mosUnlockMutex(&TestMutex);
	}
}
#endif

// Dummy thread is used for priority inheritance tests and
//   runs at the middle priority.
static s32 MutexDummyThread(s32 arg) {
    for (;;) {
        u32 dummy;
        if (mosTryReceiveFromQueue32(&TestQueue, &dummy)) {
            // Give low priority thread chance to acquire mutex
            mosDelayThread(2);
        }
        TestHisto[arg]++;
        if (IsStopRequested())
            break;
    }
    return TEST_PASS;
}

static s32 MutexChangePrioThread(s32 arg) {
    for (;;) {
        mosLockMutex(&TestMutex);
        mosUnlockMutex(&TestMutex);
        mosPrintf("Thread %d run\n", arg);
        TestHisto[arg]++;
        if (IsStopRequested())
            break;
    }
    return TEST_PASS;
}

static s32 MutexBusyThread(s32 arg) {
    for (;;) {
        if (IsStopRequested()) break;
        TestHisto[arg]++;
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
    mosPrint("Mutex Test 1\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitAndRunThread(Threads[1], 3, MutexTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, MutexTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosDelayThread(5000);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Contention / Depth 2
    //
    test_pass = true;
    mosPrint("Mutex Test 2\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitAndRunThread(Threads[1], 3, MutexTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 3, MutexTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, MutexTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(5000);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Mutex
    //
    test_pass = true;
    mosPrint("Try Mutex\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitAndRunThread(Threads[1], 2, MutexTryTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, MutexTryTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosDelayThread(5000);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Mutex 2
    //
    test_pass = true;
    mosPrint("Try Mutex Test 2\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitAndRunThread(Threads[1], 2, MutexTryTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, MutexTryTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, MutexTryTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(5000);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Priority Inheritance (1 level)
    //
    test_pass = true;
    mosPrint("Mutex Priority Inversion\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosInitQueue32(&TestQueue, queue, count_of(queue));
    mosInitAndRunThread(Threads[1], 1, MutexTestThread, MUTEX_TEST_PRIO_INHER,
                        Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, MutexDummyThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 3, MutexTestThread, 0, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(5000);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    DisplayHistogram(6);
    // It's possible scheduler wakes threads when lowest priority one doesn't hold mutex
    if (TestHisto[MUTEX_TEST_PRIO_INHER] <= 4096) test_pass = false;
    // Make sure thread priorities are restored
    if (mosGetThreadPriority(Threads[1]) != 1) test_pass = false;
    if (mosGetThreadPriority(Threads[2]) != 2) test_pass = false;
    if (mosGetThreadPriority(Threads[3]) != 3) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
#if 0
    //
    // Priority Inheritance (nested)
    //
    test_pass = true;
    mosPrint("Mutex Nested Priority Inversion\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);

    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
#endif
    //
    // Mutex Priority Change Test
    //
    test_pass = true;
    mosPrint("Mutex Thread Priority Change\n");
    ClearHistogram();
    mosInitMutex(&TestMutex);
    mosLockMutex(&TestMutex);
    mosInitAndRunThread(Threads[1], 2, MutexChangePrioThread, 2, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, MutexChangePrioThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, MutexChangePrioThread, 0, Stacks[3], DFT_STACK_SIZE);
    mosDelayThread(2);
    mosInitAndRunThread(Threads[4], 1, MutexBusyThread, 3, Stacks[4], DFT_STACK_SIZE);
    mosDelayThread(2);
    // Changing priorities should force thread order 0 -> 2 (on console log)
    mosChangeThreadPriority(Threads[1], 3);
    mosChangeThreadPriority(Threads[3], 0);
    mosUnlockMutex(&TestMutex);
    if (mosGetThreadPriority(Threads[1]) != 3) test_pass = false;
    if (mosGetThreadPriority(Threads[2]) != 2) test_pass = false;
    if (mosGetThreadPriority(Threads[3]) != 0) test_pass = false;
    if (mosGetThreadPriority(Threads[4]) != 1) test_pass = false;
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    RequestThreadStop(Threads[4]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[4]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

static bool HeapTests(void) {
    bool tests_all_pass = true;
    bool test_pass;
    MosHeap TestHeapDesc;
    //
    // Slabs 1
    //
    test_pass = true;
    mosPrint("Heap Test 1: Slabs\n");
    {
        const u32 alignment = 4;
        const u32 block_size = 20;
        mosInitHeap(&TestHeapDesc, 8, TestHeap, sizeof(TestHeap));
        MosPool TestPoolDesc;
        mosInitPool(&TestPoolDesc, &TestHeapDesc, 32, 20, alignment);
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
        if (mosAddSlabsToPool(&TestPoolDesc, 2) != 2) test_pass = false;
        u8 * block[64];
        for (u32 ix = 0; ix < count_of(block); ix++) {
            block[ix] = mosAllocFromSlab(&TestPoolDesc);
            if (!block[ix]) test_pass = false;
            if ((u32)block[ix] % alignment != 0) test_pass = false;
            memset(block[ix], 0xa5, block_size);
        }
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
        for (u32 ix = 0; ix < count_of(block); ix++) {
            mosFreeToSlab(&TestPoolDesc, block[ix]);
        }
        if (mosFreeUnallocatedSlabs(&TestPoolDesc, 2) != 2) test_pass = false;
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
        if (mosAddSlabsToPool(&TestPoolDesc, 2) != 2) test_pass = false;
        for (u32 ix = 0; ix < count_of(block); ix++) {
            block[ix] = mosAllocFromSlab(&TestPoolDesc);
            if (!block[ix]) test_pass = false;
            if ((u32)block[ix] % alignment != 0) test_pass = false;
            memset(block[ix], 0x5a, block_size);
        }
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
        for (u32 ix = 0; ix < count_of(block); ix++) {
            mosFreeToSlab(&TestPoolDesc, block[count_of(block) - ix - 1]);
        }
        if (mosFreeUnallocatedSlabs(&TestPoolDesc, 2) != 2) test_pass = false;
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Slabs 2
    //
    test_pass = true;
    mosPrint("Heap Test 2: Slabs 2\n");
    {
        const u32 alignment = 32;
        const u32 block_size = 64;
        mosInitHeap(&TestHeapDesc, 8, TestHeap, sizeof(TestHeap));
        MosPool TestPoolDesc;
        mosInitPool(&TestPoolDesc, &TestHeapDesc, 64, block_size, alignment);
        if (mosAddSlabsToPool(&TestPoolDesc, 2) != 2) test_pass = false;
        u8 * block[128];
        for (u32 ix = 0; ix < count_of(block); ix++) {
            block[ix] = mosAllocFromSlab(&TestPoolDesc);
            if (!block[ix]) test_pass = false;
            if ((u32)block[ix] % alignment != 0) test_pass = false;
            memset(block[ix], 0xa5, block_size);
        }
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
        for (u32 ix = 0; ix < count_of(block); ix++) {
            mosFreeToSlab(&TestPoolDesc, block[ix]);
        }
        if (mosFreeUnallocatedSlabs(&TestPoolDesc, 2) != 2) test_pass = false;
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
        if (mosAddSlabsToPool(&TestPoolDesc, 2) != 2) test_pass = false;
        for (u32 ix = 0; ix < count_of(block); ix++) {
            block[ix] = mosAllocFromSlab(&TestPoolDesc);
            if (!block[ix]) test_pass = false;
            if ((u32)block[ix] % alignment != 0) test_pass = false;
            memset(block[ix], 0x5a, block_size);
        }
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
        for (u32 ix = 0; ix < count_of(block); ix++) {
            mosFreeToSlab(&TestPoolDesc, block[count_of(block) - ix - 1]);
        }
        if (mosFreeUnallocatedSlabs(&TestPoolDesc, 2) != 2) test_pass = false;
        if (mosAllocFromSlab(&TestPoolDesc) != NULL) test_pass = false;
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Reallocation
    //
    test_pass = true;
    mosPrint("Heap Test 3: Reallocation\n");
    u32 alignment = 8;
    mosInitHeap(&TestHeapDesc, alignment, TestHeap, sizeof(TestHeap));
    u8 * fun[8];
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = mosAlloc(&TestHeapDesc, 400);
        if (fun[ix] == NULL) test_pass = false;
        else memset(fun[ix], ix, 400);
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = mosRealloc(&TestHeapDesc, fun[ix], 600);
        if (fun[ix] == NULL) test_pass = false;
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 400; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = mosRealloc(&TestHeapDesc, fun[ix], 400);
        if (fun[ix] == NULL) test_pass = false;
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 400; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = mosRealloc(&TestHeapDesc, fun[ix], 100);
        if (fun[ix] == NULL) test_pass = false;
        if (((u32)fun[ix] & (alignment - 1)) != 0) test_pass = false;
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 100; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = mosRealloc(&TestHeapDesc, fun[ix], 128);
        if (fun[ix] == NULL) test_pass = false;
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        if (fun[ix] != NULL) {
            for (u32 iy = 0; iy < 100; iy++) {
                if (fun[ix][iy] != ix) test_pass = false;
            }
        }
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        mosFree(&TestHeapDesc, fun[ix]);
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Exhaustion
    //
    test_pass = true;
    mosPrint("Heap Test 4: Exhaustion\n");
    {
        const u32 bs1 = 64;
        u32 ctr = 0;
        void * block, * last_block = NULL;
        while (1) {
             block = mosAlloc(&TestHeapDesc, bs1);
             if (block) last_block = block;
             else break;
             if (((u32)block & (alignment - 1)) != 0) test_pass = false;
             ctr++;
        } while (block);
        mosFree(&TestHeapDesc, last_block);
        if (mosAlloc(&TestHeapDesc, bs1) != last_block) test_pass = false;
        mosPrintf("Allocated up to %u blocks\n", ctr);
        if (ctr != sizeof(TestHeap) / (bs1 + 12 + 4)) test_pass = false;
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)

#include "secure_nsc.h"

static s32 SecurityThread(s32 arg) {
    while (1) {
        mosDelayMicroseconds(100000);
        mosReserveSecureContext();
        mosDelayMicroseconds(100000);
        SECURE_TakeSomeTime();
        mosReleaseSecureContext();
        TestHisto[arg]++;
        if (IsStopRequested()) break;
    }
    return TEST_PASS;
}

static bool SecurityTests(void) {
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Secure Context switch (2 threads)
    //
    test_pass = true;
    mosPrint("Security Test: Context Switch (2 secure threads)\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 2, FPTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, FPTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, SecurityThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[4], 2, SecurityThread, 3, Stacks[4], DFT_STACK_SIZE);
    mosDelayThread(10000);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    RequestThreadStop(Threads[4]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[4]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Secure Context switch (3 threads)
    //
    test_pass = true;
    mosPrint("Security Test: Context Switch (3 secure threads)\n");
    ClearHistogram();
    mosInitAndRunThread(Threads[1], 2, FPTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[2], 2, SecurityThread, 1, Stacks[2], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[3], 2, SecurityThread, 2, Stacks[3], DFT_STACK_SIZE);
    mosInitAndRunThread(Threads[4], 2, SecurityThread, 3, Stacks[4], DFT_STACK_SIZE);
    mosDelayThread(10000);
    RequestThreadStop(Threads[1]);
    RequestThreadStop(Threads[2]);
    RequestThreadStop(Threads[3]);
    RequestThreadStop(Threads[4]);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[2]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[3]) != TEST_PASS) test_pass = false;
    if (mosWaitForThreadStop(Threads[4]) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

#endif

static s32 StackPrintThread(s32 arg) {
    MOS_UNUSED(arg);
    u64 e = 0xdeadbeeffeebdaed;
    mosPrintf("DEADBEEFFEEBDAED == %llX\n", e);
    return TEST_PASS;
}

#if (MOS_ENABLE_SPLIM_SUPPORT == true)

static s32 MOS_OPT(0) StackOverflowThread(s32 arg) {
    mosSetTermArg(mosGetRunningThread(), TEST_PASS_HANDLER + 1);
    return StackOverflowThread(arg);
}

#endif

static bool MiscTests(void) {
    bool tests_all_pass = true;
    bool test_pass;
    //
    // 64-bit print test (stack alignment)
    //
    test_pass = true;
    mosPrint("Misc Test 1: Stack and 64-bit print alignment\n");
    for (u32 ix = 0; ix < 8; ix++) {
        mosInitAndRunThread(Threads[1], 3, StackPrintThread, 1, Stacks[1], DFT_STACK_SIZE + ix);
        if (mosWaitForThreadStop(Threads[1]) != TEST_PASS) test_pass = false;
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Stack Stats
    //
    test_pass = true;
    mosPrint("Misc Test: Stack stats\n");
    {
        u32 size = 0, usage = 0, max_usage = 0;
        mosGetStackStats(mosGetRunningThread(), &size, &usage, &max_usage);
        mosPrintf("Stack: size: %u usage: %u max_usage: %u\n", size, usage, max_usage);
        if (size != mosGetStackSize(mosGetRunningThread())) test_pass = false;
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
#if (MOS_ENABLE_SPLIM_SUPPORT == true)
    //
    // PSPLIM tests
    //
    test_pass = true;
    mosPrint("Misc Test: PSPLIM\n");
    mosInitAndRunThread(Threads[1], 2, StackOverflowThread, 0, Stacks[2], DFT_STACK_SIZE);
    if (mosWaitForThreadStop(Threads[1]) != TEST_PASS_HANDLER + 1) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }

#if 0
    // Tests security fault
    u32 val = *((volatile u32 *)0x30000038);
    mosPrintf("%08X\n", val);
#endif

#endif
    //
    // MosSNPrintf
    //
    test_pass = true;
    mosPrint("Misc Test: MosSNPrintf()\n");
    {
        char buf[128];
        {   // String, Char, %%, Int
            char * dummy = "bummy_dummy_mummy_";
            if (mosSNPrintf(buf, 32, "%s%s%s", dummy, dummy, dummy) != 54) test_pass = false;
            if (mosSNPrintf(buf, sizeof(buf), "%s", dummy) != (s32)strlen(dummy)) test_pass = false;
            if (strcmp(buf, dummy)) test_pass = false;
            if (mosSNPrintf(buf, 8, "%c%%%d%%d%c%", '*', -1, '$') != 7) test_pass = false;
            if (strcmp(buf, "*%-1%d$")) test_pass = false;
        }
        {   // Int / Int64
            if (mosSNPrintf(buf, 11, "%010llx", 0xdeadbee90) != 10) test_pass = false;
            if (strcmp(buf, "0deadbee90")) test_pass = false;
            if (mosSNPrintf(buf, 11, "%10llx", 0xdeadbee90) != 10) test_pass = false;
            if (strcmp(buf, " deadbee90")) test_pass = false;
            // Truncation
            if (mosSNPrintf(buf, 8, "%lu", 123456789) != 9) test_pass = false;
            if (strcmp(buf, "1234567")) test_pass = false;
        }
        double dbl;
        u64 p0;
        double * pf = (double *) &p0;
        {   // %f - Float / Double rounding
            float flt = -1.375;
            mosSNPrintf(buf, sizeof(buf), "%0.4f", flt);
            if (strcmp(buf, "-1.3750")) test_pass = false;
            p0 = 0x3FD5555555555555;
            dbl = 0;
            if (mosSNPrintf(buf, sizeof(buf), "%f", dbl) != 8) test_pass = false;
            if (strcmp(buf, "0.000000")) test_pass = false;
            if (mosSNPrintf(buf, sizeof(buf), "%.0f", dbl) != 1) test_pass = false;
            if (strcmp(buf, "0")) test_pass = false;
            mosSNPrintf(buf, sizeof(buf), "%0.16f", *pf);
            if (strcmp(buf, "0.3333333333333333")) test_pass = false;
            p0 = 0x400921fb54442d18;
            mosSNPrintf(buf, sizeof(buf), "%0.9f", *pf);
            if (strcmp(buf, "3.141592654")) test_pass = false;
            dbl = 123456789;
            mosSNPrintf(buf, sizeof(buf), "%f", dbl);
            if (strcmp(buf, "123456789.000000")) test_pass = false;
            // Note: Tests rounding since 123456789.1 -> 123456789.099999
            dbl = -123456789.1;
            mosSNPrintf(buf, sizeof(buf), "%.0f", dbl);
            if (strcmp(buf, "-123456789")) test_pass = false;
            if (mosSNPrintf(buf, sizeof(buf), "%.1f", dbl) != 12) test_pass = false;
            if (strcmp(buf, "-123456789.1")) test_pass = false;
            if (mosSNPrintf(buf, sizeof(buf), "%f", *pf) != 8) test_pass = false;
            if (strcmp(buf, "3.141593")) test_pass = false;
        }
        {   // Large number clamp test
            dbl = -5.391245e44;
            if (mosSNPrintf(buf, sizeof(buf), "%.15f", dbl) != 4) test_pass = false;
            if (strcmp(buf, "-ovf")) test_pass = false;
            dbl = 1.7976931348623157E+308;
            if (mosSNPrintf(buf, sizeof(buf), "%.15f", dbl) != 3) test_pass = false;
            if (strcmp(buf, "ovf")) test_pass = false;
        }
        {   // 10.501 rounding test
            dbl = 10.501;
            mosSNPrintf(buf, sizeof(buf), "%.0f", dbl);
            if (strcmp(buf, "11")) test_pass = false;
            mosSNPrintf(buf, sizeof(buf), "%.1f", dbl);
            if (strcmp(buf, "10.5")) test_pass = false;
            mosSNPrintf(buf, sizeof(buf), "%.2f", dbl);
            if (strcmp(buf, "10.50")) test_pass = false;
            mosSNPrintf(buf, sizeof(buf), "%.3f", dbl);
            if (strcmp(buf, "10.501")) test_pass = false;
        }
        {
            // Inf / NaN
            p0 = 0x7ff0000000000000;
            if (mosSNPrintf(buf, sizeof(buf), "%f", *pf) != 3) test_pass = false;
            if (strcmp(buf, "inf")) test_pass = false;
            p0 = 0xfff0000000000000;
            if (mosSNPrintf(buf, sizeof(buf), "%f", *pf) != 4) test_pass = false;
            if (strcmp(buf, "-inf")) test_pass = false;
            p0 = 0xfff0000000000001;
            mosSNPrintf(buf, sizeof(buf), "%f", *pf);
            if (strcmp(buf, "-nan")) test_pass = false;
            p0 = 0xfff8000000000001;
            if (mosSNPrintf(buf, sizeof(buf), "%f", *pf) != 4) test_pass = false;
            if (strcmp(buf, "-nan")) test_pass = false;
            p0 = 0x7ff0000000000001;
            mosSNPrintf(buf, sizeof(buf), "%f", *pf);
            if (strcmp(buf, "nan")) test_pass = false;
            p0 = 0x7ff8000000000000;
            mosSNPrintf(buf, sizeof(buf), "%f", *pf);
            if (strcmp(buf, "nan")) test_pass = false;
        }
        {
            dbl = 0.000134631111;
            mosSNPrintf(buf, sizeof(buf), "%e", dbl);
            if (strcmp(buf, "1.346311e-04")) test_pass = false;
            mosSNPrintf(buf, sizeof(buf), "%0.2e", dbl);
            if (strcmp(buf, "1.35e-04")) test_pass = false;
            dbl = 134.63e17;
            mosSNPrintf(buf, sizeof(buf), "%e", dbl);
            if (strcmp(buf, "1.346300e+19")) test_pass = false;
        }
#if 1
        {
            dbl = 2.71828182845904;
            mosSNPrintf(buf, sizeof(buf), "%.15g", dbl, dbl);
            mosPrintf("*%s*\n", buf);
        }
#endif
    }
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // String tests
    //
    test_pass = true;
    mosPrint("Misc Test: strtod() library test\n");
    double exp = 1.87554603778e-18;
    double diff = exp / 10.0;
    char * str3p0 = "1.87554603778e-18";
    char * ptr = NULL;
    double res = strtod(str3p0, &ptr);
    if (res < exp - diff || res > exp + diff) test_pass = false;
    if (test_pass) mosPrint(" Passed\n");
    else {
        mosPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

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
            if (MultiTests() == false) test_pass = false;
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
        } else if (strcmp(argv[1], "multi") == 0) {
            test_pass = MultiTests();
        } else if (strcmp(argv[1], "mutex") == 0) {
            test_pass = MutexTests();
        } else if (strcmp(argv[1], "heap") == 0) {
            test_pass = HeapTests();
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
        } else if (strcmp(argv[1], "sec") == 0) {
            test_pass = SecurityTests();
#endif
        } else if (strcmp(argv[1], "misc") == 0) {
            test_pass = MiscTests();
        } else return CMD_ERR_NOT_FOUND;
        if (test_pass) {
            mosPrint("Tests Passed\n");
            return CMD_OK;
        } else {
            mosPrint("Tests FAILED\n");
            return CMD_ERR;
        }
    }
    return CMD_ERR_NOT_FOUND;
}

static s32 CmdTime(s32 argc, char * argv[]) {
    static u64 start_ns = 0;
    u64 ns = mosGetTimeInNanoseconds();
    if (!start_ns) start_ns = ns;
    mosPrintf("Time ns: %llu\n", ns - start_ns);
    return CMD_OK;
}

static volatile bool PigeonFlag = false;

static s32 PigeonThread(s32 arg) {
    MOS_UNUSED(arg);
    u32 cnt = 0;
    u64 last = mosGetCycleCount();
    while (1) {
        mosDelayThread(1000);
        u64 tmp = mosGetCycleCount();
        u64 dur = tmp - last;
        last = tmp;
        mosPrintf("Incoming ---- .. .. %u %08X.. %llu ------\n", cnt,
                       mosGetStackDepth(mosGetStackBottom(NULL) + 2*DFT_STACK_SIZE),
                       dur);
        cnt++;
    }
    return 0;
}

static s32 CmdPigeon(s32 argc, char * argv[]) {
    MOS_UNUSED(argc);
    MOS_UNUSED(argv);
    if (!PigeonFlag) {
        MosThread * thd = Threads[PIGEON_THREAD_ID];
        mosInitAndRunThread(thd, 0, PigeonThread, 0, mosGetStackBottom(thd),
                            mosGetStackSize(thd));
        PigeonFlag = 1;
    } else {
        mosKillThread(Threads[PIGEON_THREAD_ID]);
        PigeonFlag = 0;
    }
    return CMD_OK;
}

static s32 CmdClearTickHisto(s32 argc, char * argv[]) {
    MOS_UNUSED(argc);
    MOS_UNUSED(argv);
    for (u32 ix = 0; ix < MAX_TICK_HISTO_ENTRIES; ix++) TickHisto[ix] = 0;
    SchedCount = 0;
    return CMD_OK;
}

static s32 CmdRegistry(s32 argc, char * argv[]) {
    if (argc == 3) {
        if (strcmp(argv[1], "get") == 0) {
            char string[64];
            u32 size = sizeof(string);
            if (mosGetStringEntry(NULL, argv[2], string, &size)) {
                mosPrintf("%s\n", string);
            } else return CMD_ERR;
        }
    } else if (argc == 4) {
        if (strcmp(argv[1], "set") == 0) {
            if (!mosSetStringEntry(NULL, argv[2], argv[3]))
                return CMD_ERR;
        }
    }
    return CMD_OK;
}

#define MAX_CMD_BUFFER_LENGTH   10
#define MAX_CMD_LINE_SIZE       128

static char CmdBuffers[MAX_CMD_BUFFER_LENGTH][MAX_CMD_LINE_SIZE] = {{ 0 }};

static s32 TestShell(s32 arg) {
    MOS_UNUSED(arg);
    MosShell Shell;
    mosInitShell(&Shell, MAX_CMD_BUFFER_LENGTH, MAX_CMD_LINE_SIZE, (void *)CmdBuffers, true);
    static MosShellCommand list_cmds[] = {
        { CmdTest,           "run", "Run Test", "[TEST]", {0} },
        { CmdTime,           "t",   "Print time", "", {0} },
        { CmdPigeon,         "p",   "Toggle Pigeon Printing", "", {0} },
        { CmdClearTickHisto, "cth", "Clear tick histogram", "", {0} },
        { CmdRegistry,       "reg", "Registry", "set|get name [value]", {0} },
    };
    for (u32 ix = 0; ix < count_of(list_cmds); ix++) {
        mosAddCommand(&Shell, &list_cmds[ix]);
    }
    mosRunShell(&Shell);
    return 0;
}

int InitTestBench() {
    HalTestsInit();

#if 0
    // Try out cycle counter (if implemented)
    mosPrintf("%X . %X\n", DWT->CTRL, CoreDebug->DEMCR);
    DWT->CTRL = 0x1;
    CoreDebug->DEMCR = 1 << 24;
    mosPrintf("%X = %X\n", &DWT->CTRL, DWT->CTRL);
    mosPrintf("%X = %X\n", &DWT->CYCCNT, DWT->CYCCNT);
    mosPrintf("%X = %X\n", &DWT->CYCCNT, DWT->CYCCNT);
#endif

    mosRegisterEventHook(EventCallback);

    mosInitHeap(&TestThreadHeapDesc, MOS_STACK_ALIGNMENT, TestThreadHeap, sizeof(TestThreadHeap));
    mosInitDynamicKernel(&TestThreadHeapDesc);
    mosRegistryInit(&TestThreadHeapDesc, '.');

    if (!mosAllocAndRunThread(&Threads[TEST_SHELL_THREAD_ID], 0, TestShell,
                              0, TEST_SHELL_STACK_SIZE)) {
        mosPrint("Thread allocation error\n");
        return -1;
    }

    if (!mosAllocThread(&Threads[PIGEON_THREAD_ID], 2*DFT_STACK_SIZE)) {
        mosPrint("Thread allocation error\n");
        return -1;
    }

    // Static threads with stacks allocated from the heap
    for (u32 id = 1; id < (MAX_APP_THREADS - 1); id++) {
        Threads[id] = &StaticThreads[id];
        Stacks[id] = mosAlloc(&TestThreadHeapDesc, DFT_STACK_SIZE);
        if (Stacks[id] == NULL) {
            mosPrint("Stack allocation error\n");
            return -1;
        }
    }
    return 0;
}
