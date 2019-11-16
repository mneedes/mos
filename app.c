
//  Copyright 2019 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Test Bench
//

#include <errno.h>

#include "hal.h"
#include "mos.h"
#include "mosh.h"
#include "most.h"

#define DFT_STACK_SIZE           256
#define MAX_TEST_SUB_THREADS     (MOS_MAX_APP_THREADS - 1)
#define MAX_TEST_THREADS         (MAX_TEST_SUB_THREADS + 1)

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
static u8 * stacks[MAX_TEST_THREADS];
static MoshHeap TestHeapDesc;
static u8 MOSH_HEAP_ALIGNED TestHeap[2048];

// Generic flag for tests
static volatile u32 TestFlag = 0;

// Generic histogram for tests
#define MAX_HISTO_ENTRIES   102
static volatile u32 TestHisto[MAX_HISTO_ENTRIES];

// Test Sem / Mutex / WaitMux
static MosSem TestSem;
static MosMutex TestMutex;
static MosWaitMux TestMux;

// Test Message Queue
static u32 queue[4];
static MosQueue TestQueue;

// Buffer for printing messages
static char print_buf[128];

static void ClearHistogram(void) {
    for (u32 ix = 0; ix < count_of(TestHisto); ix++)
        TestHisto[ix] = 0;
}

static void DisplayHistogram(u32 cnt) {
    for (u32 ix = 0; ix < cnt; ix++)
        MostPrintf(print_buf, " Histo[%u] = %u\n", ix, TestHisto[ix]);
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

static void ThreadTests(void) {
    const u32 test_time = 5000;
    u32 exp_iter = test_time / pri_test_delay;
    bool test_pass;
    //
    // Highest priorities must starve lowest
    //
    test_pass = true;
    MostPrint("Priority Test 1\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, PriTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, PriTestThread, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, PriTestThread, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Change of priority
    //
    test_pass = true;
    MostPrint("Priority Test 2\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, PriTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, PriTestThread, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, PriTestThread, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
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
    MosInitAndRunThread(1, 1, KillTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosDelayThread(10);
    MosKillThread(1);
    if (MosWaitForThreadStop(1) != TEST_PASS_HANDLER) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
    //
    // Kill Thread using Supplied Handler
    //
    test_pass = true;
    MostPrint("Kill Test 2\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(1, 1, KillTestThread, 1, stacks[1], DFT_STACK_SIZE);
    MosDelayThread(10);
    MosKillThread(1);
    if (MosWaitForThreadStop(1) != TEST_PASS_HANDLER) test_pass = false;
    if (TestMutex.owner != -1) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
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
static void TimerTests(void) {
    const u32 test_time = 5000;
    u32 exp_iter = test_time / timer_test_delay;
    bool test_pass;
    //
    // Run uniform timers
    //
    test_pass = true;
    MostPrint("Timer Test 1\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, TimerTestThread, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, TimerTestThread, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Run "harmonic" timers
    //
    test_pass = true;
    MostPrint("Timer Test 2\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, TimerTestThread2, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, TimerTestThread4, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Run odd timers
    //
    test_pass = true;
    MostPrint("Timer Test 3\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThreadOdd, 13, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, TimerTestThreadOdd, 33 | 0x10000, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, TimerTestThreadOdd, 37 | 0x20000, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Run two timers over busy thread
    //
    test_pass = true;
    MostPrint("Timer Test 4\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 1, TimerTestThread2, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 2, TimerTestBusyThread, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Run timer over two busy threads
    //
    test_pass = true;
    MostPrint("Timer Test 5\n");
    ClearHistogram();
    MosInitAndRunThread(1, 1, TimerTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, TimerTestBusyThread, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 2, TimerTestBusyThread, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
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
        if (MosTakeSemOrTO(&TestSem, sem_test_delay / 2 + 1)) {
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

static void SemTests(void) {
    const u32 test_time = 5000;
    u32 exp_cnt = test_time / sem_test_delay;
    bool test_pass;
    //
    // Validate counts
    //
    test_pass = true;
    MostPrint("Sem Test 1\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(1, 1, SemTestThreadTx, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRx, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Post from interrupt and threads
    //
    test_pass = true;
    MostPrint("Sem Test 2\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitAndRunThread(1, 1, SemTestPendIRQ, 1, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 2, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRx, 3, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Take Sem with Timeouts
    //
    test_pass = true;
    MostPrint("Sem Test 3\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(1, 1, SemTestThreadTx, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRxTimeout, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // TrySem
    //
    test_pass = true;
    MostPrint("Sem Test 4\n");
    ClearHistogram();
    MosInitSem(&TestSem, 5);
    MosInitAndRunThread(1, 1, SemTestThreadTx, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, SemTestThreadTx, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, SemTestThreadRxTry, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
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
                               queue_test_delay / 2 + 1)) {
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
                                    queue_test_delay / 2 + 1)) {
            TestHisto[arg + val]++;
        } else {
            TestHisto[arg + 3]++;
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static void QueueTests(void) {
    const u32 test_time = 5000;
    u32 exp_cnt = test_time / queue_test_delay;
    bool test_pass;
    //
    // Post from interrupts and threads
    //
    test_pass = true;
    MostPrint("Queue Test 1\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTx, 2, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRx, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Receive From Queue Timeout
    //
    test_pass = true;
    MostPrint("Queue Test 2\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTx, 2, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRxTimeout, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Send to Queue Timeout
    //   NOTE: The interrupt will only be able to queue the first entry since the
    //   thread will hog the queue.
    //
    test_pass = true;
    MostPrint("Queue Test 3\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTxTimeout, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRxSlow, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Try Queue
    //
    test_pass = true;
    MostPrint("Queue Test 4\n");
    ClearHistogram();
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, QueueTestPendIRQ, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, QueueTestThreadTx, 2, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, QueueTestThreadRxTry, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
}

//
// WaitMux Testing
//

static const u32 mux_test_delay = 50;

static s32 WaitMuxTestThreadTx(s32 arg) {
    for (;;) {
        if (arg == 0) MosGiveSem(&TestSem);
        else if (arg == 1) MosSendToQueue(&TestQueue, 1);
        TestHisto[arg]++;
        MosDelayThread(mux_test_delay);
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 WaitMuxTestThreadRx(s32 arg) {
    MosWaitMuxEntry mux[2];
    mux[0].type = MOS_WAIT_SEM;
    mux[0].ptr.sem = &TestSem;
    mux[1].type = MOS_WAIT_RECV_QUEUE;
    mux[1].ptr.q = &TestQueue;
    MosInitWaitMux(&TestMux);
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

static s32 WaitMuxTestThreadRxTimeout(s32 arg) {
    MosWaitMuxEntry mux[2];
    mux[0].type = MOS_WAIT_SEM;
    mux[0].ptr.sem = &TestSem;
    mux[1].type = MOS_WAIT_RECV_QUEUE;
    mux[1].ptr.q = &TestQueue;
    MosInitWaitMux(&TestMux);
    MosSetActiveMux(&TestMux, mux, count_of(mux));
    for (;;) {
        u32 idx;
        if (MosWaitOnMuxOrTO(&TestMux, &idx, mux_test_delay / 2 + 1)) {
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

static void WaitMuxTests(void) {
    const u32 test_time = 5000;
    u32 exp_cnt = test_time / mux_test_delay;
    bool test_pass;
    //
    // Wait on Mux, thread and semaphore
    //
    test_pass = true;
    MostPrint("WaitMux Test 1\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, WaitMuxTestThreadTx, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, WaitMuxTestThreadTx, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, WaitMuxTestThreadRx, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
    //
    // Wait on Mux with Timeout
    //
    test_pass = true;
    MostPrint("WaitMux Test 2\n");
    ClearHistogram();
    MosInitSem(&TestSem, 0);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, WaitMuxTestThreadTx, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, WaitMuxTestThreadTx, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, WaitMuxTestThreadRxTimeout, 2, stacks[3], DFT_STACK_SIZE);
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
    else MostPrint(" Failed\n");
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

static void MutexTests(void) {
    bool test_pass;
    //
    // Contention / Depth 1
    //
    test_pass = true;
    MostPrint("Mutex Test 1\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 3, MutexTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, MutexTestThread, 1, stacks[2], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
    //
    // Contention / Depth 2
    //
    test_pass = true;
    MostPrint("Mutex Test 2\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 3, MutexTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 3, MutexTestThread, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, MutexTestThread, 2, stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
    //
    // Priority Inheritance
    //
    test_pass = true;
    MostPrint("Mutex Test 3\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitQueue(&TestQueue, queue, count_of(queue));
    MosInitAndRunThread(1, 1, MutexTestThread, MUTEX_TEST_PRIO_INHER,
                        stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, MutexDummyThread, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 3, MutexTestThread, 0, stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    DisplayHistogram(6);
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
    //
    // Try Mutex (NOTE: may exhibit some non-deterministic behavior)
    //
    test_pass = true;
    MostPrint("Mutex Test 4\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 2, MutexTryTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, MutexTryTestThread, 1, stacks[2], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    DisplayHistogram(2);
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
    //
    // Try Mutex 2 (NOTE: may exhibit some non-deterministic behavior)
    //
    test_pass = true;
    MostPrint("Mutex Test 5\n");
    ClearHistogram();
    MosInitMutex(&TestMutex);
    MosInitAndRunThread(1, 2, MutexTryTestThread, 0, stacks[1], DFT_STACK_SIZE);
    MosInitAndRunThread(2, 2, MutexTryTestThread, 1, stacks[2], DFT_STACK_SIZE);
    MosInitAndRunThread(3, 2, MutexTryTestThread, 2, stacks[3], DFT_STACK_SIZE);
    MosDelayThread(5000);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    DisplayHistogram(3);
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
}

static void HeapTests(void) {

    //
    //  Allocate and Free of reserved blocks
    //

    //
    //  Allocate and "Free" of forever blocks
    //

    //
    //  Allocate and Free of short-lived blocks
    //

    u8 * fun[8];
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        fun[ix] = MoshAllocShortLived(&TestHeapDesc, 64);
    }
    for (u32 ix = 0; ix < count_of(fun); ix++) {
        MoshFreeShortLived(&TestHeapDesc, fun[ix]);
    }
}

#define USING_LOGIC_ANALYZER  false

s32 TestExecThread(s32 arg) {
    MostPrint("\nStarting Tests\n");
#if (USING_LOGIC_ANALYZER == false)
    // General testbench
    ThreadTests();
    TimerTests();
    SemTests();
    QueueTests();
    WaitMuxTests();
    MutexTests();
    //HeapTests();
#else
    HalTests(stacks, DFT_STACK_SIZE);
#endif
    MostPrint("Tests Complete\n");
    return 0;
}

int main() {
    HalInit();
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);

    MosInit();
    MostInit(TRACE_INFO | TRACE_ERROR | TRACE_FATAL);

    MoshInitHeap(&TestHeapDesc, TestHeap, sizeof(TestHeap));
    MoshReserveBlockSize(&TestHeapDesc, 256);
    MoshReserveBlockSize(&TestHeapDesc, 128);
    MoshReserveBlockSize(&TestHeapDesc, 512);
    for (u32 id = 0; id < count_of(stacks); id++) {
        stacks[id] = MoshAllocBlock(&TestHeapDesc, DFT_STACK_SIZE);
        if (stacks[id] == NULL) {
            MostPrint("Stack allocation error\n");
            return -1;
        }
    }

    if (MAX_TEST_THREADS > MOS_MAX_APP_THREADS) {
        MostPrint("Mos config error: not enough threads\n");
        return -1;
    }

    MosInitAndRunThread(MAX_TEST_THREADS, 0, TestExecThread, 0, stacks[0], 256);
    MosRunScheduler();
    return -1;
}
