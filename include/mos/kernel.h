
// Copyright 2019-2022 Matthew C Needes
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
    s32       refCnt;        /// Reference counter, used by thread_heap.c
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
    MosLinkHet         tmrLink;
    MosTimerCallback * pCallback;   /// Callback function
    void             * pUser;       /// User data pointer for callback
} MosTimer;

/// Initialize MOS Microkernel.
/// In general this call must precede all other calls into the MOS microkernel.
/// \note The ARM SysTick (system tick) and interrupt priority group settings should be
///       configured prior to this call.
void MosInit(void);

/// Run Scheduler.
/// Enables multi-threading, running all threads that been started prior to its
/// to invocation. Any code following this call is not reachable.
void MosRunScheduler(void);

// Hooks

void MosRegisterRawVPrintfHook(MosRawVPrintfHook * pHook, char (*buffer)[MOS_PRINT_BUFFER_SIZE]);
void MosRegisterSleepHook(MosSleepHook * pHook);
void MosRegisterWakeHook(MosWakeHook * pHook);
void MosRegisterEventHook(MosEventHook * pHook);

// Time and Timers

/// Obtain the lower half of the tick count.
///
MOS_ISR_SAFE u32 MosGetTickCount(void);
/// Get the monotonic cycle counter.
///
MOS_ISR_SAFE u64 MosGetCycleCount(void);
/// Advance the tick counter.
///
void MosAdvanceTickCount(u32 ticks);
/// Delay for a number of microseconds, e.g.: useful for bit-banging.
///   \note There is an upper limit for usec that is clock-speed dependent.
MOS_ISR_SAFE void MosDelayMicroSec(u32 usec);
/// Initialize a timer instance.
///    Supply a ISR Safe callback function to be called upon timer expiration.
void MosInitTimer(MosTimer * pTmr, MosTimerCallback * pCallback);
/// Set Timer to expire after a number of ticks.
///
void MosSetTimer(MosTimer * pTmr, u32 ticks, void * pUser);
/// Cancel running timer.
///
void MosCancelTimer(MosTimer * pTmr);
/// Reset (Restart) timer.
///
void MosResetTimer(MosTimer * pTmr);

// Thread Functions

/// Obtain pointer to currently running thread.
///
MosThread * MosGetThreadPtr(void);
/// Delay thread a number of ticks, zero input yields thread (see MosYieldThread).
///
void MosDelayThread(u32 ticks);
/// Yield to another thread of same priority.
/// \note Thread yields can be used for cooperative multitasking between threads of the same priority.
static MOS_INLINE void MosYieldThread(void) {
    MosDelayThread(0);
}
/// Get stack usage statistics for given thread.
///
void MosGetStackStats(MosThread * pThd, u32 * pStackSize, u32 * pStackUsage, u32 * pMaxStackUsage);
/// Get pointer to bottom of memory for the given thread's stack.
///
u8 * MosGetStackBottom(MosThread * pThd);
/// Get size of given thread's stack in bytes.
///
u32 MosGetStackSize(MosThread * pThd);
/// Set thread stack to the given stack with the given size.
///
void MosSetStack(MosThread * pThd, u8 * pStackBottom, u32 stackSize);
/// Obtain current stack depth of currently running thread.
///
static MOS_INLINE u32 MosGetStackDepth(u8 * pTop) {
    u32 sp;
    asm volatile (
        "mrs %0, psp"
                : "=r" (sp)
    );
    return ((u32)pTop) - sp;
}
/// Set pointer to the thread's name.
///
void MosSetThreadName(MosThread * pThd, const char * pName);
/// Initialize a thread instance, but do not start.
///
bool MosInitThread(MosThread * pThd, MosThreadPriority pri, MosThreadEntry * pEntry,
                   s32 arg, u8 * pStackBottom, u32 stackSize);
/// Run a thread that has been initialized via MosInitThread() or MosAllocThread().
///
bool MosRunThread(MosThread * pThd);
/// Initialize and start a thread.
///
bool MosInitAndRunThread(MosThread * pThd, MosThreadPriority pri,
                         MosThreadEntry * pEntry, s32 arg, u8 * pStackBottom,
                         u32 stackSize);
/// Obtain thread state and priority.
///
MosThreadState MosGetThreadState(MosThread * pThd, s32 * pRtnVal);
/// Get current priority for given thread.
///
MosThreadPriority MosGetThreadPriority(MosThread * pThd);
/// Change thread priority.
///
void MosChangeThreadPriority(MosThread * pThd, MosThreadPriority pri);
/// Waits for thread stop or termination. If a thread terminates abnormally this is
/// invoked AFTER the termination handler.
s32 MosWaitForThreadStop(MosThread * pThd);
bool MosWaitForThreadStopOrTO(MosThread * pThd, s32 * pRtnVal, u32 ticks);
/// Forcible stop, works on blocked threads, results in invocation of termination handler.
///
void MosKillThread(MosThread * pThd);
/// Sets handler to run if thread is killed or dies via exception. Thread can set its own
/// termination handler entry and/or argument. If entry is null it will use the default
/// termination handler. Another thread can use the return value from MosWaitForThreadStop()
/// to detect abnormal termination of a thread. Termination handlers can be used to recover
/// resources or restart the original thread.
void MosSetTermHandler(MosThread * pThd, MosThreadEntry * pEntry, s32 arg);
void MosSetTermArg(MosThread * pThd, s32 arg);

// Blocking Recursive Mutex with priority inheritance

void MosInitMutex(MosMutex * pMtx);
void MosLockMutex(MosMutex * pMtx);
bool MosTryMutex(MosMutex * pMtx);
void MosUnlockMutex(MosMutex * pMtx);
// Release mutex if owned (useful in termination handlers)
void MosRestoreMutex(MosMutex * pMtx);
bool MosIsMutexOwner(MosMutex * pMtx);

// Blocking Semaphores (intended for signaling)

void MosInitSem(MosSem * pSem, u32 startValue);

// (1) Counting Semaphore

// Returns false on timeout, true if taken
void MosWaitForSem(MosSem * pSem);
bool MosWaitForSemOrTO(MosSem * pSem, u32 ticks);
MOS_ISR_SAFE bool MosTrySem(MosSem * pSem);
MOS_ISR_SAFE void MosIncrementSem(MosSem * pSem);

// (2) A Signal is a set of 32 single-bit binary semaphores grouped in a u32 word
//     Operation is single-reader / multiple-writer
//     zero is returned for timeout or nothing polled
//     Can be used for receiving data on multiple prioritized queues
static MOS_INLINE void MosInitSignal(MosSignal * pSignal, u32 startValue) {
    MosInitSem(pSignal, startValue);
}
u32 MosWaitForSignal(MosSignal * pSignal);
u32 MosWaitForSignalOrTO(MosSignal * pSignal, u32 ticks);
MOS_ISR_SAFE u32 MosPollSignal(MosSignal * pSignal);
MOS_ISR_SAFE void MosRaiseSignal(MosSignal * pSignal, u32 flags);

/// Raise signal on a channel. A channel corresponds to one bit in a signal.
///
MOS_ISR_SAFE static MOS_INLINE void MosRaiseSignalForChannel(MosSignal * pSignal, u16 channel) {
    MosRaiseSignal(pSignal, 1 << channel);
}

/// Obtain next set channel from flags.
/// \return highest priority channel set in flags or -1 if no channel
MOS_ISR_SAFE static MOS_INLINE s16 MosGetNextChannelFromFlags(u32 * pFlags) {
    if (*pFlags == 0) return -1;
    return (s16)__builtin_ctz(*pFlags);
}
/// Clear flag associated with channel.
///
MOS_ISR_SAFE static MOS_INLINE void MosClearChannelFlag(u32 * pFlags, s16 channel) {
    if (channel >= 0) *pFlags &= ~(1 << channel);
}

// (3) Binary semaphores are 1-bit signals

static MOS_INLINE void MosWaitForBinarySem(MosSem * pSem) {
    MosWaitForSignal(pSem);
}
static MOS_INLINE bool MosWaitForBinarySemOrTO(MosSem * pSem, u32 ticks) {
    return MosWaitForSignalOrTO(pSem, ticks);
}
MOS_ISR_SAFE static MOS_INLINE bool MosPollBinarySem(MosSem * pSem) {
    return MosPollSignal(pSem);
}
MOS_ISR_SAFE static MOS_INLINE void MosRaiseBinarySem(MosSem * pSem) {
    MosRaiseSignal(pSem, 1);
}

void MosAssertAt(char * pFile, u32 line);
#ifdef DEBUG
  #define MosAssert(c) { if (!(c)) MosAssertAt(__FILE__, __LINE__); }
#else
  #define MosAssert(c)
#endif

#endif
