
//  Copyright 2019-2022 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// The HAL TB is intended for analyzing performance using a logic analyzer.
//   It uses the HAL primarily for toggling GPIOs.
//

#include <string.h>

#include <mos/kernel.h>
#include <mos/trace.h>
#include <hal_tb.h>

static MosSem pulse_sem;
static u32 pulse_counter;
static bool stop_thread;

void EXTI15_10_IRQHandler(void) {
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_13);
    mosIncrementSem(&pulse_sem);
}

static s32 HalPulseReceiverTermHandler(s32 arg) {
    mosPrintf("Total Received Pulses: %08x\n", pulse_counter);
    return arg;
}

static s32 HalPulseReceiverThread(s32 arg) {
    MOS_UNUSED(arg);
    pulse_counter = 0;
    mosInitSem(&pulse_sem, 0);
    mosSetTermHandler(mosGetRunningThread(), HalPulseReceiverTermHandler, TEST_PASS);
    // Set interrupt to high priority (higher than scheduler at least)
    NVIC_SetPriority(EXTI15_10_IRQn, 0);
    NVIC_EnableIRQ(EXTI15_10_IRQn);
    for (;;) {
        mosWaitForSem(&pulse_sem);
        pulse_counter++;
        if ((pulse_counter % (1 << 12)) == 0) {
            mosPrintf("Pulses: %08x\n", pulse_counter);
            if (stop_thread) break;
        }
    }
    return TEST_PASS;
}

static s32 HalRandomPulseThread(s32 arg) {
    pulse_counter = 0;
    for (u32 ix = 0; ix < arg; ix++) {
        u32 rn = HalGetRandomU32();
        u32 mask = mosDisableInterrupts();
        HalSetGpio(0, true);
        mosDelayMicroseconds(8 + (rn & 0x1F));
        HalSetGpio(0, false);
        mosEnableInterrupts(mask);
        pulse_counter++;
        if ((pulse_counter % (1 << 12)) == 0) {
            mosPrintf("Pulses: %08x\n", pulse_counter);
        }
        mosDelayMicroseconds(800 + (rn >> 23));
        if (stop_thread) break;
    }
    mosPrintf("Total Pulses: %08x\n", pulse_counter);
    return TEST_PASS;
}

static MosThread * pThread = { 0 };

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

bool HalTests(int argc, char * argv[]) {
    if (argc == 0) {
        mosPrint("Not enough arguments\n");
        return false;
    }
    bool success = true;
    if (strcmp(argv[0], "rxstart") == 0) {
        stop_thread = false;
        if (mosAllocAndRunThread(&pThread, 0, HalPulseReceiverThread, 0, 512)) {
            mosPrint("Hal Pulse Receiver Test START\n");
        }
        if (!pThread) success = false;
    } else if (strcmp(argv[0], "txstart") == 0) {
        stop_thread = false;
        if (mosAllocAndRunThread(&pThread, 0, HalRandomPulseThread, 0x1000000, 512)) {
            mosPrint("Hal Pulse Transmitter Test START\n");
        }
        if (!pThread) success = false;
    } else if (strcmp(argv[0], "stop") == 0) {
        if (pThread == NULL) {
            success = false;
        } else {
            stop_thread = true;
            if (mosWaitForThreadStop(pThread) != TEST_PASS) success = false;
            mosDecThreadRefCount(&pThread);
            mosPrint("Hal Pulse Test STOP\n");
        }
    }
    return success;
}
