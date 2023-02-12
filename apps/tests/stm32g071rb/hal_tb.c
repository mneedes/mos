
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  The HAL TB is intended for analyzing performance using a logic analyzer.
//    It uses the HAL primarily for toggling GPIOs.
//

#include <string.h>

#include <mos/kernel.h>
#include <mos/thread_heap.h>
#include <mos/trace.h>

#include <hal_tb.h>

static MosSem pulse_sem;
static u32 pulse_counter;

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
#if 0
    NVIC_SetPriority(EXTI15_10_IRQn, 0);
    NVIC_EnableIRQ(EXTI15_10_IRQn);
#endif
    for (;;) {
    	mosWaitForSem(&pulse_sem);
        pulse_counter++;
        if ((pulse_counter % (1 << 12)) == 0) {
            mosPrintf("Pulses: %08x\n", pulse_counter);
        }
    }
    return TEST_FAIL;
}

static MosThread * thread = { 0 };

void EXTI0_1_IRQHandler(void) {
    IRQ0_Callback();
}

void EXTI2_3_IRQHandler(void) {
    IRQ1_Callback();
}

void HalTestsInit(void) {
    NVIC_EnableIRQ(EXTI0_1_IRQn);
    NVIC_EnableIRQ(EXTI2_3_IRQn);
}

void HalTestsTriggerInterrupt(u32 num) {
    switch (num) {
    case 0:
        NVIC_SetPendingIRQ(EXTI0_1_IRQn);
        break;
    case 1:
        NVIC_SetPendingIRQ(EXTI2_3_IRQn);
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
	if (strcmp(argv[0], "start") == 0) {
		if (mosAllocAndRunThread(&thread, 0, HalPulseReceiverThread, 0, 512)) {
		    mosPrint("Hal Pulse Receiver Test START\n");
		}
	    if (!thread) success = false;
	} else if (strcmp(argv[0], "stop") == 0) {
		if (thread == NULL) {
			success = false;
		} else {
            mosKillThread(thread);
            if (mosWaitForThreadStop(thread) != TEST_PASS) success = false;
            mosDecThreadRefCount(&thread);
            mosPrint("Hal Pulse Receiver Test STOP\n");
		}
	}
    return success;
}
