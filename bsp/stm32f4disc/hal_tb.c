
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  The HAL TB is intended for analyzing performance using a logic analyzer.
//    It uses the HAL primarily for toggling GPIOs.
//

#include <mos/kernel.h>
#include <mos/thread_heap.h>
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

static u32 pulse_counter = 0;

void EXTI0_IRQHandler(void) {
    IRQ0_Callback();
}

void EXTI1_IRQHandler(void) {
    IRQ1_Callback();
}

void HalTestsInit(void) {
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);
}

void HalTestsTriggerInterrupt(u32 num) {
    switch (num) {
    case 0:
        NVIC_SetPendingIRQ(EXTI0_IRQn);
        break;
    case 1:
        NVIC_SetPendingIRQ(EXTI1_IRQn);
        break;
    default:
        break;
    }
}

static s32 HalRandomPulseThread(s32 arg) {
    pulse_counter = 0;
    for (u32 ix = 0; ix < arg; ix++) {
        u32 rn = HalGetRandomU32();
        MosDisableInterrupts();
        LED_ON(0);
        MosDelayMicroSec(8 + (rn & 0x1F));
        LED_OFF(0);
        MosEnableInterrupts();
        pulse_counter++;
        if ((pulse_counter % (1 << 12)) == 0) {
            MosPrintf("Pulses: %08x\n", pulse_counter);
        }
        MosDelayMicroSec(800 + (rn >> 23));
    }
    MosPrintf("Total Pulses: %08x\n", pulse_counter);
    return TEST_PASS;
}

bool HalTests(int argc, char * argv[]) {
    bool test_pass;
    test_pass = true;
    MosPrint("Hal Pulser Test\n");
    MosThread * thread = {0};
    MosAllocAndRunThread(&thread, 1, HalRandomPulseThread, 14000000, 256);
    if (!thread) test_pass = false;
    else {
        if (MosWaitForThreadStop(thread) != TEST_PASS) test_pass = false;
        MosDecThreadRefCount(&thread);
    }
    return test_pass;
}
