
// Copyright 2021-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel - ARM v7-m / v8-m(main) port
//

#if (MOS_FP_LAZY_CONTEXT_SWITCHING == true)

void MOS_NAKED PendSV_Handler(void) {
    // Floating point context switch (lazy stacking)
    asm volatile (
        "mrs r0, psp\n"
        "tst lr, #16\n"
        "it eq\n"
        "vstmdbeq r0!, {s16-s31}\n"
        "stmdb r0!, {r4-r11, lr}\n"
        "bl Scheduler\n"
        "ldmia r0!, {r4-r11, lr}\n"
        "tst lr, #16\n"
        "it eq\n"
        "vldmiaeq r0!, {s16-s31}\n"
        "msr psp, r0\n"
        "bx lr"
    );
}

#else

void MOS_NAKED PendSV_Handler(void) {
    // Vanilla context switch without floating point.
    asm volatile (
        "mrs r0, psp\n"
        "stmdb r0!, {r4-r11, lr}\n"
        "bl Scheduler\n"
        "ldmia r0!, {r4-r11, lr}\n"
        "msr psp, r0\n"
        "bx lr"
    );
}

#endif

static void MOS_USED BlockOnMutex(MosMutex * pMtx) {
    LockScheduler(IntPriMaskLow);
    // Retry (don't block) if mutex has since been given
    if (pMtx->pOwner != NO_SUCH_THREAD) {
        // Move thread to pend queue
        SortThreadByPriority(pRunningThread, &pMtx->pendQ);
        // Basic priority inheritance
        Thread * pThd = (Thread *)pMtx->pOwner;
        if (pRunningThread->pri < pThd->pri) {
            pThd->pri = pRunningThread->pri;
            if (pThd->state == THREAD_RUNNABLE) {
                mosRemoveFromList(&pThd->runLink);
                mosAddToFrontOfList(&RunQueues[pThd->pri], &pThd->runLink);
            }
        }
        pRunningThread->pBlockedOn = pMtx;
        SetThreadState(pRunningThread, THREAD_WAIT_FOR_MUTEX);
        YieldThread();
    }
    UnlockScheduler();
}

void MOS_NAKED mosLockMutex(MosMutex * pMtx) {
    MOS_USED_PARAM(pMtx);
    asm volatile (
        "ldr r1, _ThreadID\n"
        "ldr r1, [r1]\n"
      "RetryLM:\n"
        "ldrex r2, [r0]\n"
        "cmp r2, r1\n"
        "beq IncLM\n"
        "cbnz r2, BlockLM\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryLM\n"
      "IncLM:\n"
        "ldr r2, [r0, #4]\n"
        "cbnz r2, SkipAddLM\n"
        "cbz r1, SkipAddLM\n"
        "ldr r3, [r1, #4]\n"
        "add r3, #1\n"
        "str r3, [r1, #4]\n"
      "SkipAddLM:\n"
        "add r2, #1\n"
        "str r2, [r0, #4]\n"
        "dmb\n"
        "bx lr\n"
      "BlockLM:\n"
        "push {r0, r1, lr}\n"
        "bl BlockOnMutex\n"
        "pop {r0, r1, lr}\n"
        "b RetryLM\n"
        ".balign 4\n"
      "_ThreadID: .word pRunningThread"
            // Explicit clobber list prevents compiler from making
            // assumptions about registers not being changed as
            // this aspSembly code calls a C function.  Normally C
            // ABI treats r0-r3 as input or scratch registers.
            : : : "r0", "r1", "r2", "r3"
    );
}

bool MOS_NAKED mosTryMutex(MosMutex * pMtx) {
    MOS_USED_PARAM(pMtx);
    asm volatile (
        "ldr r1, _ThreadID2\n"
        "ldr r1, [r1]\n"
      "RetryTRM:\n"
        "ldrex r2, [r0]\n"
        "cmp r2, r1\n"
        "beq IncTRM\n"
        "cbnz r2, FailTRM\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTRM\n"
      "IncTRM:\n"
        "ldr r2, [r0, #4]\n"
        "cbnz r2, SkipAddTRM\n"
        "cbz r1, SkipAddTRM\n"
        "ldr r3, [r1, #4]\n"
        "add r3, #1\n"
        "str r3, [r1, #4]\n"
      "SkipAddTRM:\n"
        "add r2, #1\n"
        "str r2, [r0, #4]\n"
        "dmb\n"
        "mov r0, #1\n"
        "bx lr\n"
      "FailTRM:\n"
        "mov r0, #0\n"
        "bx lr\n"
        ".balign 4\n"
      "_ThreadID2: .word pRunningThread"
            : : : "r0", "r1", "r2", "r3"
    );
}

static void MOS_USED ReleaseMutex(MosMutex * pMtx) {
    LockScheduler(IntPriMaskLow);
    if (pRunningThread && --pRunningThread->mtxCnt == 0 &&
            pRunningThread->pri != pRunningThread->nomPri) {
        // Reset priority inheritance
        pRunningThread->pri = pRunningThread->nomPri;
        mosRemoveFromList(&pRunningThread->runLink);
        mosAddToFrontOfList(&RunQueues[pRunningThread->pri],
                                &pRunningThread->runLink);
    }
    if (!mosIsListEmpty(&pMtx->pendQ)) {
        MosLink * pElm = pMtx->pendQ.pNext;
        Thread * pThd = container_of(pElm, Thread, runLink);
        mosRemoveFromList(pElm);
        mosAddToFrontOfList(&RunQueues[pThd->pri], pElm);
        if (mosIsOnList(&pThd->tmrLink.link))
            mosRemoveFromList(&pThd->tmrLink.link);
        SetThreadState(pThd, THREAD_RUNNABLE);
        if (pThd->pri < pRunningThread->pri) YieldThread();
    }
    UnlockScheduler();
}

void MOS_NAKED mosUnlockMutex(MosMutex * pMtx) {
    MOS_USED_PARAM(pMtx);
    asm volatile (
        "dmb\n"
        "ldr r1, [r0, #4]\n"
        "sub r1, #1\n"
        "str r1, [r0, #4]\n"
        "cbnz r1, SkipGM\n"
        "mov r1, #0\n"
      "RetryGM:\n"
        "ldrex r2, [r0]\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryGM\n"
        "b ReleaseMutex\n"
      "SkipGM:\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

static void MOS_USED BlockOnSem(MosSem * pSem) {
    // Lock in case pSemaphore is about to be given by another context.
    _mosDisableInterrupts();
    // Retry (don't block) if count has since incremented
    if (pSem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(pRunningThread, &pSem->pendQ);
        pRunningThread->pBlockedOn = pSem;
        pRunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
    }
    _mosEnableInterrupts();
}

void MOS_NAKED mosWaitForSem(MosSem * pSem) {
    MOS_USED_PARAM(pSem);
    asm volatile (
      "RetryTS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockTS\n"
        "sub r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTS\n"
        "dmb\n"
        "bx lr\n"
      "BlockTS:\n"
        "push {r0, lr}\n"
        "bl BlockOnSem\n"
        "pop {r0, lr}\n"
        "b RetryTS"
            : : : "r0", "r1", "r2", "r3"
    );
}

static bool MOS_USED BlockOnSemOrTO(MosSem * pSem) {
    bool timeout = false;
    // Lock in case pSemaphore is about to be given by another context.
    _mosDisableInterrupts();
    // Immediately retry (don't block) if count has since incremented
    if (pSem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(pRunningThread, &pSem->pendQ);
        pRunningThread->timedOut = 0;
        pRunningThread->pBlockedOn = pSem;
        pRunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        // Must enable interrupts before checking timeout to allow pend
        // Barrier ensures that pend occurs before checking timeout.
        _mosEnableInterruptsWithBarrier();
        if (pRunningThread->timedOut) timeout = true;
    } else _mosEnableInterrupts();
    return timeout;
}

bool MOS_NAKED mosWaitForSemOrTO(MosSem * pSem, u32 ticks) {
    MOS_USED_PARAM(pSem);
    MOS_USED_PARAM(ticks);
    asm volatile (
        "push {r0, lr}\n"
        "mov r0, r1\n"
        "bl SetTimeout\n"
        "pop {r0, lr}\n"
      "RetryTSTO:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockTSTO\n"
        "sub r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTSTO\n"
        "dmb\n"
        "mov r0, #1\n"
        "bx lr\n"
      "BlockTSTO:\n"
        "push {r0, lr}\n"
        "bl BlockOnSemOrTO\n"
        "cmp r0, #0\n"
        "pop {r0, lr}\n"
        "beq RetryTSTO\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

MOS_ISR_SAFE bool MOS_NAKED mosTrySem(MosSem * pSem) {
    MOS_USED_PARAM(pSem);
    asm volatile (
      "RetryTRS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, FailTRS\n"
        "sub r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTRS\n"
        "dmb\n"
        "mov r0, #1\n"
        "bx lr\n"
      "FailTRS:\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

// For yielding when pSemaphore is given
MOS_ISR_SAFE static void MOS_USED ReleaseOnSem(MosSem * pSem) {
    // This places the pSemaphore on event queue to be processed by
    // scheduler to avoid direct manipulation of run queues.  If run
    // queues were manipulated here critical sections would be larger.
    u32 mask = mosDisableInterrupts();
    // Only add event if pendQ is not empty and event not already queued
    if (!mosIsListEmpty(&pSem->pendQ) && !mosIsOnList(&pSem->evtLink)) {
        mosAddToEndOfList(&ISREventQueue, &pSem->evtLink);
        Thread * pThd = container_of(pSem->pendQ.pNext, Thread, runLink);
        // Yield if released thread has higher priority than running thread
        if (pRunningThread && pThd->pri < pRunningThread->pri) YieldThread();
    }
    mosEnableInterrupts(mask);
}

MOS_ISR_SAFE void MOS_NAKED mosIncrementSem(MosSem * pSem) {
    MOS_USED_PARAM(pSem);
    asm volatile (
        "push { lr }\n"
      "RetryIS:\n"
        "ldrex r1, [r0]\n"
        "add r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryIS\n"
        "dmb\n"
        "bl ReleaseOnSem\n"
        "pop { pc }"
            : : : "r0", "r1", "r2", "r3"
    );
}

u32 MOS_NAKED mosWaitForSignal(MosSem * pSem) {
    MOS_USED_PARAM(pSem);
    asm volatile (
        "mov r3, #0\n"
      "RetryWS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockWS\n"
        "strex r2, r3, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryWS\n"
        "mov r0, r1\n"
        "dmb\n"
        "bx lr\n"
      "BlockWS:\n"
        "push {r0, r3, lr}\n"
        "bl BlockOnSem\n"
        "pop {r0, r3, lr}\n"
        "b RetryWS"
            : : : "r0", "r1", "r2", "r3"
    );
}

u32 MOS_NAKED mosWaitForSignalOrTO(MosSem * pSem, u32 ticks) {
    MOS_USED_PARAM(pSem);
    MOS_USED_PARAM(ticks);
    asm volatile (
        "push {r0, lr}\n"
        "mov r0, r1\n"
        "bl SetTimeout\n"
        "pop {r0, lr}\n"
      "RetryWSTO:\n"
        "mov r2, #0\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockWSTO\n"
        "strex r3, r2, [r0]\n"
        "cmp r3, #0\n"
        "bne RetryWSTO\n"
        "dmb\n"
        "mov r0, r1\n"
        "bx lr\n"
      "BlockWSTO:\n"
        "push {r0, lr}\n"
        "bl BlockOnSemOrTO\n"
        "cmp r0, #0\n"
        "pop {r0, lr}\n"
        "beq RetryWSTO\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

MOS_ISR_SAFE u32 MOS_NAKED mosPollSignal(MosSem * pSem) {
    MOS_USED_PARAM(pSem);
    asm volatile (
        "mov r3, #0\n"
      "RetryPS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, FailPS\n"
        "strex r2, r3, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryPS\n"
        "dmb\n"
        "mov r0, r1\n"
        "bx lr\n"
      "FailPS:\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

MOS_ISR_SAFE void MOS_NAKED mosRaiseSignal(MosSem * pSem, u32 flags) {
    MOS_USED_PARAM(pSem);
    MOS_USED_PARAM(flags);
    asm volatile (
        "push { lr }\n"
      "RetryRS:\n"
        "ldrex r2, [r0]\n"
        "orr r2, r2, r1\n"
        "strex r3, r2, [r0]\n"
        "cmp r3, #0\n"
        "bne RetryRS\n"
        "dmb\n"
        "bl ReleaseOnSem\n"
        "pop { pc }"
            : : : "r0", "r1", "r2", "r3"
    );
}

//
// Faults
//

// TODO: Limit MSP stack dump to end of MSP stack
static void MOS_USED
FaultHandler(u32 * msp, u32 * psp, u32 psr, u32 exc_rtn) {
    char * fault_type[] = {
        "Hard", "Mem", "Bus", "Usage", "Imprecise Bus"
    };

    u32 cfsr = MOS_REG(CFSR);
    u32 fault_no = (psr & 0xf) - 3;
    if (fault_no == 2 && (cfsr & 0x400)) fault_no = 4;
    bool in_isr = ((exc_rtn & 0x8) == 0x0);
    KPrintf("\n*** %s Fault %s", fault_type[fault_no], in_isr ? "IN ISR " : "");

    if (pRunningThread == NO_SUCH_THREAD) KPrintf("(Pre-Scheduler) ***\n");
    else if (pRunningThread->pName && pRunningThread->pName[0] != '\0')
        KPrintf("(Thread %s) ***\n", pRunningThread->pName);
    else
        KPrintf("(Thread @%08X) ***\n", pRunningThread);
    if ((exc_rtn & 0x10) == 0x0) KPrintf("*** Lazy Floating Point Enabled ***\n");

    KPrintf("\n    R4: %08X   R5: %08X   R6: %08X   R7: %08X\n", msp[0], msp[1], msp[2], msp[3]);
    KPrintf("    R8: %08X   R9: %08X  R10: %08X  R11: %08X\n", msp[4], msp[5], msp[6], msp[7]);
    KPrintf("  HFSR: %08X CFSR: %08X AFSR: %08X EXCR: %08X\n", MOS_REG(HFSR), cfsr, MOS_REG(AFSR), exc_rtn);
    KPrintf(" MMFAR: %08X BFAR: %08X\n\n", MOS_REG(MMFAR), MOS_REG(BFAR));
    msp += 8;

    s32 num_words = 16;
    if ((exc_rtn & 0x4) == 0x0) {
        KPrintf("Main Stack @%08X:\n", (u32)msp);
        KPrintf(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  msp[0], msp[1], msp[2], msp[3]);
        KPrintf(" %08X %08X %08X %08X (R12 LR PC PSR)\n", msp[4], msp[5], msp[6], msp[7]);
        msp += 8;
        for (s32 ix = 0; ix < (num_words - 8); ix++) {
            KPrintf(" %08X", msp[ix]);
            if ((ix & 0x3) == 0x3) KPrintf("\n");
        }
        KPrintf("\n");
    }
    if (pRunningThread != NO_SUCH_THREAD) {
        KPrintf("Thread Stack @%08X:\n", (u32)psp);

        u8 * psp2 = pRunningThread->pStackBottom;
        if (*((u32 *)psp2) != STACK_FILL_VALUE)
            KPrintf("!!! Thread Stack corruption (bottom) !!!\n");
        psp2 = (u8 *) ((u32)(psp2 + pRunningThread->stackSize - sizeof(u32)) & 0xfffffff8);
        if (*((u32 *)psp2) != STACK_FILL_VALUE)
            KPrintf("!!! Thread Stack corruption (top) !!!\n");

        s32 rem_words = ((u32 *)psp2) - psp;
        if (rem_words < 64) num_words = rem_words;
        else num_words = 64;

        bool not_stkovf = ((cfsr & 0x100000) == 0);
        KPrintf(" %08X %08X %08X %08X",  psp[0], psp[1], psp[2], psp[3]);
        if (not_stkovf) KPrintf("  (R0 R1 R2 R3)");
        KPrintf("\n %08X %08X %08X %08X", psp[4], psp[5], psp[6], psp[7]);
        if (not_stkovf) KPrintf(" (R12 LR PC PSR)");
        KPrintf("\n");

        psp += 8;
        for (s32 ix = 0; ix < (num_words - 8); ix++) {
            KPrintf(" %08X", psp[ix]);
            if ((ix & 0x3) == 0x3) KPrintf("\n");
        }
        KPrintf("\n");
    }
    if (MOS_HANG_ON_EXCEPTIONS) {
        while (1);
    } else {
        if (pRunningThread == NO_SUCH_THREAD || in_isr) {
            // Hang if fault occurred anywhere but in thread context
            while (1);
        } else {
            // Clear CFSR bits
            MOS_REG(CFSR) = cfsr;
            // Stop thread if fault occurred in thread context
            SetThreadState(pRunningThread, THREAD_TIME_TO_STOP);
            YieldThread();
        }
    }
}

void MOS_NAKED MOS_WEAK HardFault_Handler(void) {
    asm volatile (
        "push {r4-r11}\n"
        "mrs r0, msp\n"
        "mrs r1, psp\n"
        "mrs r2, psr\n"
        "mov r3, lr\n"
        "b FaultHandler"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MOS_NAKED MOS_WEAK MemManage_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

void MOS_NAKED MOS_WEAK BusFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

void MOS_NAKED MOS_WEAK UsageFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}
