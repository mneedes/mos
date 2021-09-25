
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

// Auto detect correct EXC Return setting value
#if ((MOS_ARCH == MOS_ARCH_ARM_CORTEX_V8M_MAIN) || (MOS_ARCH == MOS_ARCH_ARM_CORTEX_V8M_BASE))
  #define MOS_ARM_AUTODETECT_EXC_RETURN    true
#else
  #define MOS_ARM_AUTODETECT_EXC_RETURN    false
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

// Lazy floating point context switch
#if (defined(__VFP_FP__) && !defined(__SOFTFP__))
  #define MOS_FP_LAZY_CONTEXT_SWITCHING  true
#else
  #define MOS_FP_LAZY_CONTEXT_SWITCHING  false
#endif

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

#endif
