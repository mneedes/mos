
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
        // Add thread to pend queue
        MosLink * elm = mtx->pend_q.next;
        for (; elm != &mtx->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        // Basic priority inheritance
        Thread * thd = (Thread *) mtx->owner;
        if (RunningThread->pri < thd->pri) {
            thd->pri = RunningThread->pri;
            if (thd->state == THREAD_RUNNABLE) {
                MosRemoveFromList(&thd->run_link);
                MosAddToFrontOfList(&RunQueues[thd->pri], &thd->run_link);
            }
        }
        SetThreadState(RunningThread, THREAD_WAIT_FOR_MUTEX);
        YieldThread();
        UnlockScheduler();
        // Scheduler is invoked here
        LockScheduler(IntPriMaskLow);
    }
    // Take it
    mtx->owner = (MosThread *)RunningThread;
    mtx->depth = 1;
    asm volatile ( "dmb" );
    UnlockScheduler();
}

bool MosTryMutex(MosMutex * mtx) {
    LockScheduler(IntPriMaskLow);
    asm volatile ( "dsb" );
    if (mtx->owner == NO_SUCH_THREAD) mtx->owner = (MosThread *)RunningThread;
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
        mtx->owner = NO_SUCH_THREAD;
        if (!MosIsListEmpty(&mtx->pend_q)) {
            MosLink * elm = mtx->pend_q.next;
            Thread * thd = container_of(elm, Thread, run_link);
            MosRemoveFromList(elm);
            MosAddToFrontOfList(&RunQueues[thd->pri], elm);
            if (MosIsOnList(&thd->tmr_link.link))
                MosRemoveFromList(&thd->tmr_link.link);
            SetThreadState(thd, THREAD_RUNNABLE);
            // Reset priority inheritance
            if (RunningThread->pri != RunningThread->nom_pri) {
                RunningThread->pri = RunningThread->nom_pri;
                MosRemoveFromList(&RunningThread->run_link);
                MosAddToFrontOfList(&RunQueues[RunningThread->pri],
                                        &RunningThread->run_link);
            }
            if (thd->pri < RunningThread->pri) YieldThread();
        }
    }
    UnlockScheduler();
}

void MosWaitForSem(MosSem * sem) {
    asm volatile ( "cpsid if" );
    while (sem->value == 0) {
        MosLink * elm = sem->pend_q.next;
        for (; elm != &sem->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        RunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
        asm volatile ( "cpsie if" );
        // Scheduler is invoked here
        asm volatile ( "cpsid if" );
    }
    sem->value--;
    asm volatile ( "dmb" );
    asm volatile ( "cpsie if" );
}

bool MosWaitForSemOrTO(MosSem * sem, u32 ticks) {
    SetTimeout(ticks);
    asm volatile ( "cpsid if" );
    while (sem->value == 0) {
        RunningThread->timed_out = 0;
        MosLink * elm = sem->pend_q.next;
        for (; elm != &sem->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        RunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        asm volatile (
            "cpsie if\n"
            "isb"
        );
        // Scheduler is invoked here
        if (RunningThread->timed_out) return false;
        asm volatile ( "cpsid if" );
    }
    sem->value--;
    asm volatile ( "dmb" );
    asm volatile ( "cpsie if" );
    return true;
}

bool MOS_ISR_SAFE MosTrySem(MosSem * sem) {
    bool success = true;
    asm volatile ( "cpsid if" );
    if (sem->value > 0) {
        sem->value--;
        asm volatile ( "dmb" );
    } else {
        success = false;
    }
    asm volatile ( "cpsie if" );
    return success;
}

void MOS_ISR_SAFE MosIncrementSem(MosSem * sem) {
    asm volatile ( "cpsid if" );
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
    asm volatile ( "cpsie if" );
}

u32 MosWaitForSignal(MosSem * sem) {
    asm volatile ( "cpsid if" );
    while (sem->value == 0) {
        MosLink * elm = sem->pend_q.next;
        for (; elm != &sem->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        RunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
        asm volatile ( "cpsie if" );
        // Scheduler is invoked here
        asm volatile ( "cpsid if" );
    }
    u32 mask = sem->value;
    sem->value = 0;
    asm volatile ( "dmb" );
    asm volatile ( "cpsie if" );
    return mask;
}

u32 MosWaitForSignalOrTO(MosSem * sem, u32 ticks) {
    SetTimeout(ticks);
    asm volatile ( "cpsid if" );
    while (sem->value == 0) {
        RunningThread->timed_out = 0;
        MosLink * elm = sem->pend_q.next;
        for (; elm != &sem->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        RunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        asm volatile (
            "cpsie if\n"
            "isb"
        );
        // Scheduler is invoked here
        if (RunningThread->timed_out) return 0;
        asm volatile ( "cpsid if" );
    }
    u32 mask = sem->value;
    sem->value = 0;
    asm volatile ( "dmb" );
    asm volatile ( "cpsie if" );
    return mask;
}

u32 MOS_ISR_SAFE MosPollSignal(MosSem * sem) {
    asm volatile ( "cpsid if" );
    u32 mask = sem->value;
    sem->value = 0;
    asm volatile ( "dmb" );
    asm volatile ( "cpsie if" );
    return mask;
}

void MOS_ISR_SAFE MosRaiseSignal(MosSem * sem, u32 flags) {
    asm volatile ( "cpsid if" );
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
    asm volatile ( "cpsie if" );
}

//
// Faults
//

// TODO: Limit MSP stack dump to end of MSP stack
// TODO: Dump both stacks on ISR exception?
static void MOS_USED FaultHandler(u32 * msp, u32 * psp, u32 psr, u32 lr) {
    bool in_isr = ((lr & 0x8) == 0x0);
    if (PrintfHook) {
        (*PrintfHook)("\n*** Hard Fault %s", in_isr ? "IN ISR " : "");
        if (RunningThread == NO_SUCH_THREAD) (*PrintfHook)("(Pre-Scheduler) ***\n");
        else if (RunningThread->name && RunningThread->name[0] != '\0')
            (*PrintfHook)("(Thread %s) ***\n", RunningThread->name);
        else
            (*PrintfHook)("(Thread @%08X) ***\n", RunningThread);
        bool use_psp = ((lr & 0x4) == 0x4);
        u32 * sp = use_psp ? psp : msp;
        s32 num_words = 16;
        if (use_psp && RunningThread != NO_SUCH_THREAD) {
            u8 * sp2 = RunningThread->stack_bottom;
            if (*((u32 *)sp2) != STACK_FILL_VALUE)
                (*PrintfHook)("!!! Thread Stack corruption (bottom) !!!\n");
            sp2 = (u8 *) ((u32)(sp2 + RunningThread->stack_size - sizeof(u32)) & 0xfffffff8);
            if (*((u32 *)sp2) != STACK_FILL_VALUE)
                (*PrintfHook)("!!! Thread Stack corruption (top) !!!\n");
            s32 rem_words = ((u32 *) sp2) - sp;
            if (rem_words < 64) num_words = rem_words;
            else num_words = 64;
        }
        (*PrintfHook)("%s Stack @%08X:\n", use_psp ? "Process" : "Main", (u32) sp);
        (*PrintfHook)(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  sp[0], sp[1], sp[2], sp[3]);
        (*PrintfHook)(" %08X %08X %08X %08X (R12 LR PC PSR)\n", sp[4], sp[5], sp[6], sp[7]);
        sp += 8;
        for (s32 ix = 0; ix < (num_words - 8); ix++) {
            (*PrintfHook)(" %08X", sp[ix]);
            if ((ix & 0x3) == 0x3) (*PrintfHook)("\n");
        }
        (*PrintfHook)("\n\n");
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
        "mrs r0, msp\n"
        "mrs r1, psp\n"
        "mrs r2, psr\n"
        "mov r3, lr\n"
        "b FaultHandler"
            : : : "r0", "r1", "r2", "r3"
    );
}
