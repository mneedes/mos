
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/arch.h
/// \brief MOS Microkernel

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

#if defined(MOS_NUM_SECURE_CONTEXTS) && (MOS_NUM_SECURE_CONTEXTS > 0)
  #define MOS_SECURE_CONTEXTS      true
#else
  #define MOS_SECURE_CONTEXTS      false
#endif

#if (defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 1) && MOS_SECURE_CONTEXTS)
  #define MOS_ARM_RTOS_ON_NON_SECURE_SIDE    true
#else
  #define MOS_ARM_RTOS_ON_NON_SECURE_SIDE    false
#endif

#if (defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3) && MOS_SECURE_CONTEXTS)
  #define MOS_ARM_RTOS_ON_SECURE_SIDE        true
#else
  #define MOS_ARM_RTOS_ON_SECURE_SIDE        false
#endif

// Stack pointer overflow detection support
//   (1) Only v8-mainline supports splim_ns and additionally splim_s (with security extensions).
//   (2) v8-baseline only supports splim_s (with security extensions)
//     NOTE: CMSE Bit 0 = presence of TT (test target) instruction
//     NOTE: CMSE Bit 1 = target secure state (presence of -mcmse compiler flag)
#if ((MOS_ARCH == MOS_ARCH_ARM_CORTEX_V8M_MAIN) || (defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3)))
  #define MOS_ENABLE_SPLIM_SUPPORT       true
#else
  #define MOS_ENABLE_SPLIM_SUPPORT       false
#endif

//
// Interrupt methods
//

/// Disable Interrupts (Not nestable, assumes interrupts are enabled prior to call).
///
MOS_ISR_SAFE static MOS_INLINE void _mosDisableInterrupts(void) {
    asm volatile ( "cpsid i" );
}

/// Enable Interrupts (Not nestable, assumes interrupts are disabled prior to call).
///
MOS_ISR_SAFE static MOS_INLINE void _mosEnableInterrupts(void) {
    asm volatile ( "cpsie i" );
}

/// Enable Interrupts (Not nestable, assumes interrupts are disabled prior to call).
/// Provides barrier to ensure pending interrupt executes before
///   subsequent instructions.  Can be combined with _MosEnableInterrupts().
MOS_ISR_SAFE static MOS_INLINE void _mosEnableInterruptsWithBarrier(void) {
    asm volatile ( "cpsie i\n"
                   "isb" );
}

/// Disable interrupts (Nestable, recommended for ISRs).
///   Saves mask to remember if interrupts were already disabled prior to this.
MOS_ISR_SAFE static MOS_INLINE u32 mosDisableInterrupts(void) {
    u32 mask;
    asm volatile (
        "mrs %0, primask\n"
        "cpsid i"
            : "=r" (mask) : :
    );
    return mask;
}

/// Enable Interrupts (Nestable, recommended for ISRs).
///   Only enables if mask indicates interrupts had been enabled in prior call to MosDisableInterrupts().
MOS_ISR_SAFE static MOS_INLINE void mosEnableInterrupts(u32 mask) {
    asm volatile (
        "msr primask, %0"
            : : "r" (mask) :
    );
}

/// Used to determine if in interrupt context.
/// \return '0' if not in an interrupt, otherwise returns vector number
MOS_ISR_SAFE static MOS_INLINE u32 mosGetIRQNumber(void) {
    u32 irq;
    asm volatile (
        "mrs %0, ipsr"
            : "=r" (irq)
    );
    return irq;
}

//
// Atomic Operations
//

#if (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_BASE)

// GCC does not implement atomic builtins for Base.

/// Atomic fetch and add
///
MOS_ISR_SAFE static MOS_INLINE s32
mosAtomicFetchAndAdd32(s32 * pValue, s32 addVal) {
    u32 mask = mosDisableInterrupts();
    s32 val = *pValue;
    *pValue = val + addVal;
    mosEnableInterrupts(mask);
    return val;
}

/// Atomic compare and swap
///
MOS_ISR_SAFE static MOS_INLINE u32
mosAtomicCompareAndSwap32(u32 * pValue, u32 compareVal, u32 exchangeVal) {
    u32 mask = mosDisableInterrupts();
    u32 val = *pValue;
    if (*pValue == compareVal) *pValue = exchangeVal;
    mosEnableInterrupts(mask);
    return val;
}

#elif (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_MAIN)

/// Atomic fetch and add
///
MOS_ISR_SAFE static MOS_INLINE s32
mosAtomicFetchAndAdd32(s32 * pValue, s32 addVal) {
    return __sync_fetch_and_add_4(pValue, addVal);
}

/// Atomic compare and swap
///
MOS_ISR_SAFE static MOS_INLINE u32
mosAtomicCompareAndSwap32(u32 * pValue, u32 compareVal, u32 exchangeVal) {
    return __sync_val_compare_and_swap_4(pValue, compareVal, exchangeVal);
}

#endif

#endif
