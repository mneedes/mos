
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Testbench Application Entry
//

#include <mos/hal.h>
#include <mos/kernel.h>
#include <mos/trace.h>
#include <bsp_hal.h>

#include "tb.h"

int main() {
    // Initialize hardware, set up SysTick, NVIC priority groups, etc.
    HalInit();

#if 0
    // Subpriority group testing
    u32 subpri_group_num = 4;
    u32 tmp = (MOS_REG(AIRCR) & MOS_REG_VALUE(AIRCR_MASK)) | MOS_REG_VALUE(VECTKEY);
    tmp |= (subpri_group_num << 8);
    MOS_REG(AIRCR) = tmp;
#endif

    // Run init before calling any MOS functions
    mosInit(0);

    // Init trace before calling any print functions
    mosInitTrace(TRACE_INFO | TRACE_ERROR | TRACE_FATAL, true);
    mosPrintf("\nMaintainable OS (Version " MOS_VERSION_STRING ")\n");
    mosPrint("Copyright 2019-2023, Matthew Needes  All Rights Reserved\n");

#if 0
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
        mosPrint("Debug Enabled\n");
    }
    u32 cpu_id = SCB->CPUID;
    mosPrintf("CPU ID(0x%08X) ARCH(%1X) PART_NO(%02X)\n", cpu_id, (cpu_id >> 16) & 0xF,
                  (cpu_id >> 4) & 0xFFF);
#endif

    // Initialize and Run test bench example Application.
    if (InitTestBench() == 0) {
        // Start multitasking, running App threads.
        mosRunScheduler();
    }
    return -1;
}

