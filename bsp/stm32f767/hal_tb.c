
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

#if 0
// GPIOs are mapped to pin 12 through 15 on STM32F4 Discovery
#define GPIO_BASE   0x40020C00
#define LED_ON(x)   (MOS_VOL_U32(GPIO_BASE + 24) = (1 << (12 + (x))))
#define LED_OFF(x)  (MOS_VOL_U32(GPIO_BASE + 24) = (1 << (12 + (x))) << 16)

// Using LED pins as GPIO for logic analyzer
void HalSetGpio(u32 num, bool value) {
    if (value) LED_ON(num);
    else LED_OFF(num);
}
#endif

static MosSem pulse_sem;
static u32 pulse_counter;

void EXTI15_10_IRQHandler(void) {
	__HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_13);
	MosGiveSem(&pulse_sem);
}

static s32 HalPulseReceiverStopHandler(s32 arg) {
    MosPrintf("Total Received Pulses: %08x\n", pulse_counter);
    return arg;
}

static s32 HalPulseReceiverThread(s32 arg) {
    pulse_counter = 0;
    MosInitSem(&pulse_sem, 0);
    MosSetStopHandler(MosGetThread(), HalPulseReceiverStopHandler, TEST_PASS);
    NVIC_EnableIRQ(EXTI15_10_IRQn);
    for (;;) {
    	MosTakeSem(&pulse_sem);
        pulse_counter++;
        if ((pulse_counter % (1 << 12)) == 0) {
            MosPrintf("Pulses: %08x\n", pulse_counter);
        }
        if (MosIsStopRequested()) break;
    }
    return TEST_FAIL;
}

bool HalTests(int argc, char * argv[]) {
    const u32 test_time = 60*60*1000;
    bool test_pass;
    test_pass = true;
    MosPrint("Hal Pulse Receiver Test\n");
    MosThread * thread = {0};
    MosAllocAndRunThread(&thread, 1, HalPulseReceiverThread, 0, 512);
    if (!thread) test_pass = false;
    else {
        MosDelayThread(test_time);
        MosKillThread(thread);
        if (MosWaitForThreadStop(thread) != TEST_PASS) test_pass = false;
        MosDecThreadRefCount(&thread);
    }
    return test_pass;
}
