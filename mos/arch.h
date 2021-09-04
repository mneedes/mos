
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Architecture Definitions
//

#ifndef _MOS_ARCH_H_
#define _MOS_ARCH_H_

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

#if (MOS_FP_CONTEXT_SWITCHING == true)
  #if (__FPU_USED == 1U)
    #define ENABLE_FP_CONTEXT_SAVE    true
  #else
    #define ENABLE_FP_CONTEXT_SAVE    false
  #endif
#else
  #define ENABLE_FP_CONTEXT_SAVE    false
#endif

#define SYSTICK_CTRL_ENABLE     (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_CLKSOURCE_Msk)
#define SYSTICK_CTRL_DISABLE    (SysTick_CTRL_CLKSOURCE_Msk)

#endif
