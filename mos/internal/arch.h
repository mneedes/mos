
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Architecture Definitions (Internal)
//

#ifndef _MOS_INTERNAL_ARCH_H_
#define _MOS_INTERNAL_ARCH_H_

#include <mos/defs.h>

// Exception return values (for LR register)
#define MOS_EXC_RETURN_DEFAULT    0xfffffffd
#define MOS_EXC_RETURN_UNSECURE   0xffffffbc

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
#define MOS_VAL_VECTKEY        0x05fa0000
#define MOS_VAL_AIRCR_SEC_MASK 0x00000730
#define MOS_VAL_AIRCR_SEC      (MOS_VAL_VECTKEY | 0x00004000)

#define MOS_REG_CCR            (*(volatile u32 *)0xe000ed14)
#define MOS_VAL_DIV0_TRAP      (0x1 << 4)
#define MOS_VAL_UNALIGN_TRAP   (0x1 << 3)

#define MOS_REG_SHCSR          (*(volatile u32 *)0xe000ed24)
#define MOS_VAL_FAULT_ENABLE   (0xf << 16)

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
#define MOS_REG_SFSR           (*(volatile u32 *)0xe000ede4)
#define MOS_REG_SFAR           (*(volatile u32 *)0xe000ede8)

// Debug registers
#define MOS_REG_DHCSR          (*(volatile u32 *)0xe000edf0)
#define MOS_VAL_DEBUG_ENABLED  (0x1)

// Interrupts / Exceptions
#define MOS_REG_SHPR(x)        (*((volatile u8 *)0xe000ed18 + (x)))
#define MOS_REG_SHPR3          (*(volatile u32 *)0xe000ed20)
#define MOS_VAL_EXC_PRIORITY   0xc0c00000
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
