
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  The HAL TB is intended for analyzing performance using a logic analyzer.
//    It uses the HAL primarily for toggling GPIOs.
//

#include <mos/kernel.h>
#include <mos/trace.h>
#include <bsp/hal_tb.h>

typedef enum {
    TEST_PASS        = 0xba5eba11,
    TEST_FAIL        = 0xdeadbeef,
} TestStatus;

// GPIOs are mapped to pin 12 through 15 on STM32F4 Discovery
#define GPIO_BASE   0x40020C00
#define LED_ON(x)   (MOS_VOL_U32(GPIO_BASE + 24) = (1 << (12 + (x))))
#define LED_OFF(x)  (MOS_VOL_U32(GPIO_BASE + 24) = (1 << (12 + (x))) << 16)

// Make delay a multiple of 4
static const u32 timer_test_delay = 100;

// Using LED pins as GPIO for logic analyzer
void HalSetGpio(u32 num, bool value) {
    if (value) LED_ON(num);
    else LED_OFF(num);
}

static s32 TimerTestThread(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay);
        LED_ON(0);
        MosDelayMicroSec(10);
        LED_OFF(0);
    }
    return TEST_PASS;
}

static s32 TimerTestThread2(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(timer_test_delay / 2);
        LED_ON(1);
        MosDelayMicroSec(10);
        LED_OFF(1);
    }
    return TEST_PASS;
}

static s32 TimerTestThreadOdd(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
        MosDelayThread(arg & 0xFFFF);
        LED_ON(arg >> 16);
        MosDelayMicroSec(10);
        LED_OFF(arg >> 16);
    }
    return TEST_PASS;
}

static s32 TimerTestBusyThread(s32 arg) {
    for (;;) {
        if (MosIsStopRequested()) break;
    }
    return TEST_PASS;
}

static s32 HalRandomPulseThread(s32 arg) {
    for (;;) {
        u32 rn = HalGetRandomU32();
        MosDisableInterrupts();
        LED_ON(0);
        MosDelayMicroSec(8 + (rn & 0x1F));
        LED_OFF(0);
        MosEnableInterrupts();
        if (MosIsStopRequested()) break;
        MosDelayMicroSec(800 + (rn >> 23));
    }
    return TEST_PASS;
}

#define RUN_TEST    5

bool HalTests(MosThread * threads[], u32 max_threads, u8 * stacks[], u32 stack_size) {
    const u32 test_time = 5000;
    bool test_pass;
#if RUN_TEST == 1
    //
    // Run two timers over busy thread
    //
    test_pass = true;
    MosPrint("Hal Timer Test\n");
    MosInitAndRunThread(threads[1], 1, TimerTestThread, 0, stacks[1], stack_size);
    MosInitAndRunThread(threads[2], 1, TimerTestThread2, 1, stacks[2], stack_size);
    MosInitAndRunThread(threads[3], 2, TimerTestBusyThread, 2, stacks[3], stack_size);
    MosDelayThread(test_time);
    MosRequestThreadStop(threads[1]);
    MosRequestThreadStop(threads[2]);
    MosRequestThreadStop(threads[3]);
    if (MosWaitForThreadStop(threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(threads[3]) != TEST_PASS) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else MosPrint(" Failed\n");
#elif RUN_TEST == 2
    //
    // Run timer over two busy threads
    //
    test_pass = true;
    MosPrint("Hal Timer Test 2\n");
    MosInitAndRunThread(threads[1], 1, TimerTestThread, 0, stacks[1], stack_size);
    MosInitAndRunThread(threads[2], 2, TimerTestBusyThread, 1, stacks[2], stack_size);
    MosInitAndRunThread(threads[3], 2, TimerTestBusyThread, 2, stacks[3], stack_size);
    MosDelayThread(test_time);
    MosRequestThreadStop(threads[1]);
    MosRequestThreadStop(threads[2]);
    MosRequestThreadStop(threads[3]);
    if (MosWaitForThreadStop(threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(threads[3]) != TEST_PASS) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else MosPrint(" Failed\n");
#elif RUN_TEST == 3
    //
    // Run odd timers
    //
    test_pass = true;
    MosPrint("Hal Timer Test 3\n");
    MosInitAndRunThread(threads[1], 1, TimerTestThreadOdd, 33, stacks[1], stack_size);
    MosInitAndRunThread(threads[2], 2, TimerTestThreadOdd, 13 | 0x10000, stacks[2], stack_size);
    MosInitAndRunThread(threads[3], 3, TimerTestThreadOdd, 37 | 0x20000, stacks[3], stack_size);
    MosDelayThread(test_time);
    MosRequestThreadStop(threads[1]);
    MosRequestThreadStop(threads[2]);
    MosRequestThreadStop(threads[3]);
    if (MosWaitForThreadStop(threads[1]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(threads[2]) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(threads[3]) != TEST_PASS) test_pass = false;
    if (test_pass) MosPrint(" Passed\n");
    else MosPrint(" Failed\n");
#elif RUN_TEST == 4
    //
    // Spin up max threads
    //
    test_pass = true;
    MosPrint("Hal Timer Test 4\n");
    for (u32 ix = 1; ix <= max_threads - 2; ix++)
        MosInitAndRunThread(threads[ix], 1, TimerTestBusyThread, ix, stacks[ix], stack_size);
    MosDelayThread(test_time);
    for (u32 ix = 1; ix <= max_threads - 2; ix++) {
        MosRequestThreadStop(threads[ix]);
        if (MosWaitForThreadStop(threads[ix]) != TEST_PASS) test_pass = false;
    }
    if (test_pass) MosPrint(" Passed\n");
    else MosPrint(" Failed\n");
#elif RUN_TEST == 5
    test_pass = true;
    MosPrint("Hal RNG Test\n");
    MosInitAndRunThread(threads[1], 1, HalRandomPulseThread, 0, stacks[1], stack_size);
    MosDelayThread(test_time);
    MosRequestThreadStop(threads[1]);
    if (MosWaitForThreadStop(threads[1]) != TEST_PASS) test_pass = false;
#endif
    return test_pass;
}
