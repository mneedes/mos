
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel
//

#ifndef _MOS_KERNEL_H_
#define _MOS_KERNEL_H_

#include "mos_config.h"
#include <mos/defs.h>
#include <mos/list.h>

typedef enum {
    MOS_THREAD_NOT_STARTED,
    MOS_THREAD_RUNNING,
    MOS_THREAD_STOP_REQUEST,
    MOS_THREAD_STOPPED
} MosThreadState;

typedef enum {
    MOS_EVENT_SCHEDULER_ENTRY,
    MOS_EVENT_SCHEDULER_EXIT,
    MOS_EVENT_TICK
} MosEvent;

typedef struct MosTimer MosTimer;

// Callbacks
typedef s32 (MosThreadEntry)(s32 arg);
typedef MOS_ISR_SAFE bool (MosTimerCallback)(MosTimer * tmr);
typedef void (MosRawPrintfHook)(const char * fmt, ...);
typedef void (MosSleepHook)(void);
typedef void (MosWakeHook)(void);
typedef void (MosEventHook)(MosEvent evt, u32 val);

// Microkernel Parameters
typedef struct MosParams {
    char              * version;
    MosThreadPriority   thread_pri_hi;
    MosThreadPriority   thread_pri_low;
    u32                 int_pri_hi;
    u32                 int_pri_low;
    u32                 micro_sec_per_tick;
    bool                fp_support_en;
} MosParams;

// Mos Thread (opaque container)
typedef struct MosThread {
    u32 rsvd[18];
    s32 ref_cnt;
} MosThread;

// Blocking mutex supporting recursion
typedef struct MosMutex {
    MosThread * owner;
    s32         depth;
    MosList     pend_q;
} MosMutex;

typedef struct MosSem {
    u32     value;
    MosList pend_q;
    MosLink evt_link;
} MosSem;

typedef struct MosTimer {
    u32                ticks;
    u32                wake_tick;
    MosLinkHet         tmr_link;
    MosTimerCallback * callback;
    void             * priv_data;
} MosTimer;

// Initialize and Run Scheduler
// NOTE: SysTick and NVIC priority groups should be enabled by HAL before running Init.
void MosInit(void);
void MosRunScheduler(void);

// Hooks
void MosRegisterRawPrintfHook(MosRawPrintfHook * hook);
void MosRegisterSleepHook(MosSleepHook * hook);
void MosRegisterWakeHook(MosWakeHook * hook);
void MosRegisterEventHook(MosEventHook * hook);

// Obtain Microkernel parameters
const MosParams * MosGetParams(void);

// Interrupt methods

// Used primarily to determine if in interrupt context.
// Returns '0' if not in an interrupt, otherwise returns vector number
static MOS_INLINE u32 MOS_ISR_SAFE MosGetIRQNumber(void) {
    u32 irq;
    asm volatile (
        "mrs %0, ipsr"
            : "=r" (irq)
    );
    return irq;
}

// Nestable disable and enable interrupt methods, must be
// used in a balanced fashion like a recursive mutex.
MOS_ISR_SAFE void MosDisableInterrupts(void);
MOS_ISR_SAFE void MosEnableInterrupts(void);

// Time and Delays

MOS_ISR_SAFE u32 MosGetTickCount(void);
// For high-resolution time
MOS_ISR_SAFE u64 MosGetCycleCount(void);

void MosAdvanceTickCount(u32 ticks);

// Delay thread a number of ticks, zero yields thread.
void MosDelayThread(u32 ticks);
// For short delays, e.g.: useful for bit-banging.
//   Keep in mind there is an upper limit to usec.
MOS_ISR_SAFE void MosDelayMicroSec(u32 usec);

// Timers - Call specified callback at a period of time

void MosInitTimer(MosTimer * tmr, MosTimerCallback * callback);
void MosSetTimer(MosTimer * tmr, u32 ticks, void * priv_data);
void MosCancelTimer(MosTimer * tmr);
void MosResetTimer(MosTimer * tmr);

// Thread Functions

// Can use MosYieldThread() for cooperative multitasking
MOS_ISR_SAFE void MosYieldThread(void);
MosThread * MosGetThreadPtr(void);

// To get the current stack depth of current thread
static MOS_INLINE u32 MosGetStackDepth(u8 * top) {
    u32 sp;
    asm volatile (
        "mrs %0, psp"
                : "=r" (sp)
    );
    return ((u32) top) - sp;
}

// Thread stack methods
void MosGetStackStats(MosThread * thd, u32 * stack_size, u32 * stack_usage, u32 * max_stack_usage);
u8 * MosGetStackBottom(MosThread * thd);
u32 MosGetStackSize(MosThread * thd);
void MosSetStack(MosThread * thd, u8 * stack_bottom, u32 stack_size);

void MosSetThreadName(MosThread * thd, const char * name);
bool MosInitThread(MosThread * thd, MosThreadPriority pri, MosThreadEntry * entry,
                   s32 arg, u8 * stack_bottom, u32 stack_size);
bool MosRunThread(MosThread * thd);
bool MosInitAndRunThread(MosThread * thd, MosThreadPriority pri,
                         MosThreadEntry * entry, s32 arg, u8 * stack_bottom,
                         u32 stack_size);

// Obtain thread state and priority
MosThreadState MosGetThreadState(MosThread * thd, s32 * rtn_val);
MosThreadPriority MosGetThreadPriority(MosThread * thd);

// Change thread priority
void MosChangeThreadPriority(MosThread * thd, MosThreadPriority pri);

// For requesting normal thread stop.  Note that normal thread stop does NOT result
// in the invocation of the termination handler.
void MosRequestThreadStop(MosThread * thd);
// A thread can poll for stop requests so it may return naturally from its initial
// entry point.
bool MosIsStopRequested(void);
// Waits for thread stop or termination.  If a thread terminates abnormally this is
// invoked AFTER the termination handler.
s32 MosWaitForThreadStop(MosThread * thd);
bool MosWaitForThreadStopOrTO(MosThread * thd, s32 * rtn_val, u32 ticks);

// Forcible stop, works on blocked threads, results in invocation of termination handler.
void MosKillThread(MosThread * thd);
// Sets handler to run if thread is killed or dies via exception.  Thread can set its own
// termination handler entry and/or argument.  If entry is null it will use the default
// termination handler.  Another thread can use the return value from MosWaitForThreadStop()
// to detect abnormal termination of a thread.
void MosSetTermHandler(MosThread * thd, MosThreadEntry * entry, s32 arg);
void MosSetTermArg(MosThread * thd, s32 arg);

// Blocking Recursive Mutex with priority inheritance

void MosInitMutex(MosMutex * mtx);
void MosLockMutex(MosMutex * mtx);
bool MosTryMutex(MosMutex * mtx);
void MosUnlockMutex(MosMutex * mtx);
// Release mutex if owned (useful in termination handlers)
void MosRestoreMutex(MosMutex * mtx);
bool MosIsMutexOwner(MosMutex * mtx);

// Blocking Semaphores (intended for signaling)

void MosInitSem(MosSem * sem, u32 start_value);

//   (1) Counting Semaphore

// Returns false on timeout, true if taken
void MosWaitForSem(MosSem * sem);
bool MosWaitForSemOrTO(MosSem * sem, u32 ticks);
MOS_ISR_SAFE bool MosTrySem(MosSem * sem);
MOS_ISR_SAFE void MosIncrementSem(MosSem * sem);

//   (2) A Signal is a ganged 32-bit binary semaphore
//       Single-reader / multiple-writer
//       zero is returned for timeout or nothing polled

u32 MosWaitForSignal(MosSem * sem);
u32 MosWaitForSignalOrTO(MosSem * sem, u32 ticks);
MOS_ISR_SAFE u32 MosPollSignal(MosSem * sem);
MOS_ISR_SAFE void MosRaiseSignal(MosSem * sem, u32 flags);

//   (3) Binary semaphores are 1-bit signals

static MOS_INLINE void MosWaitForBinarySem(MosSem * sem) {
    MosWaitForSignal(sem);
}
static MOS_INLINE bool MosWaitForBinarySemOrTO(MosSem * sem, u32 ticks) {
    return MosWaitForSignalOrTO(sem, ticks);
}
MOS_ISR_SAFE static MOS_INLINE bool MosPollBinarySem(MosSem * sem) {
    return MosPollSignal(sem);
}
MOS_ISR_SAFE static MOS_INLINE void MosRaiseBinarySem(MosSem * sem) {
    MosRaiseSignal(sem, 1);
}

void MosAssertAt(char * file, u32 line);
#ifdef DEBUG
  #define MosAssert(c) { if (!(c)) MosAssertAt(__FILE__, __LINE__); }
#else
  #define MosAssert(c)
#endif

#endif
