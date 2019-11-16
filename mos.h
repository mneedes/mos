
//  Copyright 2019 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Microkernel
//

#ifndef _MOS_H_
#define _MOS_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "mos_config.h"

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

#define MOS_NAKED              __attribute__((naked))
#define MOS_INLINE             __attribute__((always_inline)) inline
#define MOS_USED               __attribute__((used))
#define MOS_OPT(x)             __attribute__((optimize(x)))
#define MOS_ALIGNED(x)         __attribute__((aligned(x)))

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

typedef s32 (MosThreadEntry)(s32 arg);
typedef s32 (MosHandlerEntry)(s32 arg);
typedef void (MosTimerHook)(u32 TickCount);
typedef s16 MosThreadID;
typedef u16 MosThreadPriority;
typedef volatile u32 MosSem;

// Microkernel Parameters
typedef struct {
    u32 max_app_threads;
    MosThreadPriority thread_pri_hi;
    MosThreadPriority thread_pri_low;
    u32 int_pri_hi;
    u32 int_pri_low;
    u32 micro_sec_per_tick;
} MosParams;

typedef enum {
    MOS_THREAD_NOT_STARTED,
    MOS_THREAD_RUNNING,
    MOS_THREAD_STOP_REQUEST,
    MOS_THREAD_STOPPED
} MosThreadState;

typedef enum {
    MOS_WAIT_DISABLED,
    MOS_WAIT_SEM,
    MOS_WAIT_RECV_QUEUE,
    MOS_WAIT_SEND_QUEUE
} MosWaitType;

// Blocking mutex supporting recursion
typedef struct {
    s32 owner;  // ThreadID stored in s32
    s32 depth;
    bool is_waiting;
} MosMutex;

// Doubly-linked lists (idea borrowed from famous OS)
typedef struct MosList {
    struct MosList * prev;
    struct MosList * next;
} MosList;

// Multi-writer / multi-reader blocking FIFO
typedef struct {
    MosSem sem_tail;
    MosSem sem_head;
    u32 * buf;
    u32 len;
    u32 tail;
    u32 head;
} MosQueue;

// Allows blocking on multiple data structures simultaneously
typedef struct {
    MosWaitType type;
    union {
        MosSem * sem;
        MosQueue * q;
    } ptr;
} MosWaitMuxEntry;

typedef struct {
    u16 num;
    MosWaitMuxEntry * entries;
} MosWaitMux;

// IS (Interrupt Safe) means the function can be called from ISRs.
// It may make sense to disable interrupts when calling those functions.

// Initialize and Run
void MosInit(void);
void MosRunScheduler(void);

// Obtain Microkernel parameters
const MosParams * MosGetParams(void);

// Interrupt methods

// Used primarily to determine if in interrupt context.
// Returns '0' if not in an interrupt, otherwise returns vector number
u32 MosGetIRQNumber(void); // IS
// In thread mode interrupt functions are recursion safe.
void MosDisableInterrupts(void); // IS (not recursion safe in ISR)
void MosEnableInterrupts(void); // IS (not recursion safe in ISR)

// Time and Delays
u32 MosGetTickCount(void);
void MosDelayThread(u32 ticks);
// For short delays, e.g.: for bit-banging.
//   Keep in mind there is an upper limit to usec.
void MosDelayMicroSec(u32 usec); // IS
void MosRegisterTimerHook(MosTimerHook * timer_hook);

// Thread Functions

// Can use MosYieldThread() for cooperative multitasking
void MosYieldThread(void); // IS
MosThreadID MosGetThreadID(void);
MosThreadState MosGetThreadState(MosThreadID id, s32 * rtn_val);
s32 MosInitThread(MosThreadID id, MosThreadPriority pri, MosThreadEntry * entry,
                  s32 arg, u8 * s_addr, u32 s_size);
s32 MosRunThread(MosThreadID id);
s32 MosInitAndRunThread(MosThreadID id, MosThreadPriority pri,
                        MosThreadEntry * entry, s32 arg, u8 * s_addr,
                        u32 s_size);
void MosChangeThreadPriority(MosThreadID id, MosThreadPriority pri);
void MosRequestThreadStop(MosThreadID id);
bool MosIsStopRequested(void);
s32 MosWaitForThreadStop(MosThreadID id);
bool MosWaitForThreadStopOrTO(MosThreadID id, s32 * rtn_val, u32 ticks);
// Forcible stop, works on blocked threads
void MosKillThread(MosThreadID id);
// Handler to run if thread is killed
void MosSetKillHandler(MosThreadID id, MosHandlerEntry * entry, s32 arg);

// Doubly-Linked Lists
void MosInitList(MosList * list); // IS
void MosAddToList(MosList * list, MosList * elm_add); // IS
static void MOS_INLINE
MosAddToListBefore(MosList * elm_exist, MosList * elm_add) { // IS
    // AddToList <=> AddToListBefore if used on element rather than list
    MosAddToList(elm_exist, elm_add);
}
void MosAddToListAfter(MosList * elm_exist, MosList * elm_add); // IS
static void MOS_INLINE
MosAddToFrontOfList(MosList * list, MosList * elm_add) { // IS
    // AddToListAfter <=> AddToFrontOfList if used on list rather than element
    MosAddToListAfter(list, elm_add);
}
void MosRemoveFromList(MosList * elm_rem); // IS
void MosMoveToEndOfList(MosList * elm_exist, MosList * elm_move); // IS
static bool MOS_INLINE MosIsLastElement(MosList * list, MosList * elm) { // IS
    return (list->prev == elm);
}
static bool MOS_INLINE MosIsListEmpty(MosList * list) { // IS
    return (list->prev == list);
}
static bool MOS_INLINE MosIsOnList(MosList * elm) { // IS
    return (elm->prev != elm);
}

// Blocking Recursive Mutex with priority inheritance
void MosInitMutex(MosMutex * mtx);
void MosTakeMutex(MosMutex * mtx);
bool MosTryMutex(MosMutex * mtx);
void MosGiveMutex(MosMutex * mtx);

// Blocking Semaphore, intended for signaling
void MosInitSem(MosSem * sem, u32 start_count);
void MosTakeSem(MosSem * sem);
// Returns false on timeout, true if taken
bool MosTakeSemOrTO(MosSem * sem, u32 ticks);
bool MosTrySem(MosSem * sem); // IS
void MosGiveSem(MosSem * sem); // IS

// Blocking Queue
void MosInitQueue(MosQueue * queue, u32 * buf, u32 len);
bool MosSendToQueue(MosQueue * queue, u32 data); // IS
// Returns false on timeout, true if sent
bool MosSendToQueueOrTO(MosQueue * queue, u32 data, u32 ticks);
u32 MosReceiveFromQueue(MosQueue * queue);
bool MosTryReceiveFromQueue(MosQueue * queue, u32 * data);
// Returns false on timeout, true if received
bool MosReceiveFromQueueOrTO(MosQueue * queue, u32 * data, u32 ticks);

// Wait Mux (Block on multiple "selected" queues and/or semaphores)
// NOTE: An active Mux should only be changed by the thread using it
void MosInitWaitMux(MosWaitMux * mux);
void MosSetActiveMux(MosWaitMux * mux, MosWaitMuxEntry * entries, u32 len);
u32 MosWaitOnMux(MosWaitMux * mux);
// Returns false on timeout, true if pending
bool MosWaitOnMuxOrTO(MosWaitMux * mux, u32 * idx, u32 ticks);

#endif
