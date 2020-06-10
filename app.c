
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Application Entry
//

#include <errno.h>
#include <string.h>

#include "hal.h"
#include "mos/kernel.h"
#include "mos/trace.h"
#include "tb.h"

int main() {
    // Initialize hardware, set up SysTick, NVIC priority groups, etc.
    HalInit();

    // Run init before calling any MOS functions
    MosInit();

    // Init trace before calling any print functions
    MosInitTrace(TRACE_INFO | TRACE_ERROR | TRACE_FATAL, true);
    MosPrintf("\nMaintainable OS (Version %s)\n", MosGetParams()->version);

    // Initialize and Run test bench example Application.
    if (InitTestBench() == 0) {
        // Start multitasking, running App threads.
        MosRunScheduler();
    }
    return -1;
}
