
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel (ARM v6-m port)
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

#undef MOS_NAKED
#define MOS_NAKED

void MOS_NAKED MosLockMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
#if 0
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
#endif
}

bool MOS_NAKED MosTryMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
    return true;
#if 0
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
#endif
}

void MOS_NAKED MosUnlockMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
#if 0
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
#endif
}

#undef MOS_NAKED
#define MOS_NAKED __attribute__((naked))

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

u32 MOS_NAKED MosWaitForSignal(MosSem * sem) {
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

u32 MOS_NAKED MosWaitForSignalOrTO(MosSem * sem, u32 ticks) {
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

u32 MOS_ISR_SAFE MosPollForSignal(MosSem * sem) {
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
