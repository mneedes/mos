
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Architecture Definitions
//

#ifndef _MOS_ARCH_H_
#define _MOS_ARCH_H_

// ARM Architectures
#define MOS_ARCH_ARM_CORTEX_V6M         0
#define MOS_ARCH_ARM_CORTEX_V7M         1
#define MOS_ARCH_ARM_CORTEX_V8M_BASE    2
#define MOS_ARCH_ARM_CORTEX_V8M_MAIN    3

// ARM Architecture Categories
#define MOS_ARCH_ARM_CORTEX_M_BASE      100
#define MOS_ARCH_ARM_CORTEX_M_MAIN      101

// Detect ARM architecture
#if (defined(__ARM_ARCH_6M__))
  #define MOS_ARCH        MOS_ARCH_ARM_CORTEX_V6M
  #define MOS_ARCH_CAT    MOS_ARCH_ARM_CORTEX_M_BASE
#elif (defined(__ARM_ARCH_8M_BASE__))
  #define MOS_ARCH        MOS_ARCH_ARM_CORTEX_V8M_BASE
  #define MOS_ARCH_CAT    MOS_ARCH_ARM_CORTEX_M_BASE
#elif (defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__))
  #define MOS_ARCH        MOS_ARCH_ARM_CORTEX_V7M
  #define MOS_ARCH_CAT    MOS_ARCH_ARM_CORTEX_M_MAIN
#elif (defined(__ARM_ARCH_8M_MAIN__))
  #define MOS_ARCH        MOS_ARCH_ARM_CORTEX_V8M_MAIN
  #define MOS_ARCH_CAT    MOS_ARCH_ARM_CORTEX_M_MAIN
#else
  #error "Architecture not recognized"
#endif

// Detect presence of security features
#if (defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE >= 3))
  #define MOS_ARM_SECURITY_SUPPORT       true
#else
  #define MOS_ARM_SECURITY_SUPPORT       false
#endif

// Stack pointer overflow detection support
#if ((MOS_ARCH == MOS_ARCH_ARM_CORTEX_V8M_MAIN) || (MOS_ARM_SECURITY_SUPPORT == true))
  #define MOS_ENABLE_SPLIM_SUPPORT       true
#else
  #define MOS_ENABLE_SPLIM_SUPPORT       false
#endif

// Lazy floating point context switch
#if (defined(__VFP_FP__) && !defined(__SOFTFP__))
  #define MOS_FP_LAZY_CONTEXT_SWITCHING  true
#else
  #define MOS_FP_LAZY_CONTEXT_SWITCHING  false
#endif

// Exception return values (for LR register)
#define MOS_EXC_RETURN_DEFAULT    0xfffffffd
#define MOS_EXC_RETURN_UNSECURE   0xffffffbc

//
// Interrupt locking
//

static MOS_INLINE void DisableInterrupts(void) {
    asm volatile ( "cpsid if" );
}

static MOS_INLINE void EnableInterrupts(void) {
    asm volatile ( "cpsie if" );
}

static MOS_INLINE void EnableInterruptsWithBarrier(void) {
    // Provides barrier to ensure pending interrupt executes before
    //   subsequent instructions.
    asm volatile ( "cpsie if\n"
                   "isb" );
}

static MOS_INLINE void ExecutePendingInterrupts(void) {
    // Execute any pending interrupts (does not require isb)
    asm volatile ( "cpsie if\n"
                   "cpsid if" );
}

//
// Scheduler locking
//

#if (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_BASE)

static MOS_INLINE void LockScheduler(u32 pri) {
    MOS_UNUSED(pri);
    asm volatile ( "cpsid if" );
}

static MOS_INLINE void UnlockScheduler(void) {
    asm volatile ( "cpsie if\n"
                   "isb" );
}

#elif (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_MAIN)

static MOS_INLINE void LockScheduler(u32 pri) {
    asm volatile (
        "msr basepri, %0\n"
        "isb"
            : : "r" (pri) : "memory"
    );
}

static MOS_INLINE void UnlockScheduler(void) {
    asm volatile (
        "msr basepri, %0\n"
        "isb"
            : : "r" (0) : "memory"
    );
}

#endif

// System Registers
#define MOS_REG_ICSR           (*(volatile u32 *)0xe000ed04)
#define MOS_VAL_ICSR_PENDST    (0x1 << 26)
#define MOS_VAL_ICSR_PENDSV    (0x1 << 28)

#define MOS_REG_AIRCR          (*(volatile u32 *)0xe000ed0c)
#define MOS_GET_PRI_GROUP_NUM  ((MOS_REG_AIRCR >> 8) & 0x7)
#define MOS_VAL_AIRCR_MASK     0x00006030
#define MOS_VAL_VECTKEY        0x05fa0000

#define MOS_REG_CCR            (*(volatile u32 *)0xe000ed14)
#define MOS_VAL_DIV0_TRAP      (0x1 << 4)
#define MOS_VAL_UNALIGN_TRAP   (0x1 << 3)

#define MOS_REG_SHCSR          (*(volatile u32 *)0xe000ed24)
#define MOS_VAL_FAULT_ENABLE   (0x7 << 16)

#define MOS_REG_CPUID_NS       (*(volatile u32 *)0xe002ed00)

// Floating Point Control
#define MOS_REG_CPACR          (*(volatile u32 *)0xe000ed88)
#define MOS_VAL_FPU_ENABLE     (0x3 << 20)
#define MOS_REG_FPCCR          (*(volatile u32 *)0xe000ef34)
#define MOS_VAL_LAZY_STACKING  (0x3 << 30)

// Fault status
#define MOS_REG_CFSR           (*(volatile u32 *)0xe000ed28)
#define MOS_REG_HFSR           (*(volatile u32 *)0xe000ed2c)
#define MOS_REG_MMFAR          (*(volatile u32 *)0xe000ed34)
#define MOS_REG_BFAR           (*(volatile u32 *)0xe000ed38)
#define MOS_REG_AFSR           (*(volatile u32 *)0xe000ed3c)

// Debug registers
#define MOS_REG_DHCSR          (*(volatile u32 *)0xe000edf0)
#define MOS_VAL_DEBUG_ENABLED  (0x1)

// Interrupts
#define MOS_REG_SHPR(x)        (*((volatile u8 *)0xe000ed18 + (x)))
#define MOS_REG_SHPR3          (*((volatile u32 *)0xe000ed20)
#define MOS_PENDSV_IRQ         14
#define MOS_SYSTICK_IRQ        15

// SysTick
#define MOS_REG_TICK_CTRL      (*(volatile u32 *)0xe000e010)
#define MOS_REG_TICK_LOAD      (*(volatile u32 *)0xe000e014)
#define MOS_REG_TICK_VAL       (*(volatile u32 *)0xe000e018)
#define MOS_VAL_TICK_ENABLE    0x7
#define MOS_VAL_TICK_DISABLE   0x4
#define MOS_VAL_TICK_FLAG      (0x1 << 16)

// Register Access
#define MOS_REG(NAME)          MOS_REG_ ## NAME
#define MOS_REG_VALUE(NAME)    MOS_VAL_ ## NAME

#endif
