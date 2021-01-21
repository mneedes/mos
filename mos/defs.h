
//  Copyright 2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Definitions
//

#ifndef _MOS_DEFS_H_
#define _MOS_DEFS_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define MOS_VERSION            0.3

#ifndef count_of
#define count_of(x)            (sizeof(x) / sizeof(x[0]))
#endif
#ifndef offset_of
#define offset_of(t, m)        ((u32)&((t *)0)->m)
#endif
#ifndef container_of
#define container_of(p, t, m)  ((t *)((u8 *)(p) - offset_of(t, m)))
#endif
#ifndef NULL
#define NULL                   ((void *)0)
#endif

// Symbol / line number to string conversion
#define MOS_TO_STR_(x)         #x
#define MOS_TO_STR(x)          MOS_TO_STR_(x)
#define MOS__LINE__            MOS_TO_STR(__LINE__)

#define MOS_INLINE             __attribute__((always_inline)) inline
#define MOS_NAKED              __attribute__((naked))
#define MOS_USED               __attribute__((used))
#define MOS_WEAK               __attribute__((weak))
#define MOS_OPT(x)             __attribute__((optimize(x)))
#define MOS_ALIGNED(x)         __attribute__((aligned(x)))
#define MOS_ISR_SAFE

#define MOS_UNUSED(x)          (void)(x);

#define MOS_STACK_ALIGNMENT    8
#define MOS_STACK_ALIGNED      MOS_ALIGNED(MOS_STACK_ALIGNMENT)

// Can be used for U32 register reads and writes
#define MOS_VOL_U32(addr)      (*((volatile u32 *)(addr)))

typedef uint8_t     u8;
typedef int8_t      s8;
typedef uint16_t    u16;
typedef int16_t     s16;
typedef uint32_t    u32;
typedef int32_t     s32;
typedef uint64_t    u64;
typedef int64_t     s64;

typedef u8 MosThreadPriority;

#endif
