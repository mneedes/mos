
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Test Bench
//

#include <errno.h>
#include <string.h>

#include "hal.h"
#include "mos.h"
#include "mosh.h"
#include "most.h"

#include "hal_tb.h"

#define DFT_STACK_SIZE           256
#define TEST_SHELL_STACK_SIZE    2048
#define MAX_TEST_SUB_THREADS     (MOS_MAX_APP_THREADS - 2)
#define MAX_TEST_THREADS         (MAX_TEST_SUB_THREADS + 2)

#define TEST_SHELL_THREAD_ID     MOS_MAX_APP_THREADS
#define PIGEON_THREAD_ID         (MOS_MAX_APP_THREADS - 1)

typedef enum {
   TRACE_DEBUG       = 1 << 0,
   TRACE_INFO        = 1 << 1,
   TRACE_ERROR       = 1 << 2,
   TRACE_FATAL       = 1 << 3,
} TraceLevel;

typedef enum {
    TEST_PASS         = 0xba5eba11,
    TEST_PASS_HANDLER = 0xba5eba12,
    TEST_FAIL         = 0xdeadbeef,
} TestStatus;

// Test thread stacks and heap
static u8 * Stacks[MAX_TEST_THREADS];
static u8 * PigeonStack;

static MoshHeap TestThreadHeapDesc;
static u8 MOSH_HEAP_ALIGNED TestThreadHeap[8192];

// Heap for Heap testing
static u8 MOSH_HEAP_ALIGNED TestHeap[16384];

// Generic flag for tests
static volatile u32 TestFlag = 0;

// Generic histogram for tests
#define MAX_HISTO_ENTRIES   102
static volatile u32 TestHisto[MAX_HISTO_ENTRIES];

// Test Sem / Mutex / Mux
static MosSem TestSem;
static MosMutex TestMutex;
static MosMux TestMux;

// Test Message Queue
static u32 queue[4];
static MosQueue TestQueue;

static void ClearHistogram(void) {
    for (u32 ix = 0; ix < count_of(TestHisto); ix++)
        TestHisto[ix] = 0;
}

static void DisplayHistogram(u32 cnt) {
    for (u32 ix = 0; ix < cnt; ix++)
        MostPrintf(" Histo[%u] = %u\n", ix, TestHisto[ix]);
}

void EXTI0_IRQHandler(void)
{
    MosGiveSem(&TestSem);
    TestHisto[0]++;
}

void EXTI1_IRQHandler(void)
{
    if (MosSendToQueue(&TestQueue, 1)) TestHisto[0]++;
}

//
// Thread tests
//

static const u32 pri_test_delay = 50;

static s32 PriTestThread(s32 arg) {
    for (;;) {
        TestHisto[arg]++;
        if (MosIsStopRequested()) break;
        // NOTE: Non-blocking delay
        MosDelayMicroSec(pri_test_delay * 1000);
    }
    return TEST_PASS;
}

static s32 KillTestHandler(s32 arg) {
    MostPrint("KillTestHandler: Running Handler\n");
    if (MosIsMutexOwner(&TestMutex)) {
        MostPrint("KillTestHandler: I own mutex\n");
        MosRestoreMutex(&TestMutex);
    }
    return arg;
}

static s32 KillTestThread(s32 arg) {
    if (arg) {
        MosSetKillHandler(MosGetThreadID(), KillTestHandler, TEST_PASS_HANDLER);
        // Take mutex a couple times... need to release it in handler
        MosTakeMutex(&TestMutex);
        MosTakeMutex(&TestMutex);
    } else MosSetKillArg(MosGetThreadID(), TEST_PASS_HANDLER);
    MostPrint("KillTestThread: Blocking\n");
    MosTakeSem(&TestSem);
    return TEST_FAIL;
}

static s32 ExcTestThread(s32 arg) {
    volatile u32 x;
    MosDelayThread(50);
    x = 30 / arg;
    (void)x;
    return TEST_FAIL;
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
    MostPrint("Priority Test 1\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, PriTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    DisplayHistogram(3);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[1] != 0) test_pass = false;
    if (TestHisto[2] != 0) test_pass = false;
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Change of priority
    //
    test_pass = true;
    MostPrint("Priority Test 2\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, PriTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, PriTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, PriTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosChangeThreadPriority(1, 2);
    MosChangeThreadPriority(2, 1);
    MosDelayThread(test_time);
    DisplayHistogram(3);
    if (TestHisto[0] < exp_iter || TestHisto[0] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[1] < exp_iter || TestHisto[1] > exp_iter + 1)
        test_pass = false;
    if (TestHisto[2] != 0) test_pass = false;
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Timeout on wait for thread
    //

    //
    // InitThread / RunThread
    //

    //
    // Set and Restore errno
    //

    //
    // Kill Thread using Default Handler
    //
    test_pass = true;
    MostPrint("Kill Test 1\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(1, 1, KillTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosDelayThread(10);
    MosKillThread(1);
    if (MosWaitForThreadStop(1) != TEST_PASS_HANDLER) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Kill Thread using Supplied Handler
    //
    test_pass = true;
    MostPrint("Kill Test 2\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(1, 1, KillTestThread, 1, Stacks[1], DFT_STACK_SIZE);
    MosDelayThread(10);
    MosKillThread(1);
    if (MosWaitForThreadStop(1) != TEST_PASS_HANDLER) test_pass = false;
    if (TestMutex.owner != -1) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Thread exception handler
    //   Currently passes if it doesn't hang, TODO: better recovery or error detection?
    test_pass = true;
    MostPrint("Exception Test\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, ExcTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosWaitForThreadStop(1);
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

// Make delay a multiple of 4
static const u32 timer_test_delay = 100;

static s32 TimerTestThread(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 TimerTestThread2(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay / 2);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 TimerTestThread4(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay / 4);
        TestHisto[arg]++;
    }
    return TEST_PASS;
}

static s32 TimerTestThreadOdd(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(arg & 0xFFFF);
        TestHisto[arg >> 16]++;
    }
    return TEST_PASS;
}

static s32 TimerTestBusyThread(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
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
    MostPrint("Timer Test 1\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, TimerTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, TimerTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter) test_pass = false;
    if (TestHisto[2] != exp_iter) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run "harmonic" timers
    //
    test_pass = true;
    MostPrint("Timer Test 2\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, TimerTestThread2, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, TimerTestThread4, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter * 2) test_pass = false;
    if (TestHisto[2] != exp_iter * 4) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run odd timers
    //
    test_pass = true;
    MostPrint("Timer Test 3\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThreadOdd, 13, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, TimerTestThreadOdd, 33 | 0x10000, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, TimerTestThreadOdd, 37 | 0x20000, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != (test_time / 13) + 1) test_pass = false;
    if (TestHisto[1] != (test_time / 33) + 1) test_pass = false;
    if (TestHisto[2] != (test_time / 37) + 1) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run two timers over busy thread
    //
    test_pass = true;
    MostPrint("Timer Test 4\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 1, TimerTestThread2, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 2, TimerTestBusyThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (TestHisto[1] != exp_iter * 2) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Run timer over two busy threads
    //
    test_pass = true;
    MostPrint("Timer Test 5\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, TimerTestBusyThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 2, TimerTestBusyThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[0] != exp_iter) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
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

static bool SemTests(void) {
    const u32 test_time = 5000;
    u32 exp_cnt = test_time / sem_test_delay;
    bool tests_all_pass = true;
    bool test_pass;
    //
    // Validate counts
    //
    test_pass = true;
    MostPrint("Sem Test 1\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(1, 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    MosGiveSem(&TestSem);  // Unblock thread to stop
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5 + 1)
        test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Post from interrupt and threads
    //
    test_pass = true;
    MostPrint("Sem Test 2\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(1, 1, SemTestPendIRQ, 1, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRx, 3, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    MosGiveSem(&TestSem); // Unblock thread to stop
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[3] != TestHisto[0] + TestHisto[2] + 1)
        test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Take Sem with Timeouts
    //
    test_pass = true;
    MostPrint("Sem Test 3\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(1, 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(4);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5)
        test_pass = false;
    // Add one for last timeout before thread stop
    if (TestHisto[3] != exp_cnt + 1) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // TrySem
    //
    test_pass = true;
    MostPrint("Sem Test 4\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(1, 1, SemTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRxTry, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (TestHisto[2] != TestHisto[0] + TestHisto[1] + 5)
        test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
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
    MostPrint("Queue Test 1\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    MosSendToQueue(&TestQueue, 2); // Unblock thread to stop
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2] + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Receive From Queue Timeout
    //
    test_pass = true;
    MostPrint("Queue Test 2\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(6);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2]) test_pass = false;
    if (TestHisto[5] != exp_cnt + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Send to Queue Timeout
    //   NOTE: The interrupt will only be able to queue the first entry since the
    //   thread will hog the queue.
    //
    test_pass = true;
    MostPrint("Queue Test 3\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTxTimeout, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRxSlow, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    // Give Thread 3 extra time to drain the queue
    MosDelayThread(queue_test_delay * (count_of(queue) + 1));
    MosRequestThreadStop(3);
    MosSendToQueue(&TestQueue, 2);
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[2] != exp_cnt) test_pass = false;
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[1] + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Queue
    //
    test_pass = true;
    MostPrint("Queue Test 4\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTx, 2, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRxTry, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    MosSendToQueue(&TestQueue, 2); // Unblock thread to stop
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[3] != TestHisto[0]) test_pass = false;
    if (TestHisto[4] != TestHisto[2] + 1) test_pass = false;
    if (TestQueue.head != TestQueue.tail) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

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
    MostPrint("Mux Test 1\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, MuxTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, MuxTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, MuxTestThreadRx, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    MosSendToQueue(&TestQueue, 2); // Unblock thread to stop
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1]) test_pass = false;
    if (TestHisto[4] != 1) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Wait on Mux with Timeout
    //
    test_pass = true;
    MostPrint("Mux Test 2\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, MuxTestThreadTx, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, MuxTestThreadTx, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, MuxTestThreadRxTimeout, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(5);
    if (TestHisto[2] != TestHisto[0]) test_pass = false;
    if (TestHisto[3] != TestHisto[1]) test_pass = false;
    if (TestHisto[4] != exp_cnt + 1) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

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
            if ((++count & 0xFFF) == 0) {
                // Give low priority thread chance to acquire mutex
                MosSendToQueue(&TestQueue, 0);
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
    MostPrint("Mutex Test 1\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 3, MutexTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, MutexTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Contention / Depth 2
    //
    test_pass = true;
    MostPrint("Mutex Test 2\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 3, MutexTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, MutexTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, MutexTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Priority Inheritance
    //
    test_pass = true;
    MostPrint("Mutex Test 3\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, MutexTestThread, MUTEX_TEST_PRIO_INHER,
                        Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, MutexDummyThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, MutexTestThread, 0, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    DisplayHistogram(6);
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Mutex (NOTE: may exhibit some non-deterministic behavior)
    //
    test_pass = true;
    MostPrint("Mutex Test 4\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 2, MutexTryTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, MutexTryTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Try Mutex 2 (NOTE: may exhibit some non-deterministic behavior)
    //
    test_pass = true;
    MostPrint("Mutex Test 5\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 2, MutexTryTestThread, 0, Stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, MutexTryTestThread, 1, Stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 2, MutexTryTestThread, 2, Stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

static bool HeapTests(void) {
    bool tests_all_pass = true;
    bool test_pass;
    MoshHeap TestHeapDesc;
    u32 rem;
    //
    // Allocate and Free of reserved block sizes
    //
    test_pass = true;
    MostPrint("Heap Test 1: Reserved block sizes\n");
    MoshInitHeap(&TestHeapDesc, TestHeap, sizeof(TestHeap));
    if (TestHeapDesc.max_bs != 0) test_pass = false;
    MoshReserveBlockSize(&TestHeapDesc, 128);
    if (TestHeapDesc.max_bs != 128) test_pass = false;
    MoshReserveBlockSize(&TestHeapDesc, 64);
    if (TestHeapDesc.max_bs != 128) test_pass = false;
    MoshReserveBlockSize(&TestHeapDesc, 256);
    if (TestHeapDesc.max_bs != 256) test_pass = false;
    // Check for sorted blocks
    // Check for alignment
    // TODO: Finish
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Allocate and Free of odd sized blocks
    //
    test_pass = true;
    MostPrint("Heap Test 2: Odd blocks\n");
    MoshInitHeap(&TestHeapDesc, TestHeap, sizeof(TestHeap));
    MoshReserveBlockSize(&TestHeapDesc, 64);
    MoshReserveBlockSize(&TestHeapDesc, 128);
    MoshReserveBlockSize(&TestHeapDesc, 256);
    if (TestHeapDesc.max_bs != 256) test_pass = false;
    rem = TestHeapDesc.bot - TestHeapDesc.pit + 16;
    MostPrintf("  Starting: %u bytes\n", rem);
    if (rem != sizeof(TestHeap)) test_pass = false;
    const u16 num_blocks = 5;
    void * blocks[num_blocks];
    for (u32 ix = 0; ix < num_blocks; ix++) {
        blocks[ix] = MoshAlloc(&TestHeapDesc, 257);
        if (blocks[ix] == NULL) test_pass = false;
        if ((u32)blocks[ix] % MOSH_HEAP_ALIGNMENT != 0) test_pass = false;
        if (!MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    }
    rem = TestHeapDesc.bot - TestHeapDesc.pit + 16;
    MostPrintf(" Remaining: %u bytes\n", rem);
    if (rem != sizeof(TestHeap) - (264 + 16) * num_blocks)
        test_pass = false;
    for (u32 ix = 0; ix < num_blocks; ix++) {
        MoshFree(&TestHeapDesc, blocks[ix]);
        if (MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    }
    for (u32 ix = 0; ix < num_blocks; ix++) {
        if (MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
        blocks[ix] = MoshAlloc(&TestHeapDesc, 257);
        if (blocks[ix] == NULL) test_pass = false;
        if ((u32)blocks[ix] % MOSH_HEAP_ALIGNMENT != 0) test_pass = false;
    }
    if (!MosIsListEmpty(&TestHeapDesc.osl)) test_pass = false;
    rem = TestHeapDesc.bot - TestHeapDesc.pit + 16;
    MostPrintf(" Remaining: %u bytes\n", rem);
    if (rem != sizeof(TestHeap) - (264 + 16) * num_blocks)
        test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    //
    // Allocation of several odd size blocks of differing sizes
    //

    //
    // Allocate and Free of short-lived blocks
    //
    test_pass = true;
    MostPrint("Heap Test 3: Short-lived blocks\n");
    u8 * fun[8];
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MoshAllocShortLived(&TestThreadHeapDesc, 64);
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        MoshFree(&TestThreadHeapDesc, fun[ix]);
    }
    if (test_pass) MostPrint(" Passed\n");
    else {
        MostPrint(" Failed\n");
        tests_all_pass = false;
    }
    return tests_all_pass;
}

typedef enum {
    CMD_ERR_NOT_FOUND = -2,
    CMD_ERR = -1,
    CMD_OK,
    CMD_OK_NO_HISTORY,
} CmdStatus;

static s32 CmdTest(s32 argc, char * argv[]) {
    bool test_pass = true;
    if (argc == 2) {
        if (strcmp(argv[1], "main") == 0) {
            if (ThreadTests() == false) test_pass = false;
            if (TimerTests() == false) test_pass = false;
            if (SemTests() == false) test_pass = false;
            if (QueueTests() == false) test_pass = false;
            if (MuxTests() == false) test_pass = false;
            if (MutexTests() == false) test_pass = false;
            if (HeapTests() == false) test_pass = false;
        } else if (strcmp(argv[1], "hal") == 0) {
            test_pass = HalTests(Stacks, DFT_STACK_SIZE);
        } else if (strcmp(argv[1], "thread") == 0) {
            test_pass = ThreadTests();
        } else if (strcmp(argv[1], "timer") == 0) {
            test_pass = TimerTests();
        } else if (strcmp(argv[1], "sem") == 0) {
            test_pass = SemTests();
        } else if (strcmp(argv[1], "queue") == 0) {
            test_pass = QueueTests();
        } else if (strcmp(argv[1], "mux") == 0) {
            test_pass = MuxTests();
        } else if (strcmp(argv[1], "mutex") == 0) {
            test_pass = MutexTests();
        } else if (strcmp(argv[1], "heap") == 0) {
            test_pass = HeapTests();
        } else return CMD_ERR_NOT_FOUND;
        if (test_pass) {
            MostPrint("Tests Passed\n");
            return CMD_OK;
        } else {
            MostPrint("Tests FAILED\n");
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
        if (PigeonFlag) MostPrintf("Incoming ---- .. .. %u .. . ------\n", cnt);
        cnt++;
    }
    return 0;
}

static s32 CmdPigeon(s32 argc, char * argv[]) {
    if (!PigeonFlag) {
        MosInitAndRunThread(PIGEON_THREAD_ID, 0, PigeonThread, 0,
                            PigeonStack, DFT_STACK_SIZE);
        PigeonFlag = 1;
    } else {
        MosKillThread(PIGEON_THREAD_ID);
        PigeonFlag = 0;
    }
    return CMD_OK;
}

static MostCmd cmd_list[] = {
    { CmdTest, "run", "Run Test", "[TEST]" },
    { CmdPigeon, "p", "Toggle Pigeon Printing", "" },
};

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
    strcpy(cmd_buf, cmd_buf_in);
    argc = MostParseCmd(argv, cmd_buf, MAX_CMD_ARGUMENTS);
    MostCmd * cmd = MostFindCmd(argv[0], cmd_list, count_of(cmd_list));
    if (cmd) {
        return (CmdStatus)cmd->func(argc, argv);
    } else if (argv[0][0] == '!') {
        if (argv[0][1] == '!') {
            u32 run_cmd_ix = CalcOffsetCmdIx(CmdIx, CmdMaxIx, -1);
            strcpy(CmdBuffers[CmdIx], CmdBuffers[run_cmd_ix]);
            return (CmdStatus)RunCmd(CmdBuffers[CmdIx]);
        } else if (argv[0][1] == '-') {
            if (argv[0][2] >= '1' && argv[0][2] <= '9') {
                u32 run_cmd_ix = CalcOffsetCmdIx(CmdIx, CmdMaxIx,
                                                 -(s8)(argv[0][2] - '0'));
                strcpy(CmdBuffers[CmdIx], CmdBuffers[run_cmd_ix]);
                return (CmdStatus)RunCmd(CmdBuffers[CmdIx]);
            }
        }
    } else if (strcmp(argv[0], "?") == 0 || strcmp(argv[0], "help") == 0) {
        MostPrintHelp(cmd_list, count_of(cmd_list));
        MostPrint("!!: Repeat prior command\n");
        MostPrint("!-#: Repeat #th prior command\n");
        MostPrint("h -or- history: Display command history\n");
        MostPrint("? -or- help: Display command help\n");\
        return CMD_OK_NO_HISTORY;
    } else if (strcmp(argv[0], "h") == 0 || strcmp(argv[0], "history") == 0) {
        for (s32 ix = CmdMaxIx; ix > 0; ix--) {
            u32 hist_cmd_ix = CalcOffsetCmdIx(CmdIx, CmdMaxIx, -ix);
            MostTakeMutex();
            MostPrintf("%2d: ", -ix);
            MostPrint(CmdBuffers[hist_cmd_ix]);
            MostPrint("\n");
            MostGiveMutex();
        }
        return CMD_OK_NO_HISTORY;
    } else if (argv[0][0] == '\0') {
        return CMD_OK_NO_HISTORY;
    }
    return CMD_ERR_NOT_FOUND;
}

static s32 TestShell(s32 arg) {
    while (1) {
        MostCmdResult result;
        CmdStatus status;
        result = MostGetNextCmd("# ", CmdBuffers[CmdIx], MAX_CMD_LINE_SIZE);
        switch (result) {
        case MOST_CMD_RECEIVED:
            status = RunCmd(CmdBuffers[CmdIx]);
            switch (status) {
            case CMD_OK_NO_HISTORY:
                break;
            case CMD_ERR_NOT_FOUND:
                MostPrint("[ERR] Command not found...\n");
                break;
            case CMD_OK:
                MostPrint("[OK]\n");
                if (++CmdIx == MAX_CMD_BUFFER_LENGTH) CmdIx = 0;
                if (CmdIx > CmdMaxIx) CmdMaxIx = CmdIx;
                break;
            default:
            case CMD_ERR:
                MostPrint("[ERR]\n");
                if (++CmdIx == MAX_CMD_BUFFER_LENGTH) CmdIx = 0;
                if (CmdIx > CmdMaxIx) CmdMaxIx = CmdIx;
                break;
            }
            CmdHistoryIx = CmdIx;
            CmdBuffers[CmdIx][0] = '\0';
            break;
        case MOST_CMD_UP_ARROW:
            // Rotate history back one, skipping over current index
            CmdHistoryIx = CalcOffsetCmdIx(CmdHistoryIx, CmdMaxIx, -1);
            if (CmdHistoryIx == CmdIx)
                CmdHistoryIx = CalcOffsetCmdIx(CmdHistoryIx, CmdMaxIx, -1);
            strcpy(CmdBuffers[CmdIx], CmdBuffers[CmdHistoryIx]);
            break;
        case MOST_CMD_DOWN_ARROW:
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

int main() {
    HalInit();
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);

    MosInit();
    MostInit(TRACE_INFO | TRACE_ERROR | TRACE_FATAL, true);
    MostPrintf("\nWelcome to Maintainable OS (Version %s)\n", MosGetParams()->version);

    MoshInitHeap(&TestThreadHeapDesc, TestThreadHeap, sizeof(TestThreadHeap));
    MoshReserveBlockSize(&TestThreadHeapDesc, 1024);
    MoshReserveBlockSize(&TestThreadHeapDesc, 512);
    MoshReserveBlockSize(&TestThreadHeapDesc, 256);
    MoshReserveBlockSize(&TestThreadHeapDesc, 128);
    MoshReserveBlockSize(&TestThreadHeapDesc, 64);
    Stacks[0] = MoshAlloc(&TestThreadHeapDesc, TEST_SHELL_STACK_SIZE);
    if (Stacks[0] == NULL) {
        MostPrint("Stack allocation error\n");
        return -1;
    }
    PigeonStack = MoshAlloc(&TestThreadHeapDesc, DFT_STACK_SIZE);
    if (PigeonStack == NULL) {
        MostPrint("Stack allocation error\n");
        return -1;
    }
    for (u32 id = 1; id < count_of(Stacks); id++) {
        Stacks[id] = MoshAllocBlock(&TestThreadHeapDesc, DFT_STACK_SIZE);
        if (Stacks[id] == NULL) {
            MostPrint("Stack allocation error\n");
            return -1;
        }
    }
    if (MAX_TEST_THREADS > MOS_MAX_APP_THREADS) {
        MostPrint("Mos config error: not enough threads\n");
        return -1;
    }
    MosInitAndRunThread(TEST_SHELL_THREAD_ID, 0, TestShell, 0,
                        Stacks[0], TEST_SHELL_STACK_SIZE);
    MosRunScheduler();
    return -1;
}
