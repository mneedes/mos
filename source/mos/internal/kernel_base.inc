
// Copyright 2021-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel - ARM v6-m / v8-m(base) port
//

void MOS_NAKED PendSV_Handler(void) {
    asm volatile (
        "mrs r0, psp\n"
        "sub r0, r0, #36\n"
        "stmia r0!, {r4-r7}\n"
        "mov r3, r8\n"
        "mov r4, r9\n"
        "mov r5, r10\n"
        "mov r6, r11\n"
        "mov r7, lr\n"
        "stmia r0!, {r3-r7}\n"
        "sub r0, r0, #36\n"
        "bl Scheduler\n"
        "ldmia r0!, {r4-r7}\n"
        "ldmia r0!, {r1-r3}\n"
        "mov r8, r1\n"
        "mov r9, r2\n"
        "mov r10, r3\n"
        "ldmia r0!, {r1-r2}\n"
        "mov r11, r1\n"
        "mov lr, r2\n"
        "msr psp, r0\n"
        "bx lr"
    );
}

void mosLockMutex(MosMutex * pMtx) {
    LockScheduler(IntPriMaskLow);
    if (pMtx->pOwner == (MosThread *)pRunningThread) {
        pMtx->depth++;
        UnlockScheduler();
        return;
    }
    while (pMtx->pOwner != NO_SUCH_THREAD) {
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
        UnlockScheduler();
        // Scheduler is invoked here
        LockScheduler(IntPriMaskLow);
    }
    // Take it
    pRunningThread->mtxCnt++;
    pMtx->pOwner = (MosThread *)pRunningThread;
    pMtx->depth = 1;
    asm volatile ( "dmb" );
    UnlockScheduler();
}

bool mosTryMutex(MosMutex * pMtx) {
    LockScheduler(IntPriMaskLow);
    asm volatile ( "dsb" );
    if (pMtx->pOwner == NO_SUCH_THREAD) {
        if (pRunningThread) pRunningThread->mtxCnt++;
        pMtx->pOwner = (MosThread *)pRunningThread;
    }
    if (pMtx->pOwner == (MosThread *)pRunningThread) {
        pMtx->depth++;
        UnlockScheduler();
        asm volatile ( "dmb" );
        return true;
    }
    UnlockScheduler();
    return false;
}

void mosUnlockMutex(MosMutex * pMtx) {
    LockScheduler(IntPriMaskLow);
    asm volatile ( "dmb" );
    if (--pMtx->depth == 0) {
        if (pRunningThread && --pRunningThread->mtxCnt == 0 &&
        	    pRunningThread->pri != pRunningThread->nomPri) {
            // Reset priority inheritance
            pRunningThread->pri = pRunningThread->nomPri;
            mosRemoveFromList(&pRunningThread->runLink);
            mosAddToFrontOfList(&RunQueues[pRunningThread->pri],
                                    &pRunningThread->runLink);
        }
        pMtx->pOwner = NO_SUCH_THREAD;
        if (!mosIsListEmpty(&pMtx->pendQ)) {
            MosLink * elm = pMtx->pendQ.pNext;
            Thread * thd = container_of(elm, Thread, runLink);
            mosRemoveFromList(elm);
            mosAddToFrontOfList(&RunQueues[thd->pri], elm);
            if (mosIsOnList(&thd->tmrLink.link))
                mosRemoveFromList(&thd->tmrLink.link);
            SetThreadState(thd, THREAD_RUNNABLE);
            if (thd->pri < pRunningThread->pri) YieldThread();
        }
    }
    UnlockScheduler();
}

void mosWaitForSem(MosSem * pSem) {
    _mosDisableInterrupts();
    while (pSem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(pRunningThread, &pSem->pendQ);
        pRunningThread->pBlockedOn = pSem;
        pRunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
        _mosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        _mosDisableInterrupts();
    }
    pSem->value--;
    asm volatile ( "dmb" );
    _mosEnableInterrupts();
}

bool mosWaitForSemOrTO(MosSem * pSem, u32 ticks) {
    SetTimeout(ticks);
    _mosDisableInterrupts();
    while (pSem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(pRunningThread, &pSem->pendQ);
        pRunningThread->timedOut = 0;
        pRunningThread->pBlockedOn = pSem;
        pRunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        _mosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        if (pRunningThread->timedOut) return false;
        _mosDisableInterrupts();
    }
    pSem->value--;
    asm volatile ( "dmb" );
    _mosEnableInterrupts();
    return true;
}

MOS_ISR_SAFE bool mosTrySem(MosSem * pSem) {
    bool success = true;
    u32 mask = mosDisableInterrupts();
    if (pSem->value > 0) {
        pSem->value--;
        asm volatile ( "dmb" );
    } else {
        success = false;
    }
    mosEnableInterrupts(mask);
    return success;
}

MOS_ISR_SAFE void mosIncrementSem(MosSem * pSem) {
    u32 mask = mosDisableInterrupts();
    pSem->value++;
    asm volatile ( "dmb" );
    // This places the semaphore on event queue to be processed by
    // scheduler to avoid direct manipulation of run queues.  If run
    // queues were manipulated here critical sections would be larger.
    // Only add event if pendQ is not empty and event not already queued
    if (!mosIsListEmpty(&pSem->pendQ) && !mosIsOnList(&pSem->evtLink)) {
        mosAddToEndOfList(&ISREventQueue, &pSem->evtLink);
        Thread * pThd = container_of(pSem->pendQ.pNext, Thread, runLink);
        // Yield if released thread has higher priority than running thread
        if (pRunningThread && pThd->pri < pRunningThread->pri) YieldThread();
    }
    mosEnableInterrupts(mask);
}

u32 mosWaitForSignal(MosSem * pSem) {
    _mosDisableInterrupts();
    while (pSem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(pRunningThread, &pSem->pendQ);
        pRunningThread->pBlockedOn = pSem;
        pRunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
        _mosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        _mosDisableInterrupts();
    }
    u32 mask = pSem->value;
    pSem->value = 0;
    asm volatile ( "dmb" );
    _mosEnableInterrupts();
    return mask;
}

u32 mosWaitForSignalOrTO(MosSem * pSem, u32 ticks) {
    SetTimeout(ticks);
    _mosDisableInterrupts();
    while (pSem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(pRunningThread, &pSem->pendQ);
        pRunningThread->timedOut = 0;
        pRunningThread->pBlockedOn = pSem;
        pRunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        _mosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        if (pRunningThread->timedOut) return 0;
        _mosDisableInterrupts();
    }
    u32 mask = pSem->value;
    pSem->value = 0;
    asm volatile ( "dmb" );
    _mosEnableInterrupts();
    return mask;
}

MOS_ISR_SAFE u32 mosPollSignal(MosSem * pSem) {
    u32 mask = mosDisableInterrupts();
    u32 poll_mask = pSem->value;
    pSem->value = 0;
    asm volatile ( "dmb" );
    mosEnableInterrupts(mask);
    return poll_mask;
}

MOS_ISR_SAFE void mosRaiseSignal(MosSem * pSem, u32 flags) {
    u32 mask = mosDisableInterrupts();
    pSem->value |= flags;
    asm volatile ( "dmb" );
    // This places the semaphore on event queue to be processed by
    // scheduler to avoid direct manipulation of run queues.  If run
    // queues were manipulated here critical sections would be larger.
    // Only add event if pendQ is not empty and event not already queued
    if (!mosIsListEmpty(&pSem->pendQ) && !mosIsOnList(&pSem->evtLink)) {
        mosAddToFrontOfList(&ISREventQueue, &pSem->evtLink);
        Thread * thd = container_of(pSem->pendQ.pNext, Thread, runLink);
        // Yield if released thread has higher priority than running thread
        if (pRunningThread && thd->pri < pRunningThread->pri) YieldThread();
    }
    mosEnableInterrupts(mask);
}

//
// Faults
//

// TODO: Limit MSP stack dump to end of MSP stack
static void MOS_USED
FaultHandler(u32 * msp, u32 * psp, u32 exc_rtn) {
    bool in_isr = ((exc_rtn & 0x8) == 0x0);
    KPrintf("\n*** Hard Fault %s", in_isr ? "IN ISR " : "");

    if (pRunningThread == NO_SUCH_THREAD) KPrintf("(Pre-Scheduler) ***\n");
    else if (pRunningThread->pName && pRunningThread->pName[0] != '\0')
        KPrintf("(Thread %s) ***\n", pRunningThread->pName);
    else
        KPrintf("(Thread @%08X) ***\n", pRunningThread);

    KPrintf("\n   R4: %08X  R5: %08X   R6: %08X   R7: %08X\n", msp[4], msp[5], msp[6], msp[7]);
    KPrintf("   R8: %08X  R9: %08X  R10: %08X  R11: %08X\n", msp[0], msp[1], msp[2], msp[3]);
    KPrintf(" EXCR: %08X\n\n", exc_rtn);
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

        KPrintf(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  psp[0], psp[1], psp[2], psp[3]);
        KPrintf(" %08X %08X %08X %08X (R12 LR PC PSR)\n", psp[4], psp[5], psp[6], psp[7]);
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
            // Stop thread if fault occurred in thread context
            SetThreadState(pRunningThread, THREAD_TIME_TO_STOP);
            YieldThread();
        }
    }
}

void MOS_NAKED MOS_WEAK HardFault_Handler(void) {
    asm volatile (
        "mov r0, r8\n"
        "mov r1, r9\n"
        "mov r2, r10\n"
        "mov r3, r11\n"
        "push {r0-r7}\n"
        "mrs r0, msp\n"
        "mrs r1, psp\n"
        "mov r2, lr\n"
        "b FaultHandler"
            : : : "r0", "r1", "r2", "r3"
    );
}
