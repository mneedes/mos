
// Copyright 2020-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Definitions
//

#ifndef _MOS_DEFS_H_
#define _MOS_DEFS_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

// Configuration

#ifndef MOS_MAX_THREAD_PRIORITIES
/// Thread priorities <=> [0 ... MOS_MAX_THREAD_PRIORITIES - 1].
/// The lower the number the higher the priority
#define MOS_MAX_THREAD_PRIORITIES       8
#endif

#ifndef MOS_TICKS_PER_SECOND
/// System timer interrupt tick rate (for timers and time-slicing)
///
#define MOS_TICKS_PER_SECOND            1000
#endif

#ifndef MOS_HANG_ON_EXCEPTIONS
/// Hang on exceptions.
/// Can be used in systems with watchdog timer reset to reboot
/// Generally set to true unless this is the testbench.
#define MOS_HANG_ON_EXCEPTIONS          true
#endif

#ifndef MOS_ENABLE_EVENTS
/// Enable events (for use in profiling or debugging).
///
#define MOS_ENABLE_EVENTS               false
#endif

#ifndef MOS_NUM_SECURE_CONTEXTS
/// Number of simultaneous secure thread contexts (e.g.: TrustZone).
/// Set to zero to disable security.
/// Ignored on systems without security support.
#define MOS_NUM_SECURE_CONTEXTS         2
#endif

#ifndef MOS_SECURE_CONTEXT_STACK_SIZE
/// Stack size for secure context store (e.g.: TrustZone).
/// Ignored on systems without security support.
#define MOS_SECURE_CONTEXT_STACK_SIZE   512
#endif

// Kernel definitions

#define MOS_VERSION            0.8
#define MOS_VERSION_STRING     MOS_TO_STR(MOS_VERSION)

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

// Static assert where n is an unique name and c is the condition
#define MOS_STATIC_ASSERT(n, c)  \
                               typedef s32 static_assert_##n[(c) ? 0 : -1];

// Symbol / line number to string conversion
#define MOS_TO_STR_(x)         #x
#define MOS_TO_STR(x)          MOS_TO_STR_(x)
#define MOS__LINE__            MOS_TO_STR(__LINE__)

#define MOS_INLINE             __attribute__((always_inline)) inline
#define MOS_NO_INLINE          __attribute__((noinline))
#define MOS_PACKED             __attribute__((packed))
#define MOS_NAKED              __attribute__((naked))
#define MOS_USED               __attribute__((used))
#define MOS_WEAK               __attribute__((weak))
#define MOS_OPT(x)             __attribute__((optimize(x)))
#define MOS_ALIGNED(x)         __attribute__((aligned(x)))
#define MOS_NSC_ENTRY          __attribute__((cmse_nonsecure_entry))
#define MOS_NS_CALL            __attribute__((cmse_nonsecure_call))
#define MOS_ISR_SAFE

#define MOS_UNUSED(x)          (void)(x)
/* The parameter is really used, but tell compiler it is unused to reject warnings */
#define MOS_USED_PARAM(x)      MOS_UNUSED(x)

#define MOS_STACK_ALIGNMENT    8
#define MOS_STACK_ALIGNED      MOS_ALIGNED(MOS_STACK_ALIGNMENT)

#define MOS_PRINT_BUFFER_SIZE  128

// Align values up or down to nearest boundary
//   mask is (alignment - 1)
#define MOS_ALIGN32(val, mask)         (((u32)(val) + (mask)) & ~((u32)(mask)))
#define MOS_ALIGN32_DOWN(val, mask)    (((u32)(val)) & ~((u32)(mask)))
#define MOS_ALIGN64(val, mask)         (((u64)(val) + (mask)) & ~((u64)(mask)))
#define MOS_ALIGN64_DOWN(val, mask)    (((u64)(val)) & ~((u64)(mask)))
#define MOS_ALIGN_PTR(val, mask)       (((mos_size)(val) + (mask)) & ~((mos_size)(mask)))
#define MOS_ALIGN_PTR_DOWN(val, mask)  (((mos_size)(val)) & ~((mos_size)(mask)))

// Can be used for u32 register reads and writes
#define MOS_VOL_U32(addr)      (*((volatile u32 *)(addr)))

typedef uint8_t     u8;
typedef int8_t      s8;
typedef uint16_t    u16;
typedef int16_t     s16;
typedef uint32_t    u32;
typedef int32_t     s32;
typedef uint64_t    u64;
typedef int64_t     s64;
typedef uint32_t    mos_size;

typedef u8 MosThreadPriority;

#endif
