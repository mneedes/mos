
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Architecture Definitions
//

#ifndef _MOS_ARCH_H_
#define _MOS_ARCH_H_

// Detect ARM architecture
#define MOS_ARM_V6M   0
#define MOS_ARM_V7M   1
#if (defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__))
  #define MOS_ARCH   MOS_ARM_V6M
#elif (defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__))
  #define MOS_ARCH   MOS_ARM_V7M
#else
  #error "Architecture not recognized"
#endif

// TODO: EXC Return actually depends on security mode on V8M
#if (defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__))
  #define DEFAULT_EXC_RETURN        0xffffffbc
#else
  #define DEFAULT_EXC_RETURN        0xfffffffd
#endif

#if (defined(__ARM_ARCH_8M_MAIN__) && __ARM_ARCH_8M_MAIN__ == 1U) || (defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE >= 3))
  #define ENABLE_SPLIM_SUPPORT      true
#else
  #define ENABLE_SPLIM_SUPPORT      false
#endif

#if (defined(__VFP_FP__) && !defined(__SOFTFP__))
  #define MOS_FP_LAZY_CONTEXT_SWITCHING  true
#else
  #define MOS_FP_LAZY_CONTEXT_SWITCHING  false
#endif

#define SYSTICK_CTRL_ENABLE     (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_CLKSOURCE_Msk)
#define SYSTICK_CTRL_DISABLE    (SysTick_CTRL_CLKSOURCE_Msk)

#endif
