
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel Secure-side Implementation
//

#if (MOS_ARM_RTOS_ON_SECURE_SIDE == true)

#include <mos/kernel.h>
#include <mos/internal/arch.h>
#include <mos/internal/security.h>

// Stack pointer storage for secure context
typedef struct {
    u32 sp;
    u32 splim;
} SecureContext;

// TODO: Default Stack???
static u8 MOS_STACK_ALIGNED DefaultStack[256];

// Secure context stacks are for user applications.
static u8 MOS_STACK_ALIGNED Stacks[MOS_NUM_SECURE_CONTEXTS][MOS_SECURE_CONTEXT_STACK_SIZE];

// Stack pointer storage for context switches.
static SecureContext Contexts[MOS_NUM_SECURE_CONTEXTS + 1];

// Secure stack size must be multiple of 8
MOS_STATIC_ASSERT(sec_stack_size, (MOS_SECURE_CONTEXT_STACK_SIZE & 0x7) == 0x0);

static MOS_INLINE u32 GetPSP(void) {
    u32 psp;
    asm volatile ( "mrs %0, psp" : "=r" (psp) );
    return psp;
}

static MOS_INLINE void SetPSP(u32 psp) {
    asm volatile ( "msr psp, %0" : "=r" (psp) );
}

static MOS_INLINE void SetPSPLIM(u32 psplim) {
    asm volatile ( "msr psplim, %0" : "=r" (psplim) );
}

static MOS_INLINE void SetControl(u32 control) {
    asm volatile ( "msr control, %0" : "=r" (control) );
}

// NOTE: This should be run in handler mode since it is
//       manipulating stack pointers and the CONTROL register.
void MOS_NSC_ENTRY _MosInitSecureContexts(void) {
    // Initialize stack pointers
    Contexts[0].sp    = (u32)DefaultStack + sizeof(DefaultStack);
    Contexts[0].splim = (u32)DefaultStack;
    for (u32 context = 1; context <= MOS_NUM_SECURE_CONTEXTS; context++) {
        Contexts[context].sp    = (u32)&Stacks[context][0];
        Contexts[context].splim = (u32)&Stacks[context - 1][0];
    }
    // Set initial stack pointers and set thread mode on secure side
    SetPSP(Contexts[0].sp);
    SetPSPLIM(Contexts[0].splim);
    SetControl(0x2);
}

// NOTE: This can be run in thread mode as long as the scheduler is locked
//       during the call as it is not manipulating stack pointers directly.
void MOS_NSC_ENTRY _MosResetSecureContext(s32 context) {
    Contexts[context].sp = (u32)&Stacks[context][0];
}

// NOTE: This must be run in handler mode since it is manipulating stack pointers.
void MOS_NSC_ENTRY _MosSwitchSecureContext(s32 save_context, s32 restore_context) {
    if (save_context >= 0) Contexts[save_context].sp = GetPSP();
    SetPSP(Contexts[restore_context].sp);
    SetPSPLIM(Contexts[restore_context].splim);
}

#if 0

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

#endif

void MOS_NAKED MOS_WEAK UsageFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

#endif

