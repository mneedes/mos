
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel
//

#include <bsp_hal.h>
#include <mos/kernel.h>
#include <mos/arch.h>
#include <errno.h>

// TODO: multi-level priority inheritance / multiple mutexes at the same time
// TODO: Waiting on multiple semaphores
// TODO: Change wait queue position on priority change
// TODO: Hooks for other timers such as LPTIM ?
// TODO: Independence from cmsis

#define NO_SUCH_THREAD       NULL
#define STACK_FILL_VALUE     0xca5eca11

/* The parameter is really used, but tell compiler it is unused to reject warnings */
#define MOS_USED_PARAM(x)    MOS_UNUSED(x)

#define EVENT(e, v) \
    { if (MOS_ENABLE_EVENTS) (*EventHook)((MOS_EVENT_ ## e), (v)); }

// Element types for heterogeneous lists
enum {
    ELM_THREAD,
    ELM_TIMER
};

typedef struct {
    u32 SWSAVE[8];   // R4-R11
    u32 LR_EXC_RTN;  // Exception Return LR
    u32 HWSAVE[4];   // R0-R3
    u32 R12;         // R12
    u32 LR;          // R14
    u32 PC;          // R15
    u32 PSR;         // xPSR
} StackFrame;

typedef enum {
    THREAD_STATE_BASE_MASK = 0xffffff00,
    THREAD_STATE_BASE      = 0xba55ba00,
    THREAD_STATE_TICK      = 16,
    THREAD_UNINIT          = THREAD_STATE_BASE,
    THREAD_INIT,
    THREAD_STOPPED,
    THREAD_TIME_TO_STOP,
    THREAD_RUNNABLE,
    THREAD_WAIT_FOR_MUTEX,
    THREAD_WAIT_FOR_SEM,
    THREAD_WAIT_FOR_STOP,
    THREAD_WAIT_FOR_TICK         = THREAD_STATE_BASE + THREAD_STATE_TICK,
    THREAD_WAIT_FOR_SEM_OR_TICK  = THREAD_WAIT_FOR_SEM + THREAD_STATE_TICK,
    THREAD_WAIT_FOR_STOP_OR_TICK = THREAD_WAIT_FOR_STOP + THREAD_STATE_TICK,
} ThreadState;

typedef struct Thread {
    u32                 sp;
    error_t             err_no;
    ThreadState         state;
    MosLink             run_link;
    MosLinkHet          tmr_link;
    MosList             stop_q;
    u32                 wake_tick;
    MosThreadPriority   pri;
    MosThreadPriority   nom_pri;
    u8                  stop_request;
    u8                  timed_out;
    s32                 rtn_val;
    MosThreadEntry    * term_handler;
    s32                 term_arg;
    u8                * stack_bottom;
    u32                 stack_size;
    const char        * name;
    s32                 ref_cnt;
} Thread;

// Ensure opaque thread structure has same size as internal structure
MOS_STATIC_ASSERT(Thread, sizeof(Thread) == sizeof(MosThread));

typedef union {
    u64 count;
    struct {
        u32 lower;
        u32 upper;
    };
} Ticker;

// Parameters
static u8 IntPriMaskLow;
static MosParams Params = {
   .version            = MOS_TO_STR(MOS_VERSION),
   .thread_pri_hi      = 0,
   .thread_pri_low     = MOS_MAX_THREAD_PRIORITIES - 1,
   .int_pri_hi         = 0,
   .int_pri_low        = 0,
   .micro_sec_per_tick = MOS_MICRO_SEC_PER_TICK,
   .fp_support_en      = MOS_FP_LAZY_CONTEXT_SWITCHING
};

// Hooks
static MosRawPrintfHook * PrintfHook = NULL;
static MosSleepHook * SleepHook = NULL;
static MosWakeHook * WakeHook = NULL;
static void DummyEventHook(MosEvent e, u32 v) { MOS_UNUSED(e); MOS_UNUSED(v); }
static MosEventHook * EventHook = DummyEventHook;

// Threads and Events
static Thread * RunningThread = NO_SUCH_THREAD;
static error_t * ErrNo;
static Thread IdleThread;
static MosList RunQueues[MOS_MAX_THREAD_PRIORITIES];
static MosList ISREventQueue;
static u32 IntDisableCount = 0;

// Timers and Ticks
static MosList TimerQueue;
static volatile Ticker MOS_ALIGNED(8) Tick = { .count = 1 };
static s32 MaxTickInterval;
static u32 CyclesPerTick;
static u32 MOS_USED CyclesPerMicroSec;

// Idle thread stack and initial dummy PSP stack frame storage
//  28 words is enough for a FP stack frame,
//     add a stack frame for idle thread stack initialization.
static u8 MOS_STACK_ALIGNED IdleStack[112 + sizeof(StackFrame)];

const MosParams * MosGetParams(void) {
    return (const MosParams *) &Params;
}

void MosRegisterRawPrintfHook(MosRawPrintfHook * hook) { PrintfHook = hook; }
void MosRegisterSleepHook(MosSleepHook * hook) { SleepHook = hook; }
void MosRegisterWakeHook(MosWakeHook * hook) { WakeHook = hook; }
void MosRegisterEventHook(MosEventHook * hook) { EventHook = hook; }

#if (MOS_ARCH == MOS_ARM_V6M)

static MOS_INLINE void LockScheduler(u32 pri) {
    MOS_UNUSED(pri);
    asm volatile ( "cpsid if" );
}

static MOS_INLINE void UnlockScheduler(void) {
    asm volatile ( "cpsie if" );
}

#elif (MOS_ARCH == MOS_ARM_V7M)

// Mask interrupts by priority, primarily for temporarily
//   disabling context switches.

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

void MOS_ISR_SAFE MosDisableInterrupts(void) {
    if (IntDisableCount++ == 0) {
        asm volatile ( "cpsid if" );
    }
}

void MOS_ISR_SAFE MosEnableInterrupts(void) {
    if (IntDisableCount == 0) return;
    if (--IntDisableCount == 0) {
        asm volatile ( "cpsie if" );
    }
}

static MOS_INLINE void SetThreadState(Thread * thd, ThreadState state) {
    asm volatile ( "dmb" );
    thd->state = state;
}

static MOS_INLINE MOS_ISR_SAFE void YieldThread(void) {
    // Invoke PendSV handler to potentially perform context switch
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    asm volatile ( "isb" );
}

static MOS_INLINE void SetRunningThreadStateAndYield(ThreadState state) {
    asm volatile ( "dmb" );
    LockScheduler(IntPriMaskLow);
    RunningThread->state = state;
    YieldThread();
    UnlockScheduler();
}

void MosAssertAt(char * file, u32 line) {
    if (PrintfHook) (*PrintfHook)("Assertion failed in %s on line %u\n", file, line);
    if (RunningThread != NO_SUCH_THREAD)
        SetRunningThreadStateAndYield(THREAD_TIME_TO_STOP);
    // not always reachable
    while (1);
}

//
// Time / Timers
//

u32 MosGetTickCount(void) {
    return Tick.lower;
}

u64 MosGetCycleCount(void) {
    asm volatile ( "cpsid if" );
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) Tick.count++;
    s64 tmp = Tick.count;
    u32 val = SysTick->VAL;
    asm volatile ( "cpsie if" );
    return (tmp * CyclesPerTick) - val;
}

void MosAdvanceTickCount(u32 ticks) {
    if (ticks) {
        asm volatile ( "cpsid if" );
        Tick.count += ticks;
        SCB->ICSR = SCB_ICSR_PENDSTSET_Msk;
        asm volatile ( "cpsie if" );
    }
}

static void MOS_USED SetTimeout(u32 ticks) {
    RunningThread->wake_tick = Tick.lower + ticks;
}

void MOS_NAKED MosDelayMicroSec(u32 usec) {
    MOS_UNUSED(usec);
    asm volatile (
        ".syntax unified\n"
        "ldr r1, _CyclesPerMicroSec\n"
        "ldr r1, [r1]\n"
        "muls r0, r0, r1\n"
        "subs r0, #13\n"  // Overhead calibration
        "delay:\n"
        // It is possible that 6 is another valid value, non-cached flash stall?
#if (MOS_CYCLES_PER_INNER_LOOP == 3)
        "subs r0, r0, #3\n"
#elif (MOS_CYCLES_PER_INNER_LOOP == 1)
        "subs r0, r0, #1\n"
#else
#error "Invalid selection for inner loop cycles"
#endif
        "bgt delay\n"
        "bx lr\n"
        ".balign 4\n"
        "_CyclesPerMicroSec: .word CyclesPerMicroSec"
            : : : "r0", "r1"
    );
}

void MosInitTimer(MosTimer * tmr, MosTimerCallback * callback) {
    MosInitLinkHet(&tmr->tmr_link, ELM_TIMER);
    tmr->callback = callback;
}

static void AddTimer(MosTimer * tmr) {
    // NOTE: Must lock scheduler before calling
    MosLink * elm;
    u32 tick_count = MosGetTickCount();
    tmr->wake_tick = tick_count + tmr->ticks;
    for (elm = TimerQueue.next; elm != &TimerQueue; elm = elm->next) {
        if (((MosLinkHet *)elm)->type == ELM_THREAD) {
            Thread * thd = container_of(elm, Thread, tmr_link);
            s32 tmr_rem_ticks = (s32)thd->wake_tick - tick_count;
            if ((s32)tmr->ticks <= tmr_rem_ticks) break;
        } else {
            MosTimer * tmr_tmr = container_of(elm, MosTimer, tmr_link);
            s32 tmr_rem_ticks = (s32)tmr_tmr->wake_tick - tick_count;
            if ((s32)tmr->ticks <= tmr_rem_ticks) break;
        }
    }
    MosAddToListBefore(elm, &tmr->tmr_link.link);
}

void MosSetTimer(MosTimer * tmr, u32 ticks, void * priv_data) {
    LockScheduler(IntPriMaskLow);
    tmr->ticks = ticks;
    tmr->priv_data = priv_data;
    AddTimer(tmr);
    UnlockScheduler();
}

void MosCancelTimer(MosTimer * tmr) {
    LockScheduler(IntPriMaskLow);
    if (MosIsOnList(&tmr->tmr_link.link))
        MosRemoveFromList(&tmr->tmr_link.link);
    UnlockScheduler();
}

void MosResetTimer(MosTimer * tmr) {
    LockScheduler(IntPriMaskLow);
    if (MosIsOnList(&tmr->tmr_link.link))
        MosRemoveFromList(&tmr->tmr_link.link);
    AddTimer(tmr);
    UnlockScheduler();
}

//
// Threads
//

void MosDelayThread(u32 ticks) {
    if (ticks) {
        SetTimeout(ticks);
        SetRunningThreadStateAndYield(THREAD_WAIT_FOR_TICK);
    } else YieldThread();
}

// ThreadExit is invoked when a thread stops (returns from its natural entry point)
//   or after its termination handler returns (kill or exception)
static s32 ThreadExit(s32 rtn_val) {
    LockScheduler(IntPriMaskLow);
    RunningThread->rtn_val = rtn_val;
    SetThreadState(RunningThread, THREAD_STOPPED);
    asm volatile ( "dmb" );
    if (MosIsOnList(&RunningThread->tmr_link.link))
        MosRemoveFromList(&RunningThread->tmr_link.link);
    MosLink * elm_save;
    for (MosLink * elm = RunningThread->stop_q.next;
             elm != &RunningThread->stop_q; elm = elm_save) {
        elm_save = elm->next;
        Thread * thd = container_of(elm, Thread, run_link);
        MosRemoveFromList(elm);
        MosAddToList(&RunQueues[thd->pri], &thd->run_link);
        if (MosIsOnList(&thd->tmr_link.link))
            MosRemoveFromList(&thd->tmr_link.link);
        SetThreadState(thd, THREAD_RUNNABLE);
    }
    MosRemoveFromList(&RunningThread->run_link);
    YieldThread();
    UnlockScheduler();
    // Not reachable
    MosAssert(0);
    return 0;
}

static void InitThread(Thread * thd, MosThreadPriority pri,
                       MosThreadEntry * entry, s32 arg,
                       u8 * stack_bottom, u32 stack_size) {
    u8 * sp = stack_bottom;
    // Ensure 8-byte alignment for ARM / varargs compatibility.
    sp = (u8 *) ((u32)(sp + stack_size - sizeof(u32)) & 0xfffffff8);
    // Place canary value at top and fill out initial frame
    *((u32 *)sp) = STACK_FILL_VALUE;
    StackFrame * sf = (StackFrame *)sp - 1;
    sf->PSR = 0x01000000;
    sf->PC = (u32)entry;
    sf->LR = (u32)ThreadExit;
    sf->R12 = 0;
    sf->HWSAVE[0] = arg;
    sf->LR_EXC_RTN = DEFAULT_EXC_RETURN;
    // Either fill lower stack OR just place canary value at bottom
    if (MOS_STACK_USAGE_MONITOR) {
        u32 * fill = (u32 *)sf - 1;
        for (; fill >= (u32 *)stack_bottom; fill--) {
            *fill = STACK_FILL_VALUE;
        }
    } else {
        *((u32 *)stack_bottom) = STACK_FILL_VALUE;
    }
    // Initialize context and state
    thd->sp = (u32)sf;
    thd->err_no = 0;
    thd->pri = pri;
    thd->nom_pri = pri;
    thd->stop_request = false;
    thd->term_handler = ThreadExit;
    thd->term_arg = 0;
    thd->stack_bottom = stack_bottom;
    thd->stack_size = stack_size;
    thd->name = "";
    // ref_cnt is not initialized here, it is manipulated externally
}

static s32 IdleThreadEntry(s32 arg) {
    MOS_UNUSED(arg);
    while (1) {
        // Disable interrupts and timer
        asm volatile ( "cpsid i" ::: "memory" );
        SysTick->CTRL = SYSTICK_CTRL_DISABLE;
        if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) Tick.count += 1;
        // Figure out how long to wait
        s32 tick_interval = MaxTickInterval;
        if (!MosIsListEmpty(&TimerQueue)) {
            if (((MosLinkHet *)TimerQueue.next)->type == ELM_THREAD) {
                Thread * thd = container_of(TimerQueue.next, Thread, tmr_link);
                tick_interval = (s32)thd->wake_tick - Tick.lower;
            } else {
                MosTimer * tmr = container_of(TimerQueue.next, MosTimer, tmr_link);
                tick_interval = (s32)tmr->wake_tick - Tick.lower;
            }
            if (tick_interval <= 0) {
                tick_interval = 1;
            } else if (tick_interval > MaxTickInterval) {
                tick_interval = MaxTickInterval;
            }
        }
        u32 load = 0;
        if (tick_interval != 1) {
            load = (tick_interval - 1) * CyclesPerTick + SysTick->VAL - 1;
            SysTick->LOAD = load;
            SysTick->VAL = 0;
        }
        if (SleepHook) (*SleepHook)();
        SysTick->CTRL = SYSTICK_CTRL_ENABLE;
        asm volatile (
            "dsb\n"
            "wfi" ::: "memory"
        );
        if (WakeHook) (*WakeHook)();
        if (load) {
            SysTick->CTRL = SYSTICK_CTRL_DISABLE;
            if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) {
                // If counter rolled over then account for all ticks
                SysTick->LOAD = CyclesPerTick - 1;
                SysTick->VAL = 0;
                Tick.count += tick_interval;
            } else {
                // Interrupt was early so account for elapsed ticks
                u32 adj_tick_interval = (load - SysTick->VAL) / CyclesPerTick;
                SysTick->LOAD = SysTick->VAL;
                SysTick->VAL = 0;
                SysTick->LOAD = CyclesPerTick - 1;
                Tick.count += adj_tick_interval; // or adj_tick_interval - 1 ?
            }
            SysTick->CTRL = SYSTICK_CTRL_ENABLE;
        }
        asm volatile ( "cpsie i\n"
                       "dsb\n"
                       "isb" ::: "memory" );
    }
    return 0;
}

void MOS_ISR_SAFE MosYieldThread(void) {
    if (RunningThread == NO_SUCH_THREAD) return;
    // Invoke PendSV handler to potentially perform context switch
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    asm volatile ( "isb" );
}

MosThread * MosGetThreadPtr(void) {
    return (MosThread *)RunningThread;
}

void MosGetStackStats(MosThread * _thd, u32 * stack_size, u32 * stack_usage, u32 * max_stack_usage) {
    Thread * thd = (Thread *)_thd;
    LockScheduler(IntPriMaskLow);
    *stack_size = thd->stack_size;
    u8 * stack_top = thd->stack_bottom + *stack_size;
    if (thd == RunningThread)
        *stack_usage = MosGetStackDepth(stack_top);
    else
        *stack_usage = stack_top - (u8 *)thd->sp;
    if (MOS_STACK_USAGE_MONITOR) {
        u32 * check = (u32 *)thd->stack_bottom;
        while (*check++ == STACK_FILL_VALUE);
        *max_stack_usage = stack_top - (u8 *)check + 4;
    } else {
        *max_stack_usage = *stack_usage;
    }
    UnlockScheduler();
}

u8 * MosGetStackBottom(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    u8 * stack_bottom = NULL;
    LockScheduler(IntPriMaskLow);
    if (thd) stack_bottom = thd->stack_bottom;
    else if (RunningThread) stack_bottom = RunningThread->stack_bottom;
    UnlockScheduler();
    return stack_bottom;
}

u32 MosGetStackSize(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    return thd->stack_size;
}

void MosSetStack(MosThread * _thd, u8 * stack_bottom, u32 stack_size) {
    Thread * thd = (Thread *)_thd;
    thd->stack_bottom = stack_bottom;
    thd->stack_size = stack_size;
}

void MosSetThreadName(MosThread * _thd, const char * name) {
    Thread * thd = (Thread *)_thd;
    thd->name = name;
}

bool MosInitThread(MosThread * _thd, MosThreadPriority pri,
                   MosThreadEntry * entry, s32 arg,
                   u8 * stack_bottom, u32 stack_size) {
    Thread * thd = (Thread *)_thd;
    if (thd == RunningThread) return false;
    LockScheduler(IntPriMaskLow);
    // Detect uninitialized thread state variable
    if ((thd->state & THREAD_STATE_BASE_MASK) != THREAD_STATE_BASE)
        thd->state = THREAD_UNINIT;
    // Check thread state
    switch (thd->state) {
    case THREAD_UNINIT:
    case THREAD_INIT:
        MosInitList(&thd->stop_q);
        // fall through
    case THREAD_STOPPED:
        MosInitList(&thd->run_link);
        MosInitLinkHet(&thd->tmr_link, ELM_THREAD);
        break;
    default:
        // Forcibly stop thread if running
        // This will run if thread is killed, stop queue
        //   is processed only after kill handler returns.
        if (MosIsOnList(&thd->tmr_link.link))
            MosRemoveFromList(&thd->tmr_link.link);
        // Lock because thread might be on semaphore pend queue
        asm volatile ( "cpsid if" );
        MosRemoveFromList(&thd->run_link);
        asm volatile ( "cpsie if" );
        break;
    }
    SetThreadState(thd, THREAD_UNINIT);
    UnlockScheduler();
    InitThread(thd, pri, entry, arg, stack_bottom, stack_size);
    SetThreadState(thd, THREAD_INIT);
    return true;
}

bool MosRunThread(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    if (thd->state == THREAD_INIT) {
        LockScheduler(IntPriMaskLow);
        SetThreadState(thd, THREAD_RUNNABLE);
        if (thd != &IdleThread)
            MosAddToList(&RunQueues[thd->pri], &thd->run_link);
        if (RunningThread != NO_SUCH_THREAD && thd->pri < RunningThread->pri)
            YieldThread();
        UnlockScheduler();
        return true;
    }
    return false;
}

bool MosInitAndRunThread(MosThread * _thd,  MosThreadPriority pri,
                         MosThreadEntry * entry, s32 arg, u8 * stack_bottom,
                         u32 stack_size) {
    if (!MosInitThread(_thd, pri, entry, arg, stack_bottom, stack_size))
        return false;
    return MosRunThread(_thd);
}

MosThreadState MosGetThreadState(MosThread * _thd, s32 * rtn_val) {
    Thread * thd = (Thread *)_thd;
    MosThreadState state;
    LockScheduler(IntPriMaskLow);
    switch (thd->state) {
    case THREAD_UNINIT:
    case THREAD_INIT:
        state = MOS_THREAD_NOT_STARTED;
        break;
    case THREAD_STOPPED:
        state = MOS_THREAD_STOPPED;
        if (rtn_val != NULL) *rtn_val = thd->rtn_val;
        break;
    case THREAD_TIME_TO_STOP:
        state = MOS_THREAD_STOP_REQUEST;
        break;
    default:
        if (thd->stop_request) state = MOS_THREAD_STOP_REQUEST;
        else state = MOS_THREAD_RUNNING;
        break;
    }
    UnlockScheduler();
    return state;
}

MosThreadPriority MosGetThreadPriority(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    return thd->pri;
}

void MosChangeThreadPriority(MosThread * _thd, MosThreadPriority new_pri) {
    Thread * thd = (Thread *)_thd;
    LockScheduler(IntPriMaskLow);
    // Snapshot the running thread priority (in case it gets changed)
    MosThreadPriority curr_pri = 0;
    if (RunningThread != NO_SUCH_THREAD) curr_pri = RunningThread->pri;
    // Change current priority if priority inheritance isn't active
    //  -OR- if new priority is higher than priority inheritance priority
    if (thd->pri == thd->nom_pri || new_pri < thd->pri) {
        thd->pri = new_pri;
        if (thd->state == THREAD_RUNNABLE) {
            MosRemoveFromList(&thd->run_link);
            MosAddToList(&RunQueues[new_pri], &thd->run_link);
        }
    }
    // Always change nominal priority
    thd->nom_pri = new_pri;
    // Yield if priority is lowered on currently running thread
    //  -OR- if other thread has a greater priority than running thread
    if (thd == RunningThread) {
        if (thd->pri > curr_pri) YieldThread();
    } else if (thd->pri < curr_pri) YieldThread();
    UnlockScheduler();
}

void MosRequestThreadStop(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    thd->stop_request = true;
}

bool MosIsStopRequested(void) {
    return (bool)RunningThread->stop_request;
}

s32 MosWaitForThreadStop(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    LockScheduler(IntPriMaskLow);
    if (thd->state > THREAD_STOPPED) {
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToList(&thd->stop_q, &RunningThread->run_link);
        SetThreadState(RunningThread, THREAD_WAIT_FOR_STOP);
        YieldThread();
    }
    UnlockScheduler();
    return thd->rtn_val;
}

bool MosWaitForThreadStopOrTO(MosThread * _thd, s32 * rtn_val, u32 ticks) {
    Thread * thd = (Thread *)_thd;
    SetTimeout(ticks);
    RunningThread->timed_out = 0;
    LockScheduler(IntPriMaskLow);
    if (thd->state > THREAD_STOPPED) {
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToList(&thd->stop_q, &RunningThread->run_link);
        SetThreadState(RunningThread, THREAD_WAIT_FOR_STOP_OR_TICK);
        YieldThread();
    }
    UnlockScheduler();
    if (RunningThread->timed_out) return false;
    *rtn_val = thd->rtn_val;
    return true;
}

void MosKillThread(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    if (thd == RunningThread) {
        SetRunningThreadStateAndYield(THREAD_TIME_TO_STOP);
        // Not reachable
        MosAssert(0);
    } else {
        // Snapshot the arguments, run thread stop handler, allowing thread
        //   to stop at its original run priority.
        LockScheduler(IntPriMaskLow);
        MosThreadPriority pri = thd->pri;
        MosThreadEntry * stop_handler = thd->term_handler;
        s32 stop_arg = thd->term_arg;
        u8 * stack_bottom = thd->stack_bottom;
        u32 stack_size = thd->stack_size;
        UnlockScheduler();
        MosInitAndRunThread((MosThread *) thd, pri, stop_handler, stop_arg,
                            stack_bottom, stack_size);
    }
}

void MosSetTermHandler(MosThread * _thd, MosThreadEntry * entry, s32 arg) {
    Thread * thd = (Thread *)_thd;
    LockScheduler(IntPriMaskLow);
    if (entry) thd->term_handler = entry;
    else thd->term_handler = ThreadExit;
    thd->term_arg = arg;
    UnlockScheduler();
}

void MosSetTermArg(MosThread * _thd, s32 arg) {
    Thread * thd = (Thread *)_thd;
    thd->term_arg = arg;
}

//
// Initialization
//

// TODO: auto tick startup?
void MosInit(void) {
#if (MOS_ARCH == MOS_ARM_V7M)
    // Trap Divide By 0 and (optionally) "Unintentional" Unaligned Accesses
    if (MOS_ENABLE_UNALIGN_FAULTS) {
        SCB->CCR |= (SCB_CCR_DIV_0_TRP_Msk | SCB_CCR_UNALIGN_TRP_Msk);
    } else {
        SCB->CCR |=  (SCB_CCR_DIV_0_TRP_Msk);
        SCB->CCR &= ~(SCB_CCR_UNALIGN_TRP_Msk);
    }
    // Enable Bus, Memory and Usage Faults in general
    SCB->SHCSR |= (SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_MEMFAULTENA_Msk |
                   SCB_SHCSR_USGFAULTENA_Msk);
    if (MOS_FP_LAZY_CONTEXT_SWITCHING) {
        // Ensure lazy stacking is enabled (for floating point)
        FPU->FPCCR |= (FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk);
    } else {
        FPU->FPCCR &= ~(FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk);
    }
#endif
    // Save errno pointer for use during context switch
    ErrNo = __errno();
    // Set up timers with tick-reduction
    CyclesPerTick = SysTick->LOAD + 1;
    MaxTickInterval = ((1 << 24) - 1) / CyclesPerTick;
    CyclesPerMicroSec = CyclesPerTick / MOS_MICRO_SEC_PER_TICK;
    // Set lowest preemption priority for SysTick and PendSV (highest number).
    // MOS requires that SysTick and PendSV share the same priority.
    u32 pri_grp = NVIC_GetPriorityGrouping();
    u32 pri_bits = 7 - pri_grp;
    if (pri_bits > __NVIC_PRIO_BITS) pri_bits = __NVIC_PRIO_BITS;
    u32 pri_low = (1 << pri_bits) - 1;
    // If there are subpriorities give SysTick higher subpriority (lower number).
    u32 subpri_bits = pri_grp - 7 + __NVIC_PRIO_BITS;
    if (pri_grp + __NVIC_PRIO_BITS < 7) subpri_bits = 0;
    if (subpri_bits == 0) {
        u32 pri = NVIC_EncodePriority(pri_grp, pri_low, 0);
        IntPriMaskLow = (pri << (8 - __NVIC_PRIO_BITS));
        NVIC_SetPriority(SysTick_IRQn, pri);
        NVIC_SetPriority(PendSV_IRQn, pri);
    } else {
        u32 subpri = (1 << subpri_bits) - 2;
        u32 pri = NVIC_EncodePriority(pri_grp, pri_low, subpri);
        IntPriMaskLow = (pri << (8 - __NVIC_PRIO_BITS));
        NVIC_SetPriority(SysTick_IRQn, pri);
        NVIC_SetPriority(PendSV_IRQn, NVIC_EncodePriority(pri_grp, pri_low, subpri + 1));
    }
    // Initialize empty queues
    for (MosThreadPriority pri = 0; pri < MOS_MAX_THREAD_PRIORITIES; pri++)
        MosInitList(&RunQueues[pri]);
    MosInitList(&ISREventQueue);
    MosInitList(&TimerQueue);
    // Create idle thread
    MosInitAndRunThread((MosThread *) &IdleThread, MOS_MAX_THREAD_PRIORITIES,
                        IdleThreadEntry, 0, IdleStack, sizeof(IdleStack));
    // Fill out remaining parameters
    Params.int_pri_low = pri_low;
}

//
// Scheduler
//

void MosRunScheduler(void) {
    // Start PSP in a safe place for first PendSV and then enable interrupts
    asm volatile (
        "ldr r0, psp_start\n"
        "msr psp, r0\n"
        "mov r0, #0\n"
        "msr basepri, r0\n"
        "cpsie if\n"
        "b SkipRS\n"
        ".balign 4\n"
        // 112 (28 words) is enough to store a dummy FP stack frame
        "psp_start: .word IdleStack + 112\n"
        "SkipRS:"
            : : : "r0"
    );
    // Invoke PendSV handler to start scheduler (first context switch)
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    asm volatile ( "isb" );
    // Not reachable
    MosAssert(0);
}

void SysTick_Handler(void) {
    asm volatile ( "cpsid if" );
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) Tick.count += 1;
    asm volatile ( "cpsie if" );
    if (RunningThread == NO_SUCH_THREAD) return;
    // Process timer queue
    //  Timer queues can contain threads or message timers
    MosLink * elm_save;
    for (MosLink * elm = TimerQueue.next; elm != &TimerQueue; elm = elm_save) {
        elm_save = elm->next;
        if (((MosLinkHet *)elm)->type == ELM_THREAD) {
            Thread * thd = container_of(elm, Thread, tmr_link);
            s32 rem_ticks = (s32)thd->wake_tick - Tick.lower;
            if (rem_ticks <= 0) {
                MosRemoveFromList(elm);
                // Lock interrupts since thread could be on semaphore pend queue
                asm volatile ( "cpsid if" );
                MosRemoveFromList(&thd->run_link);
                asm volatile ( "cpsie if" );
                MosAddToList(&RunQueues[thd->pri], &thd->run_link);
                thd->timed_out = 1;
                SetThreadState(thd, THREAD_RUNNABLE);
            } else break;
        } else {
            MosTimer * tmr = container_of(elm, MosTimer, tmr_link);
            s32 rem_ticks = (s32)tmr->wake_tick - Tick.lower;
            if (rem_ticks <= 0) {
                if ((tmr->callback)(tmr)) MosRemoveFromList(elm);
            } else break;
        }
    }
    YieldThread();
    EVENT(TICK, Tick.lower);
}

// Locking notes:
//   Since semaphore data structures can be manipulated in high-priority
//   ISR contexts interrupt disable is required to ensure data integrity.
//   Interrupts must be disabled for anything that manipulates the IRQ
//   event queue or manipulates/inspects semaphore pend queues.  For
//   mutexes and timers changing BASEPRI provides sufficient locking.

static u32 MOS_USED Scheduler(u32 sp) {
    EVENT(SCHEDULER_ENTRY, 0);
    // Save SP and ErrNo context
    if (RunningThread != NO_SUCH_THREAD) {
        RunningThread->sp = sp;
        RunningThread->err_no = *ErrNo;
    } else RunningThread = &IdleThread;
    // Update Running Thread state
    //   Threads can't directly kill themselves, so do it here.
    //   If thread needs to go onto timer queue, do it here.
    if (RunningThread->state == THREAD_TIME_TO_STOP) {
        // Arrange death of running thread via kill handler
        if (MosIsOnList(&RunningThread->tmr_link.link))
            MosRemoveFromList(&RunningThread->tmr_link.link);
        InitThread(RunningThread, RunningThread->pri,
                   RunningThread->term_handler, RunningThread->term_arg,
                   RunningThread->stack_bottom, RunningThread->stack_size);
        SetThreadState(RunningThread, THREAD_RUNNABLE);
    } else if (RunningThread->state & THREAD_STATE_TICK) {
        // Update running thread timer state (insertion sort in timer queue)
        s32 rem_ticks = (s32)RunningThread->wake_tick - Tick.lower;
        MosLink * elm;
        for (elm = TimerQueue.next; elm != &TimerQueue; elm = elm->next) {
            s32 wake_tick;
            if (((MosLinkHet *)elm)->type == ELM_THREAD) {
                Thread * thd = container_of(elm, Thread, tmr_link);
                wake_tick = (s32)thd->wake_tick;
            } else {
                MosTimer * tmr = container_of(elm, MosTimer, tmr_link);
                wake_tick = (s32)tmr->wake_tick;
            }
            s32 tmr_rem_ticks = wake_tick - Tick.lower;
            if (rem_ticks <= tmr_rem_ticks) break;
        }
        MosAddToListBefore(elm, &RunningThread->tmr_link.link);
        // If thread is only waiting for a tick
        if (RunningThread->state == THREAD_WAIT_FOR_TICK)
            MosRemoveFromList(&RunningThread->run_link);
    }
    // Process ISR event queue
    //  Event queue allows ISRs to signal semaphores without directly
    //  manipulating run queues, making critical sections shorter
    while (1) {
        asm volatile ( "cpsid if" );
        if (!MosIsListEmpty(&ISREventQueue)) {
            MosLink * elm = ISREventQueue.next;
            MosRemoveFromList(elm);
            // Currently only semaphores are on event list
            MosSem * sem = container_of(elm, MosSem, evt_link);
            // Release thread if it is pending
            if (!MosIsListEmpty(&sem->pend_q)) {
                MosLink * elm = sem->pend_q.next;
                MosRemoveFromList(elm);
                asm volatile ( "cpsie if" );
                Thread * thd = container_of(elm, Thread, run_link);
                MosAddToFrontOfList(&RunQueues[thd->pri], elm);
                if (MosIsOnList(&thd->tmr_link.link))
                    MosRemoveFromList(&thd->tmr_link.link);
                SetThreadState(thd, THREAD_RUNNABLE);
            } else asm volatile ( "cpsie if" );
        } else {
            asm volatile ( "cpsie if" );
            break;
        }
    }
    // Process Priority Queues
    // Start scan at first thread of highest priority, looking for first
    //  thread of list, and if no threads are runnable schedule idle thread.
    Thread * run_thd = NULL;
    for (MosThreadPriority pri = 0; pri < MOS_MAX_THREAD_PRIORITIES; pri++) {
        if (!MosIsListEmpty(&RunQueues[pri])) {
            run_thd = container_of(RunQueues[pri].next, Thread, run_link);
            break;
        }
    }
    if (run_thd) {
        // Round-robin
        if (!MosIsLastElement(&RunQueues[run_thd->pri], &run_thd->run_link))
            MosMoveToEndOfList(&RunQueues[run_thd->pri], &run_thd->run_link);
    } else run_thd = &IdleThread;
    if (ENABLE_SPLIM_SUPPORT) {
        asm volatile ( "MSR psplim, %0" : : "r" (run_thd->stack_bottom) );
    }
    // Set next thread ID and errno and return its stack pointer
    RunningThread = run_thd;
    *ErrNo = RunningThread->err_no;
    EVENT(SCHEDULER_EXIT, 0);
    return (u32)RunningThread->sp;
}

//
// Mutex
//

void MosInitMutex(MosMutex * mtx) {
    mtx->owner = NO_SUCH_THREAD;
    mtx->depth = 0;
    MosInitList(&mtx->pend_q);
}

void MosRestoreMutex(MosMutex * mtx) {
    if (mtx->owner == (void *)RunningThread) {
        mtx->depth = 1;
        MosUnlockMutex(mtx);
    }
}

bool MosIsMutexOwner(MosMutex * mtx) {
    return (mtx->owner == (void *)RunningThread);
}

//
// Semaphore
//

void MosInitSem(MosSem * sem, u32 start_value) {
    sem->value = start_value;
    MosInitList(&sem->pend_q);
    MosInitList(&sem->evt_link);
}

//
// Architecture specific
//

#if (MOS_ARCH == MOS_ARM_V6M)
  #include "kernel_v6m.inc"
#elif (MOS_ARCH == MOS_ARM_V7M)
  #include "kernel_v7m.inc"
#else
  #error "Unknown architecture"
#endif
