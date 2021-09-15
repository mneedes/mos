
// Copyright 2021 Matthew C Needes
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

static void MOS_USED BlockOnMutex(MosMutex * mtx) {
    LockScheduler(IntPriMaskLow);
    // Retry (don't block) if mutex has since been given
    if (mtx->owner != NO_SUCH_THREAD) {
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
    }
    UnlockScheduler();
}

void MOS_NAKED MosLockMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
    asm volatile (
        "ldr r1, _ThreadID\n"
        "ldr r1, [r1]\n"
        "RetryTM:\n"
        "ldrex r2, [r0]\n"
        "cmp r2, r1\n"
        "beq IncTM\n"
        "cbnz r2, BlockTM\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTM\n"
        "IncTM:\n"
        "ldr r2, [r0, #4]\n"
        "add r2, #1\n"
        "str r2, [r0, #4]\n"
        "dmb\n"
        "bx lr\n"
        "BlockTM:\n"
        "push {r0, r1, lr}\n"
        "bl BlockOnMutex\n"
        "pop {r0, r1, lr}\n"
        "b RetryTM\n"
        ".balign 4\n"
        "_ThreadID: .word RunningThread"
            // Explicit clobber list prevents compiler from making
            // assumptions about registers not being changed as
            // this assembly code calls a C function.  Normally C
            // ABI treats r0-r3 as input or scratch registers.
            : : : "r0", "r1", "r2", "r3"
    );
}

bool MOS_NAKED MosTryMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
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
        "add r2, #1\n"
        "str r2, [r0, #4]\n"
        "dmb\n"
        "mov r0, #1\n"
        "bx lr\n"
        "FailTRM:\n"
        "mov r0, #0\n"
        "bx lr\n"
        ".balign 4\n"
        "_ThreadID2: .word RunningThread"
            : : : "r0", "r1", "r2", "r3"
    );
}

static void MOS_USED ReleaseMutex(MosMutex * mtx) {
    LockScheduler(IntPriMaskLow);
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
    UnlockScheduler();
}

void MOS_NAKED MosUnlockMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
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

static void MOS_USED BlockOnSem(MosSem * sem) {
    // Lock in case semaphore is about to be given by another context.
    DisableInterrupts();
    // Retry (don't block) if count has since incremented
    if (sem->value == 0) {
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
    }
    EnableInterrupts();
}

void MOS_NAKED MosWaitForSem(MosSem * sem) {
    MOS_USED_PARAM(sem);
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

static bool MOS_USED BlockOnSemOrTO(MosSem * sem) {
    bool timeout = false;
    // Lock in case semaphore is about to be given by another context.
    DisableInterrupts();
    // Immediately retry (don't block) if count has since incremented
    if (sem->value == 0) {
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
        // Must enable interrupts before checking timeout to allow pend
        // Barrier ensures that pend occurs before checking timeout.
        EnableInterruptsWithBarrier();
        if (RunningThread->timed_out) timeout = true;
    } else EnableInterrupts();
    return timeout;
}

bool MOS_NAKED MosWaitForSemOrTO(MosSem * sem, u32 ticks) {
    MOS_USED_PARAM(sem);
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

bool MOS_NAKED MOS_ISR_SAFE MosTrySem(MosSem * sem) {
    MOS_USED_PARAM(sem);
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

// For yielding when semaphore is given
static void MOS_USED MOS_ISR_SAFE ReleaseOnSem(MosSem * sem) {
    // This places the semaphore on event queue to be processed by
    // scheduler to avoid direct manipulation of run queues.  If run
    // queues were manipulated here critical sections would be larger.
    DisableInterrupts();
    // Only add event if pend_q is not empty and event not already queued
    if (!MosIsListEmpty(&sem->pend_q) && !MosIsOnList(&sem->evt_link)) {
        MosAddToList(&ISREventQueue, &sem->evt_link);
        Thread * thd = container_of(sem->pend_q.next, Thread, run_link);
        // Yield if released thread has higher priority than running thread
        if (RunningThread && thd->pri < RunningThread->pri) YieldThread();
    }
    EnableInterrupts();
}

void MOS_NAKED MOS_ISR_SAFE MosIncrementSem(MosSem * sem) {
    MOS_USED_PARAM(sem);
    asm volatile (
        "push { lr }\n"
        "RetryGS:\n"
        "ldrex r1, [r0]\n"
        "add r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryGS\n"
        "dmb\n"
        "bl ReleaseOnSem\n"
        "pop { pc }"
            : : : "r0", "r1", "r2", "r3"
    );
}

u32 MOS_NAKED MosWaitForSignal(MosSem * sem) {
    MOS_USED_PARAM(sem);
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

u32 MOS_NAKED MosWaitForSignalOrTO(MosSem * sem, u32 ticks) {
    MOS_USED_PARAM(sem);
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

u32 MOS_NAKED MOS_ISR_SAFE MosPollSignal(MosSem * sem) {
    MOS_USED_PARAM(sem);
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

void MOS_NAKED MOS_ISR_SAFE MosRaiseSignal(MosSem * sem, u32 flags) {
    MOS_USED_PARAM(sem);
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
// TODO: Dump security registers (if run in secure mode?)
// TODO: Dump both stacks on ISR exception?
static void MOS_USED FaultHandler(u32 * msp, u32 * psp, u32 psr, u32 lr) {
    //if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
    //    asm volatile ( "bkpt 1" );
    //}
    char * fault_type[] = {
        "Hard", "Mem", "Bus", "Usage", "Imprecise Bus"
    };
    bool in_isr = ((lr & 0x8) == 0x0);
    bool fp_en = ((lr & 0x10) == 0x0);
    u32 cfsr = MOS_REG(CFSR);
    if (PrintfHook) {
        u32 fault_no = (psr & 0xf) - 3;
        if (fault_no == 2 && (cfsr & 0x400)) fault_no = 4;
        (*PrintfHook)("\n*** %s Fault %s", fault_type[fault_no],
                          in_isr ? "IN ISR " : "");
        if (RunningThread == NO_SUCH_THREAD) (*PrintfHook)("(Pre-Scheduler) ***\n");
        else if (RunningThread->name && RunningThread->name[0] != '\0')
            (*PrintfHook)("(Thread %s) ***\n", RunningThread->name);
        else
            (*PrintfHook)("(Thread @%08X) ***\n", RunningThread);

        if (fp_en) (*PrintfHook)("*** Lazy Floating Point Enabled ***\n");

        (*PrintfHook)(" HFSR: %08X  CFSR: %08X AFSR: %08X\n",
                          MOS_REG(HFSR), cfsr, MOS_REG(AFSR));
        (*PrintfHook)(" BFAR: %08X MMFAR: %08X\n\n", MOS_REG(BFAR), MOS_REG(MMFAR));

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
            // Clear CFSR bits
            MOS_REG(CFSR) = cfsr;
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
