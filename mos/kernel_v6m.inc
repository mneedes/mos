
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel (ARM v6-m port)
//

void MOS_NAKED PendSV_Handler(void) {
    // Vanilla context switch without floating point.
    asm volatile (
        "mrs r0, psp\n"
        "stmdb r0!, {r4-r11, lr}\n"
        "bl Scheduler\n"
        "ldmfd r0!, {r4-r11, lr}\n"
        "msr psp, r0\n"
        "bx lr"
    );
}

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

void MOS_NAKED MosWaitForSem(MosSem * sem) {
    MOS_USED_PARAM(sem);
#if 0
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
#endif
}

bool MOS_NAKED MosWaitForSemOrTO(MosSem * sem, u32 ticks) {
    MOS_USED_PARAM(sem);
    MOS_USED_PARAM(ticks);
#if 0
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
#endif
}

bool MOS_NAKED MOS_ISR_SAFE MosTrySem(MosSem * sem) {
    MOS_USED_PARAM(sem);
    asm volatile (
#if 0
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
#endif
}

void MOS_NAKED MOS_ISR_SAFE MosIncrementSem(MosSem * sem) {
    MOS_USED_PARAM(sem);
#if 0
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
#endif
}

u32 MOS_NAKED MosWaitForSignal(MosSem * sem) {
    MOS_USED_PARAM(sem);
#if 0
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
#endif
}

u32 MOS_NAKED MosWaitForSignalOrTO(MosSem * sem, u32 ticks) {
    MOS_USED_PARAM(sem);
    MOS_USED_PARAM(ticks);
#if 0
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
#endif
}

u32 MOS_NAKED MOS_ISR_SAFE MosPollForSignal(MosSem * sem) {
    MOS_USED_PARAM(sem);
#if 0
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
#endif
}

void MOS_NAKED MOS_ISR_SAFE MosRaiseSignal(MosSem * sem, u32 flags) {
    MOS_USED_PARAM(sem);
    MOS_USED_PARAM(flags);
#if 0
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
#endif
}
