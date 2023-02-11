
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel Secure-side Implementation
//

#include <mos/kernel.h>
#include <mos/kernel_s.h>

#include <mos/internal/arch.h>
#include <mos/internal/security.h>
#include <mos/format_string_s.h>

#if (MOS_ARM_RTOS_ON_SECURE_SIDE == true)

#define STACK_SEAL       0xfef5eda5

// Stack pointer storage for secure context
typedef struct {
    u8 * splim;
    u8 * sp;
} SecureContext;

// Secure context stacks are for user applications.
static u8 MOS_STACK_ALIGNED
Stacks[MOS_NUM_SECURE_CONTEXTS][MOS_SECURE_CONTEXT_STACK_SIZE];

// Secure stack size must be multiple of 8
MOS_STATIC_ASSERT(sec_stack_size, (MOS_SECURE_CONTEXT_STACK_SIZE & 0x7) == 0x0);

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
        _MosDisableInterrupts();
        S_MosVSNPrintf(*RawPrintBuffer, MOS_PRINT_BUFFER_SIZE, fmt, args);
        _MosEnableInterrupts();
        va_end(args);
        (*KPrintHook)();
    }
}

static MOS_INLINE u8 * GetPSP(void) {
    u8 * psp;
    asm volatile ( "mrs %0, psp" : "=r" (psp) : : );
    return psp;
}

static MOS_INLINE void SetPSP(u8 * psp) {
    asm volatile ( "msr psp, %0" : : "r" (psp) : );
}

static MOS_INLINE void SetPSPLIM(u8 * psplim) {
    asm volatile ( "msr psplim, %0" : : "r" (psplim) : );
}

static MOS_INLINE void SetControl(u32 control) {
    asm volatile (
        "msr control, %0\n"
        "isb"
           : : "r" (control) : "memory" );
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

// NOTE: This should be run in handler mode since it is
//       manipulating stack pointers and the CONTROL register.
void MOS_NSC_ENTRY
_NSC_mosInitSecureContexts(MosSecKPrintHook * hook, char (*buffer)[MOS_PRINT_BUFFER_SIZE]) {
    KPrintHook = (NS_SecKPrintHook *)hook;
    RawPrintBuffer = buffer;
    // Prioritize secure exceptions and enable all secure mode faults
    MOS_REG(AIRCR) = (MOS_REG(AIRCR) & MOS_REG_VALUE(AIRCR_SEC_MASK)) | MOS_REG_VALUE(AIRCR_SEC);
    // Trap Divide By 0 and disable "Unintentional" Alignment Faults
    MOS_REG(CCR) |=  MOS_REG_VALUE(DIV0_TRAP);
    MOS_REG(CCR) &= ~MOS_REG_VALUE(UNALIGN_TRAP);
    // Enable Bus, Memory, Usage and Security Faults in general
    MOS_REG(SHCSR) |= MOS_REG_VALUE(FAULT_ENABLE);
    // Initialize stack pointers
    for (u32 context = 0; context < MOS_NUM_SECURE_CONTEXTS; context++) {
        Contexts[context].splim = Stacks[context];
        Contexts[context].sp    = Stacks[context] + MOS_SECURE_CONTEXT_STACK_SIZE - 8;
        ((u32 *)Contexts[context].splim)[0] = STACK_SEAL;
        ((u32 *)Contexts[context].sp)[0]    = STACK_SEAL;
        ((u32 *)Contexts[context].sp)[1]    = STACK_SEAL;
    }
    // Set initial stack pointers and set thread mode on secure side
    SetPSPLIM(Contexts[0].splim);
    SetPSP(Contexts[0].sp);
    SetControl(0x2);
}

// NOTE: This can be run in thread mode as long as the scheduler is locked
//       during this call as it is not manipulating stack pointers directly.
void MOS_NSC_ENTRY _NSC_mosResetSecureContext(s32 context) {
    Contexts[context].sp = Stacks[context] + MOS_SECURE_CONTEXT_STACK_SIZE - 8;
    //S_CauseCrash();
}

// NOTE: This must be run in handler mode since it is manipulating stack pointers.
void MOS_NSC_ENTRY _NSC_mosSwitchSecureContext(s32 save_context, s32 restore_context) {
    if (save_context >= 0) Contexts[save_context].sp = GetPSP();
    SetPSPLIM(Contexts[restore_context].splim);
    SetPSP(Contexts[restore_context].sp);
    //S_CauseCrash();
}

//
// Faults
//

// TODO: Limit MSP stack dump to end of MSP stack
// TODO: Faults in Secure ISRs -- test
static void MOS_USED
FaultHandler(u32 * msp, u32 * psp, u32 psr, u32 exc_rtn) {
    char * fault_type[] = {
        "Hard", "Mem", "Bus", "Usage", "Security", "Imprecise Bus"
    };

    u32 cfsr = MOS_REG(CFSR);
    u32 fault_no = (psr & 0xf) - 3;
    if (fault_no == 2 && (cfsr & 0x400)) fault_no = 5;
    bool in_isr = ((exc_rtn & 0x8) == 0x0);

    S_KPrintf("\n*** %s Fault %s***\n", fault_type[fault_no], in_isr ? "IN ISR " : "");
    if ((exc_rtn & 0x10) == 0x0) S_KPrintf("*** Lazy Floating Point Enabled ***\n");

    S_KPrintf("  HFSR: %08X CFSR: %08X AFSR: %08X EXCR: %08X\n",
                MOS_REG(HFSR), cfsr, MOS_REG(AFSR), exc_rtn);
    S_KPrintf(" MMFAR: %08X BFAR: %08X SFSR: %08X SFAR: %08X\n\n",
                MOS_REG(MMFAR), MOS_REG(BFAR), MOS_REG(SFSR), MOS_REG(SFAR));

    // Read non-secure stack pointers from MSP
    u32 * msp_ns = (u32 *)msp[0];
    u32 * psp_ns = (u32 *)msp[1];
    msp += 2;

    if (fault_no != 4) {
        // IS NOT a security fault (originated from S side) ...
        S_KPrintf("   MSP: %08X  PSP: %08X\n", (u32)msp, (u32)psp);
        if ((cfsr & 0x100000) == 0x0) {
            // If this is a secure fault (NB: different from Security Fault)
            //   and not a stack overflow (STK_OVF) then print PC and LR of fault.
            u32 * sp = in_isr ? msp : psp;
            S_KPrintf("    LR: %08X   PC: %08X\n", sp[5], sp[6]);
        } else {
            S_KPrintf("!!! Secure Stack overflow (bottom) !!!\n");
        }
        for (u32 context = 0; context < MOS_NUM_SECURE_CONTEXTS; context++) {
            if (((u32 *)Stacks[context])[0] != STACK_SEAL) {
                S_KPrintf("!!! Secure Stack %u corruption (bottom) !!!\n", context);
            }
            u32 * top = (u32 *) (Stacks[context] + MOS_SECURE_CONTEXT_STACK_SIZE - 8);
            if (*top != STACK_SEAL) {
                S_KPrintf("!!! Secure Stack %u corruption (top) !!!\n", context);
            }
        }
    } else {
        // IS a security fault (originated from NS side) ... dump stacks after validating pointers
        if (in_isr || psp_ns == NULL) {
            S_KPrintf("NS Main Stack @%08X:\n", (u32)msp_ns);
            if (S_MosIsAddressRangeNonSecure(msp_ns, 64)) {
                S_KPrintf(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  msp_ns[0], msp_ns[1], msp_ns[2], msp_ns[3]);
                S_KPrintf(" %08X %08X %08X %08X (R12 LR PC PSR)\n", msp_ns[4], msp_ns[5], msp_ns[6], msp_ns[7]);
                msp_ns += 8;
                for (s32 ix = 0; ix < 8; ix++) {
                    S_KPrintf(" %08X", msp_ns[ix]);
                    if ((ix & 0x3) == 0x3) S_KPrintf("\n");
                }
                S_KPrintf("\n");
            }
        }
        S_KPrintf("NS Thread Stack @%08X:\n", (u32)psp_ns);
        if (S_MosIsAddressRangeNonSecure(psp_ns, 64)) {
            S_KPrintf(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  psp_ns[0], psp_ns[1], psp_ns[2], psp_ns[3]);
            S_KPrintf(" %08X %08X %08X %08X (R12 LR PC PSR)\n", psp_ns[4], psp_ns[5], psp_ns[6], psp_ns[7]);
            psp_ns += 8;
            for (s32 ix = 0; ix < 8; ix++) {
                S_KPrintf(" %08X", psp_ns[ix]);
                if ((ix & 0x3) == 0x3) S_KPrintf("\n");
            }
            S_KPrintf("\n R4: %08X R5: %08X  R6: %08X  R7: %08X\n", msp[0], msp[1], msp[2], msp[3]);
            S_KPrintf(" R8: %08X R9: %08X R10: %08X R11: %08X\n",   msp[4], msp[5], msp[6], msp[7]);
        }
    }
    S_KPrintf("\n");
    while (1);
}

void MOS_NAKED MOS_WEAK HardFault_Handler(void) {
    asm volatile (
        "mrs r0, msp_ns\n"
        "mrs r1, psp_ns\n"
        "push {r0-r1,r4-r11}\n"
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
