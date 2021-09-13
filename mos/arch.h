
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

// Enable or disable system tick
#define MOS_SYSTICK_CTRL_ENABLE   (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_CLKSOURCE_Msk)
#define MOS_SYSTICK_CTRL_DISABLE  (SysTick_CTRL_CLKSOURCE_Msk)

//
// Scheduler Locking
//
#if (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_BASE)

static MOS_INLINE void LockScheduler(u32 pri) {
    MOS_UNUSED(pri);
    asm volatile ( "cpsid if" );
}

static MOS_INLINE void UnlockScheduler(void) {
    asm volatile ( "cpsie if" );
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

#endif
