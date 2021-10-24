
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel Secure-side Implementation
//

// TODO: Seal the stack per ARM security recommendations

#include <mos/kernel.h>
#include <mos/internal/arch.h>
#include <mos/internal/security.h>
#include <mos_s/format_string_s.h>

#if (MOS_ARM_RTOS_ON_SECURE_SIDE == true)

// Stack pointer storage for secure context
typedef struct {
    u32 splim;
    u32 sp;
} SecureContext;

// Secure context stacks are for user applications.
static u8 MOS_STACK_ALIGNED
Stacks[MOS_NUM_SECURE_CONTEXTS][MOS_SECURE_CONTEXT_STACK_SIZE];

// Stack pointer storage for context switches.
static SecureContext Contexts[MOS_NUM_SECURE_CONTEXTS];

// Raw Printf hook for the secure side
typedef MOS_NS_CALL MosSecKPrintHook NS_SecKPrintHook;
static NS_SecKPrintHook * KPrintHook = NULL;
static char (*RawPrintBuffer)[MOS_PRINT_BUFFER_SIZE] = NULL;

static void S_KPrintf(const char * fmt, ...) {
    if (KPrintHook) {
        va_list args;
        va_start(args, fmt);
        asm volatile ( "cpsid if" );
        S_MosVSNPrintf(*RawPrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
        (*KPrintHook)();
        asm volatile ( "cpsie if" );
        va_end(args);
    }
}

// Secure stack size must be multiple of 8
MOS_STATIC_ASSERT(sec_stack_size, (MOS_SECURE_CONTEXT_STACK_SIZE & 0x7) == 0x0);

static MOS_INLINE u32 GetPSP(void) {
    u32 psp;
    asm volatile ( "mrs %0, psp" : "=r" (psp) : : );
    return psp;
}

static MOS_INLINE void SetPSP(u32 psp) {
    asm volatile ( "msr psp, %0" : : "r" (psp) : );
}

static MOS_INLINE void SetPSPLIM(u32 psplim) {
    asm volatile ( "msr psplim, %0" : : "r" (psplim) : );
}

static MOS_INLINE void SetControl(u32 control) {
    asm volatile (
        "msr control, %0\n"
        "isb"
           : : "r" (control) : "memory" );
}

// NOTE: This should be run in handler mode since it is
//       manipulating stack pointers and the CONTROL register.
void MOS_NSC_ENTRY
_NSC_MosInitSecureContexts(MosSecKPrintHook * hook, char (*buffer)[MOS_PRINT_BUFFER_SIZE]) {
    KPrintHook = (NS_SecKPrintHook *)hook;
    RawPrintBuffer = buffer;

    // TODO: Initialize exception handler configuration
    MOS_REG(CCR) |=  MOS_REG_VALUE(DIV0_TRAP);
    MOS_REG(CCR) &= ~MOS_REG_VALUE(UNALIGN_TRAP);
    // Enable Bus, Memory and Usage Faults in general
    MOS_REG(SHCSR) |= MOS_REG_VALUE(FAULT_ENABLE);

    // Initialize stack pointers
    for (u32 context = 0; context < MOS_NUM_SECURE_CONTEXTS; context++) {
        Contexts[context].splim = (u32)&Stacks[context][0];
        Contexts[context].sp    = (u32)&Stacks[context][0] + MOS_SECURE_CONTEXT_STACK_SIZE;
    }
    // Set initial stack pointers and set thread mode on secure side
    SetPSPLIM(Contexts[0].splim);
    SetPSP(Contexts[0].sp);
    SetControl(0x2);
    S_KPrintf("Initializing secure side\n");
}

// NOTE: This can be run in thread mode as long as the scheduler is locked
//       during the call as it is not manipulating stack pointers directly.
void MOS_NSC_ENTRY _NSC_MosResetSecureContext(s32 context) {
    Contexts[context].sp = (u32)&Stacks[context][0] + MOS_SECURE_CONTEXT_STACK_SIZE;
}

// Induces a crash
static MOS_INLINE void S_CauseCrash(void) {
#if (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_BASE)
    // Unaligned access
    asm volatile (
        "mov r0, #3\n"
        "ldr r1, [r0]"
            : : : "r0", "r1"
    );
#elif (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_MAIN)
    // Divide-by-zero
    asm volatile (
        "mov r0, #0\n"
        "udiv r1, r1, r0"
            : : : "r0", "r1"
    );
#endif
}

// NOTE: This must be run in handler mode since it is manipulating stack pointers.
void MOS_NSC_ENTRY _NSC_MosSwitchSecureContext(s32 save_context, s32 restore_context) {
    if (save_context >= 0) Contexts[save_context].sp = GetPSP();
    SetPSPLIM(Contexts[restore_context].splim);
    SetPSP(Contexts[restore_context].sp);
    //S_CauseCrash();
}

//
// Faults
//

// TODO: Which faults are supported on secure side?
// TODO: Secure debug register dump?

static void MOS_USED FaultHandler(u32 * msp, u32 * psp, u32 psr, u32 lr) {
    char * fault_type[] = {
        "Hard", "Mem", "Bus", "Usage", "Secure", "Imprecise Bus"
    };
    bool in_isr = ((lr & 0x8) == 0x0);
    bool fp_en = ((lr & 0x10) == 0x0);
    u32 cfsr = MOS_REG(CFSR);
    u32 fault_no = (psr & 0xf) - 3;
    if (fault_no == 2 && (cfsr & 0x400)) fault_no = 5;
    S_KPrintf("\n*** %s Fault %s", fault_type[fault_no], in_isr ? "IN ISR " : "");
    if (fp_en) S_KPrintf("*** Lazy Floating Point Enabled ***\n");

    S_KPrintf(" HFSR: %08X  CFSR: %08X AFSR: %08X\n", MOS_REG(HFSR), cfsr, MOS_REG(AFSR));
    S_KPrintf(" BFAR: %08X MMFAR: %08X\n\n", MOS_REG(BFAR), MOS_REG(MMFAR));

    bool use_psp = ((lr & 0x4) == 0x4);
    u32 * sp = use_psp ? psp : msp;
    s32 num_words = 16;
#if 0
    if (use_psp && RunningThread != NO_SUCH_THREAD) {
        u8 * sp2 = RunningThread->stack_bottom;
        s32 rem_words = ((u32 *) sp2) - sp;
        if (rem_words < 64) num_words = rem_words;
        else num_words = 64;
    }
#endif
    S_KPrintf("%s Stack @%08X:\n", use_psp ? "Process" : "Main", (u32) sp);
    S_KPrintf(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  sp[0], sp[1], sp[2], sp[3]);
    S_KPrintf(" %08X %08X %08X %08X (R12 LR PC PSR)\n", sp[4], sp[5], sp[6], sp[7]);
    sp += 8;
    for (s32 ix = 0; ix < (num_words - 8); ix++) {
        S_KPrintf(" %08X", sp[ix], 0, 0, 0i);
        if ((ix & 0x3) == 0x3) S_KPrintf("\n", 0, 0, 0,0);
    }
    S_KPrintf("\n\n", 0, 0, 0, 0);
    while (1);
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

void MOS_NAKED MOS_WEAK SecureFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

#endif
