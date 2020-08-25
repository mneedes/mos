
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Microkernel
//

#ifndef _MOS_KERNEL_H_
#define _MOS_KERNEL_H_

#include "mos_config.h"
#include "mos/defs.h"
#include "mos/list.h"

// Microkernel Parameters
typedef struct {
    char * version;
    MosThreadPriority thread_pri_hi;
    MosThreadPriority thread_pri_low;
    u32 int_pri_hi;
    u32 int_pri_low;
    u32 micro_sec_per_tick;
    bool fp_support_en;
} MosParams;

// Mos Thread (opaque container)
typedef struct {
    u32 rsvd[18];
} MosThread;

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

typedef enum {
    MOS_EVENT_SCHEDULER_ENTRY,
    MOS_EVENT_SCHEDULER_EXIT,
    MOS_EVENT_TICK
} MosEvent;

// Blocking mutex supporting recursion
typedef struct {
    MosThread * owner;
    s32 depth;
    bool to_yield;
} MosMutex;

typedef struct {
    u32 count;
    MosThreadPriority block_pri;
} MosSem;

// Multi-writer / multi-reader blocking FIFO
typedef struct {
    MosSem sem_tail;
    MosSem sem_head;
    u32 * buf;
    u32 len;
    u32 tail;
    u32 head;
} MosQueue;

// Timers
typedef struct {
    u32 msg;
    u32 wake_tick;
    u32 ticks;
    MosQueue * q;
    MosListElm tmr_q;
} MosTimer;

// Allows blocking on multiple data structures simultaneously
typedef struct {
    MosWaitType type;
    union {
        MosSem * sem;
        MosQueue * q;
    } ptr;
} MosMuxEntry;

typedef struct {
    u32 num;
    MosMuxEntry * entries;
} MosMux;

typedef s32 (MosThreadEntry)(s32 arg);
typedef s32 (MosHandler)(s32 arg);
typedef void (MosRawPrintfHook)(const char * fmt, ...);
typedef void (MosSleepHook)(void);
typedef void (MosWakeHook)(void);
typedef void (MosEventHook)(MosEvent evt, u32 val);
typedef void (MosThreadFreeHook)(MosThread *); // TODO: Not implemented yet

// IS (Interrupt Safe) means the function can be called from ISRs.
// It may make sense to disable interrupts when calling those functions.

// Initialize and Run
// NOTE: SysTick and NVIC priority groups should be set up by HAL before running Init.
void MosInit(void);
void MosRunScheduler(void);

// Hooks
void MosRegisterRawPrintfHook(MosRawPrintfHook * hook);
void MosRegisterSleepHook(MosSleepHook * hook);
void MosRegisterWakeHook(MosWakeHook * hook);
void MosRegisterEventHook(MosEventHook * hook);
void MosRegisterThreadFreeHook(MosThreadFreeHook * hook);

// Obtain Microkernel parameters
const MosParams * MosGetParams(void);

// Interrupt methods

// Used primarily to determine if in interrupt context.
// Returns '0' if not in an interrupt, otherwise returns vector number
u32 MosGetIRQNumber(void); // IS
void MosDisableInterrupts(void); // IS
void MosEnableInterrupts(void); // IS

// Time and Delays

u32 MosGetTickCount(void);
void MosDelayThread(u32 ticks);
// For short delays, e.g.: useful for bit-banging.
//   Keep in mind there is an upper limit to usec.
void MosDelayMicroSec(u32 usec); // IS

// Timers - Write specified message to queue at appointed time

void MosInitTimer(MosTimer * timer, MosQueue * q);
void MosSetTimer(MosTimer * timer, u32 ticks, u32 msg);
void MosCancelTimer(MosTimer * timer);
void MosResetTimer(MosTimer * timer);

// Thread Functions

// Can use MosYieldThread() for cooperative multitasking
void MosYieldThread(void); // IS
MosThread * MosGetThread(void);
u8 * MosGetStackBottom(MosThread * thd);
u32 MosGetStackSize(MosThread * thd);
void MosSetStack(MosThread * thd, u8 * stack_bottom, u32 stack_size);
bool MosInitThread(MosThread * thd, MosThreadPriority pri, MosThreadEntry * entry,
                   s32 arg, u8 * stack_bottom, u32 stack_size);
bool MosRunThread(MosThread * thd);
bool MosInitAndRunThread(MosThread * thd, MosThreadPriority pri,
                         MosThreadEntry * entry, s32 arg, u8 * stack_bottom,
                         u32 stack_size);
MosThreadState MosGetThreadState(MosThread * thd, s32 * rtn_val);
void MosChangeThreadPriority(MosThread * thd, MosThreadPriority pri);
void MosRequestThreadStop(MosThread * thd);
bool MosIsStopRequested(void);
s32 MosWaitForThreadStop(MosThread * thd);
bool MosWaitForThreadStopOrTO(MosThread * thd, s32 * rtn_val, u32 ticks);
// Forcible stop, works on blocked threads.
void MosKillThread(MosThread * thd);
// Handler to run if thread is killed.  Thread can set own handler and argument.
void MosSetKillHandler(MosThread * thd, MosHandler * handler, s32 arg);
void MosSetKillArg(MosThread * thd, s32 arg);

// Blocking Recursive Mutex with priority inheritance

void MosInitMutex(MosMutex * mtx);
void MosTakeMutex(MosMutex * mtx);
bool MosTryMutex(MosMutex * mtx);
void MosGiveMutex(MosMutex * mtx);
// Release mutex if owned (useful in kill handlers)
void MosRestoreMutex(MosMutex * mtx);
bool MosIsMutexOwner(MosMutex * mtx);

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

// Mux (Block on multiple "selected" queues and/or semaphores)
// NOTE: An active Mux should only be changed by the thread using it

void MosInitMux(MosMux * mux);
void MosSetActiveMux(MosMux * mux, MosMuxEntry * entries, u32 len);
u32 MosWaitOnMux(MosMux * mux);
// Returns false on timeout, true if pending
bool MosWaitOnMuxOrTO(MosMux * mux, u32 * idx, u32 ticks);

#define MosAssert(c) { if (!(c)) MosAssertAt(__FILE__, __LINE__); }
void MosAssertAt(char * file, u32 line);

static u32 MOS_INLINE MosGetStackDepth(u8 * top) {
    u32 sp;
    asm volatile (
        "mrs %0, psp\n\t"
                : "=r" (sp)
    );
    return ((u32) top) - sp;
}

#endif
