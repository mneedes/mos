
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/kernel.h
/// \brief MOS Microkernel

#ifndef _MOS_KERNEL_H_
#define _MOS_KERNEL_H_

#include <mos_config.h>

#include <mos/defs.h>
#include <mos/arch.h>
#include <mos/list.h>

enum {
    MOS_THREAD_PRIORITY_HI = 0,
    MOS_THREAD_PRIORITY_LO = MOS_MAX_THREAD_PRIORITIES - 1,
    MOS_HW_FLOAT_SUPPORT   = MOS_FP_LAZY_CONTEXT_SWITCHING,
};

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
typedef MOS_ISR_SAFE bool (MosTimerCallback)(MosTimer * pTmr);
typedef void (MosRawVPrintfHook)(const char * pFmt, va_list args);
typedef void (MosSleepHook)(void);
typedef void (MosWakeHook)(void);
typedef void (MosEventHook)(MosEvent evt, u32 val);

// Mos Thread
typedef struct MosThread {
    u32       rsvd[21];
    void    * pUser;         /// User data pointer, set to NULL after thread initialization
    s32       refCnt;        /// Reference counter
} MosThread;

// Blocking mutex supporting recursion
typedef struct MosMutex {
    MosThread * pOwner;
    s32         depth;
    MosList     pendQ;
} MosMutex;

typedef struct MosSem {
    u32      value;
    MosList  pendQ;
    MosLink  evtLink;
} MosSem;

typedef MosSem MosSignal;

typedef struct MosTimer {
    u32                ticks;
    u32                wakeTick;
    MosPmLink         tmrLink;
    MosTimerCallback * pCallback;   /// Callback function
    void             * pUser;       /// User data pointer for callback
} MosTimer;

/// Initialize MOS Microkernel.
/// In general this call must precede all other calls into the MOS microkernel.
/// \note The ARM SysTick (system tick) and interrupt priority group settings should be
///       configured prior to this call.
void mosInit(void);

/// Run Scheduler.
/// Enables multi-threading, running all threads that been started prior to its
/// to invocation. Any code following this call is not reachable.
void mosRunScheduler(void);

// Hooks

void mosRegisterRawVPrintfHook(MosRawVPrintfHook * pHook, char (*buffer)[MOS_PRINT_BUFFER_SIZE]);
void mosRegisterSleepHook(MosSleepHook * pHook);
void mosRegisterWakeHook(MosWakeHook * pHook);
void mosRegisterEventHook(MosEventHook * pHook);

// Time and Timers

/// Obtain the lower half of the tick count.
///
MOS_ISR_SAFE u32 mosGetTickCount(void);
/// Get the monotonic cycle counter.
///
MOS_ISR_SAFE u64 mosGetCycleCount(void);
/// Advance the tick counter.
///
void mosAdvanceTickCount(u32 ticks);
/// Delay for a number of microseconds, e.g.: useful for bit-banging.
///   \note There is an upper limit for usec that is clock-speed dependent.
MOS_ISR_SAFE void mosDelayMicroseconds(u32 usec);
/// Initialize a timer instance.
///    Supply a ISR Safe callback function to be called upon timer expiration.
void mosInitTimer(MosTimer * pTmr, MosTimerCallback * pCallback);
/// Set Timer to expire after a number of ticks.
///
void mosSetTimer(MosTimer * pTmr, u32 ticks, void * pUser);
/// Cancel running timer.
///
void mosCancelTimer(MosTimer * pTmr);
/// Reset (Restart) timer.
///
void mosResetTimer(MosTimer * pTmr);

// Thread Functions

/// Obtain pointer to currently running thread.
///
MosThread * mosGetRunningThread(void);
/// Delay thread a number of ticks, zero input yields thread (see mosYieldThread).
///
void mosDelayThread(u32 ticks);
/// Yield to another thread of same priority.
/// \note Thread yields can be used for cooperative multitasking between threads of the same priority.
static MOS_INLINE void mosYieldThread(void) {
    mosDelayThread(0);
}
/// Get stack usage statistics for given thread.
///
void mosGetStackStats(MosThread * pThd, u32 * pStackSize, u32 * pStackUsage, u32 * pMaxStackUsage);
/// Get pointer to bottom of memory for the given thread's stack.
///
u8 * mosGetStackBottom(MosThread * pThd);
/// Get size of given thread's stack in bytes.
///
u32 mosGetStackSize(MosThread * pThd);
/// Set thread stack to the given stack with the given size.
///
void mosSetStack(MosThread * pThd, u8 * pStackBottom, u32 stackSize);
/// Obtain current stack depth of currently running thread.
///
static MOS_INLINE u32 mosGetStackDepth(u8 * pTop) {
    u32 sp;
    asm volatile (
        "mrs %0, psp"
                : "=r" (sp)
    );
    return ((u32)pTop) - sp;
}
/// Set pointer to the thread's name.
///
void mosSetThreadName(MosThread * pThd, const char * pName);
/// Initialize a thread instance, but do not start.
///
bool mosInitThread(MosThread * pThd, MosThreadPriority pri, MosThreadEntry * pEntry,
                   s32 arg, u8 * pStackBottom, u32 stackSize);
/// Run a thread that has been initialized via mosInitThread() or mosAllocThread().
///
bool mosRunThread(MosThread * pThd);
/// Initialize and start a thread.
///
bool mosInitAndRunThread(MosThread * pThd, MosThreadPriority pri,
                         MosThreadEntry * pEntry, s32 arg, u8 * pStackBottom,
                         u32 stackSize);
/// Obtain thread state and priority.
///
MosThreadState mosGetThreadState(MosThread * pThd, s32 * pRtnVal);
/// Get current priority for given thread.
///
MosThreadPriority mosGetThreadPriority(MosThread * pThd);
/// Change thread priority.
///
void mosChangeThreadPriority(MosThread * pThd, MosThreadPriority pri);
/// Waits for thread stop or termination. If a thread terminates abnormally this is
/// invoked AFTER the termination handler.
s32 mosWaitForThreadStop(MosThread * pThd);
bool mosWaitForThreadStopOrTO(MosThread * pThd, s32 * pRtnVal, u32 ticks);
/// Forcible stop, works on blocked threads, results in invocation of termination handler.
///
void mosKillThread(MosThread * pThd);
/// Sets handler to run if thread is killed or dies via exception. Thread can set its own
/// termination handler entry and/or argument. If entry is null it will use the default
/// termination handler. Another thread can use the return value from mosWaitForThreadStop()
/// to detect abnormal termination of a thread. Termination handlers can be used to recover
/// resources or restart the original thread.
void mosSetTermHandler(MosThread * pThd, MosThreadEntry * pEntry, s32 arg);
void mosSetTermArg(MosThread * pThd, s32 arg);

// Blocking Recursive Mutex with priority inheritance

void mosInitMutex(MosMutex * pMtx);
void mosLockMutex(MosMutex * pMtx);
bool mosTryMutex(MosMutex * pMtx);
void mosUnlockMutex(MosMutex * pMtx);
// Release mutex if owned (useful in termination handlers)
void mosRestoreMutex(MosMutex * pMtx);
bool mosIsMutexOwner(MosMutex * pMtx);

// Blocking Semaphores (intended for signaling)

void mosInitSem(MosSem * pSem, u32 startValue);

// (1) Counting Semaphore

// Returns false on timeout, true if taken
void mosWaitForSem(MosSem * pSem);
bool mosWaitForSemOrTO(MosSem * pSem, u32 ticks);
MOS_ISR_SAFE bool mosTrySem(MosSem * pSem);
MOS_ISR_SAFE void mosIncrementSem(MosSem * pSem);

// (2) A Signal is a set of 32 single-bit binary semaphores grouped in a u32 word
//     Operation is single-reader / multiple-writer
//     zero is returned for timeout or nothing polled
//     Can be used for receiving data on multiple prioritized queues
static MOS_INLINE void mosInitSignal(MosSignal * pSignal, u32 startValue) {
    mosInitSem(pSignal, startValue);
}
u32 mosWaitForSignal(MosSignal * pSignal);
u32 mosWaitForSignalOrTO(MosSignal * pSignal, u32 ticks);
MOS_ISR_SAFE u32 mosPollSignal(MosSignal * pSignal);
MOS_ISR_SAFE void mosRaiseSignal(MosSignal * pSignal, u32 flags);

/// Raise signal on a channel. A channel corresponds to one bit in a signal.
///
MOS_ISR_SAFE static MOS_INLINE void mosRaiseSignalForChannel(MosSignal * pSignal, u16 channel) {
    mosRaiseSignal(pSignal, 1 << channel);
}

/// Obtain next set channel from flags.
/// \return highest priority channel set in flags or -1 if no channel
MOS_ISR_SAFE static MOS_INLINE s16 mosGetNextChannelFromFlags(u32 * pFlags) {
    if (*pFlags == 0) return -1;
    return (s16)__builtin_ctz(*pFlags);
}
/// Clear flag associated with channel.
///
MOS_ISR_SAFE static MOS_INLINE void mosClearChannelFlag(u32 * pFlags, s16 channel) {
    if (channel >= 0) *pFlags &= ~(1 << channel);
}

// (3) Binary semaphores are 1-bit signals

static MOS_INLINE void mosWaitForBinarySem(MosSem * pSem) {
    mosWaitForSignal(pSem);
}
static MOS_INLINE bool mosWaitForBinarySemOrTO(MosSem * pSem, u32 ticks) {
    return mosWaitForSignalOrTO(pSem, ticks);
}
MOS_ISR_SAFE static MOS_INLINE bool mosPollBinarySem(MosSem * pSem) {
    return mosPollSignal(pSem);
}
MOS_ISR_SAFE static MOS_INLINE void mosRaiseBinarySem(MosSem * pSem) {
    mosRaiseSignal(pSem, 1);
}

/// Asserts induce crash if given condition is not satisfied.
///
void mosAssertAt(char * pFile, u32 line);
#define mosAssert(c) { if (!(c)) mosAssertAt(__FILE__, __LINE__); }

#endif
