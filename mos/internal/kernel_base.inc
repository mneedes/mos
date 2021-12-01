
// Copyright 2021 Matthew C Needes
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

void MosLockMutex(MosMutex * mtx) {
    LockScheduler(IntPriMaskLow);
    if (mtx->owner == (MosThread *)RunningThread) {
        mtx->depth++;
        UnlockScheduler();
        return;
    }
    while (mtx->owner != NO_SUCH_THREAD) {
        // Move thread to pend queue
        SortThreadByPriority(RunningThread, &mtx->pend_q);
        // Basic priority inheritance
        Thread * thd = (Thread *)mtx->owner;
        if (RunningThread->pri < thd->pri) {
            thd->pri = RunningThread->pri;
            if (thd->state == THREAD_RUNNABLE) {
                MosRemoveFromList(&thd->run_link);
                MosAddToFrontOfList(&RunQueues[thd->pri], &thd->run_link);
            }
        }
        RunningThread->blocked_on = mtx;
        SetThreadState(RunningThread, THREAD_WAIT_FOR_MUTEX);
        YieldThread();
        UnlockScheduler();
        // Scheduler is invoked here
        LockScheduler(IntPriMaskLow);
    }
    // Take it
    RunningThread->mtx_cnt++;
    mtx->owner = (MosThread *)RunningThread;
    mtx->depth = 1;
    asm volatile ( "dmb" );
    UnlockScheduler();
}

bool MosTryMutex(MosMutex * mtx) {
    LockScheduler(IntPriMaskLow);
    asm volatile ( "dsb" );
    if (mtx->owner == NO_SUCH_THREAD) {
        RunningThread->mtx_cnt++;
        mtx->owner = (MosThread *)RunningThread;
    }
    if (mtx->owner == (MosThread *)RunningThread) {
        mtx->depth++;
        UnlockScheduler();
        asm volatile ( "dmb" );
        return true;
    }
    UnlockScheduler();
    return false;
}

void MosUnlockMutex(MosMutex * mtx) {
    LockScheduler(IntPriMaskLow);
    asm volatile ( "dmb" );
    if (--mtx->depth == 0) {
        if (--RunningThread->mtx_cnt == 0 &&
        	    RunningThread->pri != RunningThread->nom_pri) {
            // Reset priority inheritance
            RunningThread->pri = RunningThread->nom_pri;
            MosRemoveFromList(&RunningThread->run_link);
            MosAddToFrontOfList(&RunQueues[RunningThread->pri],
                                    &RunningThread->run_link);
        }
        mtx->owner = NO_SUCH_THREAD;
        if (!MosIsListEmpty(&mtx->pend_q)) {
            MosLink * elm = mtx->pend_q.next;
            Thread * thd = container_of(elm, Thread, run_link);
            MosRemoveFromList(elm);
            MosAddToFrontOfList(&RunQueues[thd->pri], elm);
            if (MosIsOnList(&thd->tmr_link.link))
                MosRemoveFromList(&thd->tmr_link.link);
            SetThreadState(thd, THREAD_RUNNABLE);
            if (thd->pri < RunningThread->pri) YieldThread();
        }
    }
    UnlockScheduler();
}

void MosWaitForSem(MosSem * sem) {
    _MosDisableInterrupts();
    while (sem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(RunningThread, &sem->pend_q);
        RunningThread->blocked_on = sem;
        RunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
        _MosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        _MosDisableInterrupts();
    }
    sem->value--;
    asm volatile ( "dmb" );
    _MosEnableInterrupts();
}

bool MosWaitForSemOrTO(MosSem * sem, u32 ticks) {
    SetTimeout(ticks);
    _MosDisableInterrupts();
    while (sem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(RunningThread, &sem->pend_q);
        RunningThread->timed_out = 0;
        RunningThread->blocked_on = sem;
        RunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        _MosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        if (RunningThread->timed_out) return false;
        _MosDisableInterrupts();
    }
    sem->value--;
    asm volatile ( "dmb" );
    _MosEnableInterrupts();
    return true;
}

MOS_ISR_SAFE bool MosTrySem(MosSem * sem) {
    bool success = true;
    u32 mask = MosDisableInterrupts();
    if (sem->value > 0) {
        sem->value--;
        asm volatile ( "dmb" );
    } else {
        success = false;
    }
    MosEnableInterrupts(mask);
    return success;
}

MOS_ISR_SAFE void MosIncrementSem(MosSem * sem) {
    u32 mask = MosDisableInterrupts();
    sem->value++;
    asm volatile ( "dmb" );
    // This places the semaphore on event queue to be processed by
    // scheduler to avoid direct manipulation of run queues.  If run
    // queues were manipulated here critical sections would be larger.
    // Only add event if pend_q is not empty and event not already queued
    if (!MosIsListEmpty(&sem->pend_q) && !MosIsOnList(&sem->evt_link)) {
        MosAddToList(&ISREventQueue, &sem->evt_link);
        Thread * thd = container_of(sem->pend_q.next, Thread, run_link);
        // Yield if released thread has higher priority than running thread
        if (RunningThread && thd->pri < RunningThread->pri) YieldThread();
    }
    MosEnableInterrupts(mask);
}

u32 MosWaitForSignal(MosSem * sem) {
    _MosDisableInterrupts();
    while (sem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(RunningThread, &sem->pend_q);
        RunningThread->blocked_on = sem;
        RunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
        _MosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        _MosDisableInterrupts();
    }
    u32 mask = sem->value;
    sem->value = 0;
    asm volatile ( "dmb" );
    _MosEnableInterrupts();
    return mask;
}

u32 MosWaitForSignalOrTO(MosSem * sem, u32 ticks) {
    SetTimeout(ticks);
    _MosDisableInterrupts();
    while (sem->value == 0) {
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        SortThreadByPriority(RunningThread, &sem->pend_q);
        RunningThread->timed_out = 0;
        RunningThread->blocked_on = sem;
        RunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        _MosEnableInterruptsWithBarrier();
        // Scheduler is invoked here
        if (RunningThread->timed_out) return 0;
        _MosDisableInterrupts();
    }
    u32 mask = sem->value;
    sem->value = 0;
    asm volatile ( "dmb" );
    _MosEnableInterrupts();
    return mask;
}

MOS_ISR_SAFE u32 MosPollSignal(MosSem * sem) {
    u32 mask = MosDisableInterrupts();
    u32 poll_mask = sem->value;
    sem->value = 0;
    asm volatile ( "dmb" );
    MosEnableInterrupts(mask);
    return poll_mask;
}

MOS_ISR_SAFE void MosRaiseSignal(MosSem * sem, u32 flags) {
    u32 mask = MosDisableInterrupts();
    sem->value |= flags;
    asm volatile ( "dmb" );
    // This places the semaphore on event queue to be processed by
    // scheduler to avoid direct manipulation of run queues.  If run
    // queues were manipulated here critical sections would be larger.
    // Only add event if pend_q is not empty and event not already queued
    if (!MosIsListEmpty(&sem->pend_q) && !MosIsOnList(&sem->evt_link)) {
        MosAddToList(&ISREventQueue, &sem->evt_link);
        Thread * thd = container_of(sem->pend_q.next, Thread, run_link);
        // Yield if released thread has higher priority than running thread
        if (RunningThread && thd->pri < RunningThread->pri) YieldThread();
    }
    MosEnableInterrupts(mask);
}

//
// Faults
//

// TODO: Limit MSP stack dump to end of MSP stack
static void MOS_USED
FaultHandler(u32 * msp, u32 * psp, u32 exc_rtn) {
    bool in_isr = ((exc_rtn & 0x8) == 0x0);
    KPrintf("\n*** Hard Fault %s", in_isr ? "IN ISR " : "");

    if (RunningThread == NO_SUCH_THREAD) KPrintf("(Pre-Scheduler) ***\n");
    else if (RunningThread->name && RunningThread->name[0] != '\0')
        KPrintf("(Thread %s) ***\n", RunningThread->name);
    else
        KPrintf("(Thread @%08X) ***\n", RunningThread);

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
    if (RunningThread != NO_SUCH_THREAD) {
        KPrintf("Thread Stack @%08X:\n", (u32)psp);

        u8 * psp2 = RunningThread->stack_bottom;
        if (*((u32 *)psp2) != STACK_FILL_VALUE)
            KPrintf("!!! Thread Stack corruption (bottom) !!!\n");
        psp2 = (u8 *) ((u32)(psp2 + RunningThread->stack_size - sizeof(u32)) & 0xfffffff8);
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
        if (RunningThread == NO_SUCH_THREAD || in_isr) {
            // Hang if fault occurred anywhere but in thread context
            while (1);
        } else {
            // Stop thread if fault occurred in thread context
            SetThreadState(RunningThread, THREAD_TIME_TO_STOP);
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
