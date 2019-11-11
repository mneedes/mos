
//  Copyright 2019 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  The HAL TB is intended for analyzing performance using a logic analyzer.
//    It uses the HAL primarily for toggling GPIOs.
//

#include "stm32f4xx_hal.h"
#include "stm32f4_discovery.h"
#include "hal.h"
#include "mos.h"
#include "hal_tb.h"

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

#define RUN_TEST    3

void HalTests(u8 *stacks[], u32 stack_size) {
    const u32 test_time = 5000;
    bool test_pass;
#if RUN_TEST == 1
    //
    // Run two timers over busy thread
    //
    test_pass = true;
    MostPrint("Hal Timer Test\n");
    MosInitAndRunThread(1, 1, TimerTestThread, 0, stacks[1], stack_size);
    MosInitAndRunThread(2, 1, TimerTestThread2, 1, stacks[2], stack_size);
    MosInitAndRunThread(3, 2, TimerTestBusyThread, 2, stacks[3], stack_size);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
#elif RUN_TEST == 2
    //
    // Run timer over two busy threads
    //
    test_pass = true;
    MostPrint("Hal Timer Test 2\n");
    MosInitAndRunThread(1, 1, TimerTestThread, 0, stacks[1], stack_size);
    MosInitAndRunThread(2, 2, TimerTestBusyThread, 1, stacks[2], stack_size);
    MosInitAndRunThread(3, 2, TimerTestBusyThread, 2, stacks[3], stack_size);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
#elif RUN_TEST == 3
    //
    // Run odd timers
    //
    test_pass = true;
    MostPrint("Hal Timer Test 3\n");
    MosInitAndRunThread(1, 1, TimerTestThreadOdd, 33, stacks[1], stack_size);
    MosInitAndRunThread(2, 2, TimerTestThreadOdd, 13 | 0x10000, stacks[2], stack_size);
    MosInitAndRunThread(3, 3, TimerTestThreadOdd, 37 | 0x20000, stacks[3], stack_size);
    MosDelayThread(test_time);
    MosRequestThreadStop(1);
    MosRequestThreadStop(2);
    MosRequestThreadStop(3);
    if (MosWaitForThreadStop(1) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(2) != TEST_PASS) test_pass = false;
    if (MosWaitForThreadStop(3) != TEST_PASS) test_pass = false;
    if (test_pass) MostPrint(" Passed\n");
    else MostPrint(" Failed\n");
#endif
}
