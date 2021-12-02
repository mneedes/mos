
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel
//

#include <bsp_hal.h>
#include <mos/kernel.h>
#include <mos/internal/arch.h>
#include <mos/internal/security.h>
#include <errno.h>

// TODO: Hooks for other timers such as LPTIM ?
// TODO: auto tick startup?
// TODO: smaller init for term handlers

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
    u32                 mtx_cnt;
    error_t             err_no;
    ThreadState         state;
    MosLink             run_link;
    MosLinkHet          tmr_link;
    MosList             stop_q;
    u32                 wake_tick;
    void              * blocked_on;
    MosThreadPriority   pri;
    MosThreadPriority   nom_pri;
    u8                  timed_out;
    u8                  pad;
    s32                 rtn_val;
    MosThreadEntry    * term_handler;
    s32                 term_arg;
    u8                * stack_bottom;
    u32                 stack_size;
    const char        * name;
    s8                  secure_context;
    s8                  secure_context_new;
    u16                 user_data16;
    void              * user_ptr;
    u32                 ref_cnt;
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
static MosRawVPrintfHook * VPrintfHook = NULL;
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
static u32 ExcReturnInitial = MOS_EXC_RETURN_DEFAULT;
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
MOS_STATIC_ASSERT(num_sec_contexts, MOS_NUM_SECURE_CONTEXTS <= 32);
static u32 SecureContextReservation = (1 << MOS_NUM_SECURE_CONTEXTS) - 1;
static MosSem SecureContextCounter;
#endif

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

// Print buffer
static char (*RawPrintBuffer)[MOS_PRINT_BUFFER_SIZE] = NULL;

const MosParams * MosGetParams(void) {
    return (const MosParams *) &Params;
}

void MosRegisterRawVPrintfHook(MosRawVPrintfHook * hook, char (*buffer)[MOS_PRINT_BUFFER_SIZE]) {
    VPrintfHook = hook;
    RawPrintBuffer = buffer;
}
void MosRegisterSleepHook(MosSleepHook * hook) { SleepHook = hook; }
void MosRegisterWakeHook(MosWakeHook * hook) { WakeHook = hook; }
void MosRegisterEventHook(MosEventHook * hook) { EventHook = hook; }

static MOS_INLINE void SetThreadState(Thread * thd, ThreadState state) {
    asm volatile ( "dmb" );
    thd->state = state;
}

MOS_ISR_SAFE static MOS_INLINE void YieldThread(void) {
    // Invoke PendSV handler to potentially perform context switch
    MOS_REG(ICSR) = MOS_REG_VALUE(ICSR_PENDSV);
    asm volatile ( "dsb" );
}

static MOS_INLINE void SetRunningThreadStateAndYield(ThreadState state) {
    asm volatile ( "dmb" );
    LockScheduler(IntPriMaskLow);
    RunningThread->state = state;
    YieldThread();
    UnlockScheduler();
}

static void KPrintf(const char * fmt, ...) {
    if (VPrintfHook) {
        va_list args;
        va_start(args, fmt);
        (*VPrintfHook)(fmt, args);
        va_end(args);
    }
}

#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
static void KPrint(void) {
    KPrintf(*RawPrintBuffer);
}
#endif

void MosAssertAt(char * file, u32 line) {
    KPrintf("Assertion failed in %s on line %u\n", file, line);
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

MOS_ISR_SAFE u64 MosGetCycleCount(void) {
    u32 mask = MosDisableInterrupts();
    if (MOS_REG(TICK_CTRL) & MOS_REG_VALUE(TICK_FLAG)) Tick.count++;
    s64 tmp = Tick.count;
    u32 val = MOS_REG(TICK_VAL);
    MosEnableInterrupts(mask);
    return (tmp * CyclesPerTick) - val;
}

MOS_ISR_SAFE void MosAdvanceTickCount(u32 ticks) {
    if (ticks) {
        u32 mask = MosDisableInterrupts();
        Tick.count += ticks;
        MOS_REG(ICSR) = MOS_REG_VALUE(ICSR_PENDST);
        MosEnableInterrupts(mask);
    }
}

MOS_ISR_SAFE static void MOS_USED SetTimeout(u32 ticks) {
    RunningThread->wake_tick = Tick.lower + ticks;
}

MOS_ISR_SAFE void MOS_NAKED MosDelayMicroSec(u32 usec) {
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

void MosSetTimer(MosTimer * tmr, u32 ticks, void * user_ptr) {
    LockScheduler(IntPriMaskLow);
    tmr->ticks = ticks;
    tmr->user_ptr = user_ptr;
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

MOS_ISR_SAFE static void
InitThread(Thread * thd, MosThreadPriority pri, MosThreadEntry * entry, s32 arg,
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
    sf->LR_EXC_RTN = ExcReturnInitial;
    // Either fill lower stack OR just place canary value at bottom
    if (MOS_STACK_USAGE_MONITOR) {
        u32 * fill = (u32 *)sf - 1;
        for (; fill >= (u32 *)stack_bottom; fill--) {
            *fill = STACK_FILL_VALUE;
        }
    } else *((u32 *)stack_bottom) = STACK_FILL_VALUE;
    // Initialize context and state
    thd->sp = (u32)sf;
    thd->mtx_cnt = 0;
    thd->err_no = 0;
    thd->pri = pri;
    thd->nom_pri = pri;
    thd->term_handler = ThreadExit;
    thd->term_arg = 0;
    thd->stack_bottom = stack_bottom;
    thd->stack_size = stack_size;
    thd->name = "";
    thd->secure_context     = MOS_DEFAULT_SECURE_CONTEXT;
    thd->secure_context_new = MOS_DEFAULT_SECURE_CONTEXT;
    thd->user_data16 = 0;
    thd->user_ptr = NULL;
    // ref_cnt is not initialized here, it is manipulated externally
}

static s32 IdleThreadEntry(s32 arg) {
    MOS_UNUSED(arg);
    while (1) {
        // Disable interrupts and timer
        asm volatile ( "cpsid i" ::: "memory" );
        MOS_REG(TICK_CTRL) = MOS_REG_VALUE(TICK_DISABLE);
        if (MOS_REG(TICK_CTRL) & MOS_REG_VALUE(TICK_FLAG)) Tick.count += 1;
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
            load = (tick_interval - 1) * CyclesPerTick + MOS_REG(TICK_VAL) - 1;
            MOS_REG(TICK_LOAD) = load;
            MOS_REG(TICK_VAL) = 0;
        }
        if (SleepHook) (*SleepHook)();
        MOS_REG(TICK_CTRL) = MOS_REG_VALUE(TICK_ENABLE);
        asm volatile (
            "dsb\n"
            "wfi" ::: "memory"
        );
        if (WakeHook) (*WakeHook)();
        if (load) {
            MOS_REG(TICK_CTRL) = MOS_REG_VALUE(TICK_DISABLE);
            if (MOS_REG(TICK_CTRL) & MOS_REG_VALUE(TICK_FLAG)) {
                // If counter rolled over then account for all ticks
                MOS_REG(TICK_LOAD) = CyclesPerTick - 1;
                MOS_REG(TICK_VAL) = 0;
                Tick.count += tick_interval;
            } else {
                // Interrupt was early so account for elapsed ticks
                u32 adj_tick_interval = (load - MOS_REG(TICK_VAL)) / CyclesPerTick;
                MOS_REG(TICK_LOAD) = MOS_REG(TICK_VAL);
                MOS_REG(TICK_VAL) = 0;
                MOS_REG(TICK_LOAD) = CyclesPerTick - 1;
                Tick.count += adj_tick_interval;
            }
            MOS_REG(TICK_CTRL) = MOS_REG_VALUE(TICK_ENABLE);
        }
        asm volatile ( "dsb\n"
                       "cpsie i\n"
                       "isb" ::: "memory" );
    }
    return 0;
}

MOS_ISR_SAFE void MosYieldThread(void) {
    if (RunningThread == NO_SUCH_THREAD) return;
    // Invoke PendSV handler to potentially perform context switch
    MOS_REG(ICSR) = MOS_REG_VALUE(ICSR_PENDSV);
    asm volatile ( "dsb" );
}

MosThread * MosGetThreadPtr(void) {
    return (MosThread *)RunningThread;
}

void MosGetStackStats(MosThread * _thd, u32 * stack_size, u32 * stack_usage, u32 * max_stack_usage) {
    Thread * thd = (Thread *)_thd;
    LockScheduler(IntPriMaskLow);
    // Detect uninitialized thread state
    u32 state = thd->state;
    if (state == THREAD_UNINIT || (state & THREAD_STATE_BASE_MASK) != THREAD_STATE_BASE) return;
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
        _MosDisableInterrupts();
        MosRemoveFromList(&thd->run_link);
        _MosEnableInterrupts();
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
        state = MOS_THREAD_RUNNING;
        break;
    }
    UnlockScheduler();
    return state;
}

MosThreadPriority MosGetThreadPriority(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    return thd->pri;
}

// Sort thread into pend queue by priority
MOS_ISR_SAFE static void SortThreadByPriority(Thread * thd, MosList * pend_q) {
    MosRemoveFromList(&thd->run_link);
    MosLink * elm = pend_q->next;
    for (; elm != pend_q; elm = elm->next) {
        Thread * _thd = container_of(elm, Thread, run_link);
        if (_thd->pri > thd->pri) break;
    }
    MosAddToListBefore(elm, &thd->run_link);
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
        switch (thd->state) {
        case THREAD_RUNNABLE:
            MosRemoveFromList(&thd->run_link);
            MosAddToList(&RunQueues[new_pri], &thd->run_link);
            break;
        case THREAD_WAIT_FOR_MUTEX:
            SortThreadByPriority(thd, &((MosMutex *)thd->blocked_on)->pend_q);
            break;
        case THREAD_WAIT_FOR_SEM:
        case THREAD_WAIT_FOR_SEM_OR_TICK:
            SortThreadByPriority(thd, &((MosSem *)thd->blocked_on)->pend_q);
            break;
        default:
            break;
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

void MosInit(void) {
    // Save errno pointer for use during context switch
    ErrNo = __errno();
    // Set up timers with tick-reduction
    CyclesPerTick = MOS_REG(TICK_LOAD) + 1;
    MaxTickInterval = ((1 << 24) - 1) / CyclesPerTick;
    CyclesPerMicroSec = CyclesPerTick / MOS_MICRO_SEC_PER_TICK;
    // Architecture-specific setup
#if (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_MAIN)
    // Trap Divide By 0 and disable "Unintentional" Alignment Faults
    MOS_REG(CCR) |=  MOS_REG_VALUE(DIV0_TRAP);
    MOS_REG(CCR) &= ~MOS_REG_VALUE(UNALIGN_TRAP);
    // Enable Bus, Memory and Usage Faults in general
    MOS_REG(SHCSR) |= MOS_REG_VALUE(FAULT_ENABLE);
    if (MOS_FP_LAZY_CONTEXT_SWITCHING) {
        // Ensure lazy stacking is enabled (for floating point)
        MOS_REG(FPCCR) |=  MOS_REG_VALUE(LAZY_STACKING);
    } else {
        MOS_REG(FPCCR) &= ~MOS_REG_VALUE(LAZY_STACKING);
    }
    // Set lowest preemption priority for SysTick and PendSV.
    //   MOS requires that SysTick and PendSV share the same priority,
    MOS_REG(SHPR)(MOS_PENDSV_IRQ - 4) = 0xff;
    // Read back register to determine mask (and number of implemented priority bits)
    u8 pri_mask = MOS_REG(SHPR)(MOS_PENDSV_IRQ - 4);
    u8 nvic_pri_bits = 8 - __builtin_ctz(pri_mask);
    // If priority groups are enabled SysTick will be set to the
    //   2nd lowest priority group, and PendSV the lowest.
    u32 pri_bits = 7 - MOS_GET_PRI_GROUP_NUM;
    if (pri_bits > nvic_pri_bits) pri_bits = nvic_pri_bits;
    u32 pri_low = (1 << pri_bits) - 1;
    u8 pri_systick = pri_mask;
    // If there are sub-priority bits give SysTick higher sub-priority (one lower number).
    if (pri_bits < nvic_pri_bits) {
        // Clear lowest set bit in mask
        pri_systick = pri_mask - (1 << (8 - nvic_pri_bits));
    }
    MOS_REG(SHPR)(MOS_SYSTICK_IRQ  - 4) = pri_systick;
    IntPriMaskLow = pri_systick;
#elif (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_BASE)
    // NOTE: BASEPRI isn't implemented on baseline architectures, hence IntPriMaskLow is not used
    // Set lowest preemption priority for SysTick and PendSV
    MOS_REG(SHPR3) = MOS_REG_VALUE(EXC_PRIORITY);
    // Only two implemented priority bits on baseline
    u8 pri_low = 3;
#endif
#if (MOS_ARM_AUTODETECT_EXC_RETURN == true)
    // Detect security mode and set Exception Return accordingly
    if (MOS_REG(CPUID_NS) == 0) ExcReturnInitial = MOS_EXC_RETURN_UNSECURE;
#endif
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
    MosInitSem(&SecureContextCounter, MOS_NUM_SECURE_CONTEXTS);
#endif
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
    // Start PSP in a safe place for first PendSV
    asm volatile (
        "ldr r0, psp_start\n"
        "msr psp, r0\n"
        "mov r0, #0\n"
        "msr basepri, r0\n"
        "b SkipRS\n"
        ".balign 4\n"
        // 112 (28 words) is enough to store a dummy FP stack frame
        "psp_start: .word IdleStack + 112\n"
      "SkipRS:"
            : : : "r0"
    );
    // Invoke PendSV handler to start scheduler (first context switch)
    YieldThread();
    _MosEnableInterruptsWithBarrier();
    // Not reachable
    MosAssert(0);
}

void SysTick_Handler(void) {
    _MosDisableInterrupts();
    if (MOS_REG(TICK_CTRL) & MOS_REG_VALUE(TICK_FLAG)) Tick.count += 1;
    _MosEnableInterrupts();
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
                if (thd->state == THREAD_WAIT_FOR_SEM_OR_TICK) {
                    _MosDisableInterrupts();
                    if (MosIsOnList(&((MosSem *)thd->blocked_on)->evt_link)) {
                        // Event occurred before timeout, just let it be processed
                        _MosEnableInterrupts();
                        continue;
                    } else {
                        MosRemoveFromList(&thd->run_link);
                        _MosEnableInterrupts();
                    }
                } else MosRemoveFromList(&thd->run_link);
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
    } else {
        RunningThread = &IdleThread;
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
        _NSC_MosInitSecureContexts(KPrint, RawPrintBuffer);
#endif
    }
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
        _MosDisableInterrupts();
        if (!MosIsListEmpty(&ISREventQueue)) {
            MosLink * elm = ISREventQueue.next;
            MosRemoveFromList(elm);
            // Currently only semaphores are on event list
            MosSem * sem = container_of(elm, MosSem, evt_link);
            // Release thread if it is pending
            if (!MosIsListEmpty(&sem->pend_q)) {
                MosLink * elm = sem->pend_q.next;
                MosRemoveFromList(elm);
                _MosEnableInterrupts();
                Thread * thd = container_of(elm, Thread, run_link);
                MosAddToFrontOfList(&RunQueues[thd->pri], elm);
                if (MosIsOnList(&thd->tmr_link.link))
                    MosRemoveFromList(&thd->tmr_link.link);
                SetThreadState(thd, THREAD_RUNNABLE);
            } else _MosEnableInterrupts();
        } else {
            _MosEnableInterrupts();
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
    if (MOS_ENABLE_SPLIM_SUPPORT) {
        asm volatile ( "msr psplim, %0" : : "r" (run_thd->stack_bottom) );
    }
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
    // If there is a new secure context, only load the next context, don't save it.
    // otherwise only save/load the context if it is different.
    if (RunningThread->secure_context_new != RunningThread->secure_context) {
        _NSC_MosSwitchSecureContext(-1, run_thd->secure_context);
        RunningThread->secure_context = RunningThread->secure_context_new;
    } else if (RunningThread->secure_context != run_thd->secure_context)
        _NSC_MosSwitchSecureContext(RunningThread->secure_context, run_thd->secure_context);
#endif
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
// Security
//

#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)

// Reserve secure contexts for threads.
//   Scheduler must be locked out during reservation.
//   Scheduler is invoked to change the context.
void MosReserveSecureContext(void) {
    MosWaitForSem(&SecureContextCounter);
    LockScheduler(IntPriMaskLow);
    u32 new_context = __builtin_ctz(SecureContextReservation);
    RunningThread->secure_context_new = new_context;
    SecureContextReservation &= ~(1 << new_context);
    // Yield so that this thread can immediately use new stack pointer
    YieldThread();
    UnlockScheduler();
}

bool MosTryReserveSecureContext(void) {
    if (MosTrySem(&SecureContextCounter)) {
        LockScheduler(IntPriMaskLow);
        u32 new_context = __builtin_ctz(SecureContextReservation);
        RunningThread->secure_context_new = new_context;
        SecureContextReservation &= ~(1 << new_context);
        YieldThread();
        UnlockScheduler();
        return true;
    }
    return false;
}

// Revert all threads to default secure context
void MosReleaseSecureContext(void) {
    LockScheduler(IntPriMaskLow);
    u32 old_context = RunningThread->secure_context;
    // Reset pointer value for next thread (using current thread context)
    _NSC_MosResetSecureContext(old_context);
    RunningThread->secure_context_new = MOS_DEFAULT_SECURE_CONTEXT;
    SecureContextReservation |= (1 << old_context);
    // Yield so that stack pointer is made available for next thread.
    YieldThread();
    UnlockScheduler();
    MosIncrementSem(&SecureContextCounter);
}

#endif

//
// Architecture specific
//

#if (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_BASE)
  #include "internal/kernel_base.inc"
#elif (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_MAIN)
  #include "internal/kernel_main.inc"
#else
  #error "Unknown architecture category"
#endif
