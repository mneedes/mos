
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

// Mask interrupts by priority, primarily for temporarily
//   disabling context switches.
static MOS_INLINE void SetBasePri(u32 pri) {
    asm volatile (
        "msr basepri, %0\n"
        "isb"
            : : "r" (pri) : "memory"
    );
}

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
    SetBasePri(IntPriMaskLow);
    RunningThread->state = state;
    YieldThread();
    SetBasePri(0);
}

// ThreadExit is invoked when a thread stops (returns from its natural entry point)
//   or after its termination handler returns (kill or exception)
static s32 ThreadExit(s32 rtn_val) {
    SetBasePri(IntPriMaskLow);
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
    SetBasePri(0);
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

#if (MOS_FP_LAZY_CONTEXT_SWITCHING == true)

void MOS_NAKED PendSV_Handler(void) {
    // Floating point context switch (lazy stacking)
    asm volatile (
        "mrs r0, psp\n"
        "tst lr, #16\n"
        "it eq\n"
        "vstmdbeq r0!, {s16-s31}\n"
        "stmdb r0!, {r4-r11, lr}\n"
        "bl Scheduler\n"
        "ldmfd r0!, {r4-r11, lr}\n"
        "tst lr, #16\n"
        "it eq\n"
        "vldmiaeq r0!, {s16-s31}\n"
        "msr psp, r0\n"
        "bx lr"
    );
}

#else

void MOS_NAKED PendSV_Handler(void) {
    // Vanilla context switch without floating point.
    asm volatile (
        "mrs r0, psp\n"
        "stmdb r0!, {r4-r11, lr}\n"
        "bl Scheduler\n"
        "ldmfd r0!, {r4-r11, lr}\n"
        "msr psp, r0\n"
        "bx lr"
    );
}

#endif

// TODO: Limit MSP stack dump to end of MSP stack
// TODO: Dump more registers in general including FP registers ?
static void MOS_USED FaultHandler(u32 * msp, u32 * psp, u32 psr, u32 lr) {
    if (MOS_BKPT_IN_EXCEPTIONS) MosHaltIfDebugging();
    char * fault_type[] = {
        "Hard", "Mem", "Bus", "Usage", "Imprecise Bus"
    };
    bool in_isr = ((lr & 0x8) == 0x0);
    bool fp_en = ((lr & 0x10) == 0x0);
    u32 cfsr = SCB->CFSR;
    if (PrintfHook) {
        u32 fault_no = (psr & 0xf) - 3;
        if (fault_no == 2 && (cfsr & 0x400)) fault_no = 4;
        (*PrintfHook)("\n*** %s Fault %s", fault_type[fault_no],
                          in_isr ? "IN ISR " : "");
        if (RunningThread == NO_SUCH_THREAD) (*PrintfHook)("(Pre-Scheduler) ***\n");
        else if (RunningThread->name && RunningThread->name[0] != '\0')
            (*PrintfHook)("(Thread %s) ***\n", RunningThread->name);
        else
            (*PrintfHook)("(Thread @%08X) ***\n", RunningThread);

        if (fp_en) (*PrintfHook)("*** Lazy Floating Point Enabled ***\n");

        (*PrintfHook)(" HFSR: %08X  CFSR: %08X AFSR: %08X\n",
                          SCB->HFSR, cfsr, SCB->AFSR);
        (*PrintfHook)(" BFAR: %08X MMFAR: %08X\n\n", SCB->BFAR, SCB->MMFAR);

        bool use_psp = ((lr & 0x4) == 0x4);
        u32 * sp = use_psp ? psp : msp;
        s32 num_words = 16;
        if (use_psp && RunningThread != NO_SUCH_THREAD) {
            u8 * sp2 = RunningThread->stack_bottom;
            if (*((u32 *)sp2) != STACK_FILL_VALUE)
                (*PrintfHook)("!!! Thread Stack corruption (bottom) !!!\n");
            sp2 = (u8 *) ((u32)(sp2 + RunningThread->stack_size - sizeof(u32)) & 0xfffffff8);
            if (*((u32 *)sp2) != STACK_FILL_VALUE)
                (*PrintfHook)("!!! Thread Stack corruption (top) !!!\n");
            s32 rem_words = ((u32 *) sp2) - sp;
            if (rem_words < 64) num_words = rem_words;
            else num_words = 64;
        }
        (*PrintfHook)("%s Stack @%08X:\n", use_psp ? "Process" : "Main", (u32) sp);
        (*PrintfHook)(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  sp[0], sp[1], sp[2], sp[3]);
        (*PrintfHook)(" %08X %08X %08X %08X (R12 LR PC PSR)\n", sp[4], sp[5], sp[6], sp[7]);
        sp += 8;
        for (s32 ix = 0; ix < (num_words - 8); ix++) {
            (*PrintfHook)(" %08X", sp[ix]);
            if ((ix & 0x3) == 0x3) (*PrintfHook)("\n");
        }
        (*PrintfHook)("\n\n");
    }
    if (MOS_HANG_ON_EXCEPTIONS) {
        while (1);
    } else {
        if (RunningThread == NO_SUCH_THREAD || in_isr) {
            // Hang if fault occurred anywhere but in thread context
            while (1);
        } else {
            // Clear CFSR bits
            SCB->CFSR = cfsr;
            // Stop thread if fault occurred in thread context
            SetThreadState(RunningThread, THREAD_TIME_TO_STOP);
            YieldThread();
        }
    }
}

void MOS_NAKED MOS_WEAK HardFault_Handler(void) {
    asm volatile (
        "mrs r0, msp\n"
        "mrs r1, psp\n"
        "mrs r2, psr\n"
        "mov r3, lr\n"
        "b FaultHandler"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MOS_NAKED MOS_WEAK MemManage_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

void MOS_NAKED MOS_WEAK BusFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

void MOS_NAKED MOS_WEAK UsageFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

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

void MosDelayThread(u32 ticks) {
    if (ticks) {
        SetTimeout(ticks);
        SetRunningThreadStateAndYield(THREAD_WAIT_FOR_TICK);
    } else YieldThread();
}

void MOS_NAKED MosDelayMicroSec(u32 usec) {
    MOS_UNUSED(usec);
    asm volatile (
        "ldr r1, _CyclesPerMicroSec\n"
        "ldr r1, [r1]\n"
        "mul r0, r0, r1\n"
        "sub r0, #13\n"  // Overhead calibration
        "delay:\n"
        // It is possible that 6 is another valid value, non-cached flash stall?
#if (MOS_CYCLES_PER_INNER_LOOP == 3)
        "subs r0, #3\n"
#elif (MOS_CYCLES_PER_INNER_LOOP == 1)
        "subs r0, #1\n"
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

// Timers

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
    SetBasePri(IntPriMaskLow);
    tmr->ticks = ticks;
    tmr->priv_data = priv_data;
    AddTimer(tmr);
    SetBasePri(0);
}

void MosCancelTimer(MosTimer * tmr) {
    SetBasePri(IntPriMaskLow);
    if (MosIsOnList(&tmr->tmr_link.link))
        MosRemoveFromList(&tmr->tmr_link.link);
    SetBasePri(0);
}

void MosResetTimer(MosTimer * tmr) {
    SetBasePri(IntPriMaskLow);
    if (MosIsOnList(&tmr->tmr_link.link))
        MosRemoveFromList(&tmr->tmr_link.link);
    AddTimer(tmr);
    SetBasePri(0);
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

// TODO: auto tick startup?
void MosInit(void) {
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

void MosRegisterRawPrintfHook(MosRawPrintfHook * hook) { PrintfHook = hook; }
void MosRegisterSleepHook(MosSleepHook * hook) { SleepHook = hook; }
void MosRegisterWakeHook(MosWakeHook * hook) { WakeHook = hook; }
void MosRegisterEventHook(MosEventHook * hook) { EventHook = hook; }

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

const MosParams * MosGetParams(void) {
    return (const MosParams *) &Params;
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
    SetBasePri(IntPriMaskLow);
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
    SetBasePri(0);
}

u8 * MosGetStackBottom(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    u8 * stack_bottom = NULL;
    SetBasePri(IntPriMaskLow);
    if (thd) stack_bottom = thd->stack_bottom;
    else if (RunningThread) stack_bottom = RunningThread->stack_bottom;
    SetBasePri(0);
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
    SetBasePri(IntPriMaskLow);
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
    SetBasePri(0);
    InitThread(thd, pri, entry, arg, stack_bottom, stack_size);
    SetThreadState(thd, THREAD_INIT);
    return true;
}

bool MosRunThread(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    if (thd->state == THREAD_INIT) {
        SetBasePri(IntPriMaskLow);
        SetThreadState(thd, THREAD_RUNNABLE);
        if (thd != &IdleThread)
            MosAddToList(&RunQueues[thd->pri], &thd->run_link);
        if (RunningThread != NO_SUCH_THREAD && thd->pri < RunningThread->pri)
            YieldThread();
        SetBasePri(0);
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
    SetBasePri(IntPriMaskLow);
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
    SetBasePri(0);
    return state;
}

MosThreadPriority MosGetThreadPriority(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    return thd->pri;
}

void MosChangeThreadPriority(MosThread * _thd, MosThreadPriority new_pri) {
    Thread * thd = (Thread *)_thd;
    SetBasePri(IntPriMaskLow);
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
    SetBasePri(0);
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
    SetBasePri(IntPriMaskLow);
    if (thd->state > THREAD_STOPPED) {
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToList(&thd->stop_q, &RunningThread->run_link);
        SetThreadState(RunningThread, THREAD_WAIT_FOR_STOP);
        YieldThread();
    }
    SetBasePri(0);
    return thd->rtn_val;
}

bool MosWaitForThreadStopOrTO(MosThread * _thd, s32 * rtn_val, u32 ticks) {
    Thread * thd = (Thread *)_thd;
    SetTimeout(ticks);
    RunningThread->timed_out = 0;
    SetBasePri(IntPriMaskLow);
    if (thd->state > THREAD_STOPPED) {
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToList(&thd->stop_q, &RunningThread->run_link);
        SetThreadState(RunningThread, THREAD_WAIT_FOR_STOP_OR_TICK);
        YieldThread();
    }
    SetBasePri(0);
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
        SetBasePri(IntPriMaskLow);
        MosThreadPriority pri = thd->pri;
        MosThreadEntry * stop_handler = thd->term_handler;
        s32 stop_arg = thd->term_arg;
        u8 * stack_bottom = thd->stack_bottom;
        u32 stack_size = thd->stack_size;
        SetBasePri(0);
        MosInitAndRunThread((MosThread *) thd, pri, stop_handler, stop_arg,
                            stack_bottom, stack_size);
    }
}

void MosSetTermHandler(MosThread * _thd, MosThreadEntry * entry, s32 arg) {
    Thread * thd = (Thread *)_thd;
    SetBasePri(IntPriMaskLow);
    if (entry) thd->term_handler = entry;
    else thd->term_handler = ThreadExit;
    thd->term_arg = arg;
    SetBasePri(0);
}

void MosSetTermArg(MosThread * _thd, s32 arg) {
    Thread * thd = (Thread *)_thd;
    thd->term_arg = arg;
}

void MosInitMutex(MosMutex * mtx) {
    mtx->owner = NO_SUCH_THREAD;
    mtx->depth = 0;
    MosInitList(&mtx->pend_q);
}

static void MOS_USED BlockOnMutex(MosMutex * mtx) {
    SetBasePri(IntPriMaskLow);
    asm volatile ( "dsb" );
    // Retry (don't block) if mutex has since been given
    if (mtx->owner != NO_SUCH_THREAD) {
        // Add thread to pend queue
        MosLink * elm = mtx->pend_q.next;
        for (; elm != &mtx->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        // Basic priority inheritance
        Thread * thd = (Thread *) mtx->owner;
        if (RunningThread->pri < thd->pri) {
            thd->pri = RunningThread->pri;
            if (thd->state == THREAD_RUNNABLE) {
                MosRemoveFromList(&thd->run_link);
                MosAddToFrontOfList(&RunQueues[thd->pri], &thd->run_link);
            }
        }
        SetThreadState(RunningThread, THREAD_WAIT_FOR_MUTEX);
        YieldThread();
    }
    SetBasePri(0);
}

void MOS_NAKED MosLockMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
    asm volatile (
        "ldr r1, _ThreadID\n"
        "ldr r1, [r1]\n"
        "RetryTM:\n"
        "ldrex r2, [r0]\n"
        "cmp r2, r1\n"
        "beq IncTM\n"
        "cbnz r2, BlockTM\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTM\n"
        "IncTM:\n"
        "ldr r2, [r0, #4]\n"
        "add r2, #1\n"
        "str r2, [r0, #4]\n"
        "dmb\n"
        "bx lr\n"
        "BlockTM:\n"
        "push {r0, r1, lr}\n"
        "bl BlockOnMutex\n"
        "pop {r0, r1, lr}\n"
        "b RetryTM\n"
        ".balign 4\n"
        "_ThreadID: .word RunningThread"
            // Explicit clobber list prevents compiler from making
            // assumptions about registers not being changed as
            // this assembly code calls a C function.  Normally C
            // ABI treats r0-r3 as input or scratch registers.
            : : : "r0", "r1", "r2", "r3"
    );
}

bool MOS_NAKED MosTryMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
    asm volatile (
        "ldr r1, _ThreadID2\n"
        "ldr r1, [r1]\n"
        "RetryTRM:\n"
        "ldrex r2, [r0]\n"
        "cmp r2, r1\n"
        "beq IncTRM\n"
        "cbnz r2, FailTRM\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTRM\n"
        "IncTRM:\n"
        "ldr r2, [r0, #4]\n"
        "add r2, #1\n"
        "str r2, [r0, #4]\n"
        "dmb\n"
        "mov r0, #1\n"
        "bx lr\n"
        "FailTRM:\n"
        "mov r0, #0\n"
        "bx lr\n"
        ".balign 4\n"
        "_ThreadID2: .word RunningThread"
            : : : "r0", "r1", "r2", "r3"
    );
}

static void MOS_USED ReleaseMutex(MosMutex * mtx) {
    SetBasePri(IntPriMaskLow);
    asm volatile ( "dsb" );
    if (!MosIsListEmpty(&mtx->pend_q)) {
        MosLink * elm = mtx->pend_q.next;
        Thread * thd = container_of(elm, Thread, run_link);
        MosRemoveFromList(elm);
        MosAddToFrontOfList(&RunQueues[thd->pri], elm);
        if (MosIsOnList(&thd->tmr_link.link))
            MosRemoveFromList(&thd->tmr_link.link);
        SetThreadState(thd, THREAD_RUNNABLE);
        // Reset priority inheritance
        if (RunningThread->pri != RunningThread->nom_pri) {
            RunningThread->pri = RunningThread->nom_pri;
            MosRemoveFromList(&RunningThread->run_link);
            MosAddToFrontOfList(&RunQueues[RunningThread->pri],
                                    &RunningThread->run_link);
        }
        if (thd->pri < RunningThread->pri) YieldThread();
    }
    SetBasePri(0);
}

void MOS_NAKED MosUnlockMutex(MosMutex * mtx) {
    MOS_USED_PARAM(mtx);
    asm volatile (
        "dmb\n"
        "ldr r1, [r0, #4]\n"
        "sub r1, #1\n"
        "str r1, [r0, #4]\n"
        "cbnz r1, SkipGM\n"
        "mov r1, #0\n"
        "RetryGM:\n"
        "ldrex r2, [r0]\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryGM\n"
        "b ReleaseMutex\n"
        "SkipGM:\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
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

void MosInitSem(MosSem * sem, u32 start_value) {
    sem->value = start_value;
    MosInitList(&sem->pend_q);
    MosInitList(&sem->evt_link);
}

static void MOS_USED BlockOnSem(MosSem * sem) {
    // Lock in case semaphore is about to be given by another context.
    asm volatile ( "cpsid if" );
    // Retry (don't block) if count has since incremented
    if (sem->value == 0) {
        MosLink * elm = sem->pend_q.next;
        for (; elm != &sem->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        RunningThread->state = THREAD_WAIT_FOR_SEM;
        YieldThread();
    }
    asm volatile ( "cpsie if" );
}

void MOS_NAKED MosWaitForSem(MosSem * sem) {
    MOS_USED_PARAM(sem);
    asm volatile (
        "RetryTS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockTS\n"
        "sub r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTS\n"
        "dmb\n"
        "bx lr\n"
        "BlockTS:\n"
        "push {r0, lr}\n"
        "bl BlockOnSem\n"
        "pop {r0, lr}\n"
        "b RetryTS"
            : : : "r0", "r1", "r2", "r3"
    );
}

static bool MOS_USED BlockOnSemOrTO(MosSem * sem) {
    bool timeout = false;
    // Lock in case semaphore is about to be given by another context.
    asm volatile ( "cpsid if" );
    // Immediately retry (don't block) if count has since incremented
    if (sem->value == 0) {
        RunningThread->timed_out = 0;
        MosLink * elm = sem->pend_q.next;
        for (; elm != &sem->pend_q; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_link);
            if (thd->pri > RunningThread->pri) break;
        }
        // Can directly manipulate run queues here since scheduler
        // and other other interrupts are locked out.
        MosRemoveFromList(&RunningThread->run_link);
        MosAddToListBefore(elm, &RunningThread->run_link);
        RunningThread->state = THREAD_WAIT_FOR_SEM_OR_TICK;
        YieldThread();
        // Must enable interrupts before checking timeout to allow pend
        // Barrier ensures that pend occurs before checking timeout.
        asm volatile (
            "cpsie if\n"
            "isb"
        );
        if (RunningThread->timed_out) timeout = true;
    } else asm volatile ( "cpsie if" );
    return timeout;
}

bool MOS_NAKED MosWaitForSemOrTO(MosSem * sem, u32 ticks) {
    MOS_USED_PARAM(sem);
    MOS_USED_PARAM(ticks);
    asm volatile (
        "push {r0, lr}\n"
        "mov r0, r1\n"
        "bl SetTimeout\n"
        "pop {r0, lr}\n"
        "RetryTSTO:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockTSTO\n"
        "sub r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTSTO\n"
        "dmb\n"
        "mov r0, #1\n"
        "bx lr\n"
        "BlockTSTO:\n"
        "push {r0, lr}\n"
        "bl BlockOnSemOrTO\n"
        "cmp r0, #0\n"
        "pop {r0, lr}\n"
        "beq RetryTSTO\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

bool MOS_NAKED MOS_ISR_SAFE MosTrySem(MosSem * sem) {
    MOS_USED_PARAM(sem);
    asm volatile (
        "RetryTRS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, FailTRS\n"
        "sub r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryTRS\n"
        "dmb\n"
        "mov r0, #1\n"
        "bx lr\n"
        "FailTRS:\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

// For yielding when semaphore is given
static void MOS_USED MOS_ISR_SAFE ReleaseOnSem(MosSem * sem) {
    // This places the semaphore on event queue to be processed by
    // scheduler to avoid direct manipulation of run queues.  If run
    // queues were manipulated here critical sections would be larger.
    asm volatile ( "cpsid if" );
    // Only add event if pend_q is not empty and event not already queued
    if (!MosIsListEmpty(&sem->pend_q) && !MosIsOnList(&sem->evt_link)) {
        MosAddToList(&ISREventQueue, &sem->evt_link);
        Thread * thd = container_of(sem->pend_q.next, Thread, run_link);
        // Yield if released thread has higher priority than running thread
        if (RunningThread && thd->pri < RunningThread->pri) YieldThread();
    }
    asm volatile ( "cpsie if" );
}

void MOS_NAKED MOS_ISR_SAFE MosIncrementSem(MosSem * sem) {
    MOS_USED_PARAM(sem);
    asm volatile (
        "push { lr }\n"
        "RetryGS:\n"
        "ldrex r1, [r0]\n"
        "add r1, #1\n"
        "strex r2, r1, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryGS\n"
        "dmb\n"
        "bl ReleaseOnSem\n"
        "pop { pc }"
            : : : "r0", "r1", "r2", "r3"
    );
}

u32 MOS_NAKED MosWaitForSignal(MosSem * sem) {
    MOS_USED_PARAM(sem);
    asm volatile (
        "mov r3, #0\n"
        "RetryWS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockWS\n"
        "strex r2, r3, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryWS\n"
        "mov r0, r1\n"
        "dmb\n"
        "bx lr\n"
        "BlockWS:\n"
        "push {r0, r3, lr}\n"
        "bl BlockOnSem\n"
        "pop {r0, r3, lr}\n"
        "b RetryWS"
            : : : "r0", "r1", "r2", "r3"
    );
}

u32 MOS_NAKED MosWaitForSignalOrTO(MosSem * sem, u32 ticks) {
    MOS_USED_PARAM(sem);
    MOS_USED_PARAM(ticks);
    asm volatile (
        "push {r0, lr}\n"
        "mov r0, r1\n"
        "bl SetTimeout\n"
        "pop {r0, lr}\n"
        "RetryWSTO:\n"
        "mov r2, #0\n"
        "ldrex r1, [r0]\n"
        "cbz r1, BlockWSTO\n"
        "strex r3, r2, [r0]\n"
        "cmp r3, #0\n"
        "bne RetryWSTO\n"
        "dmb\n"
        "mov r0, r1\n"
        "bx lr\n"
        "BlockWSTO:\n"
        "push {r0, lr}\n"
        "bl BlockOnSemOrTO\n"
        "cmp r0, #0\n"
        "pop {r0, lr}\n"
        "beq RetryWSTO\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

u32 MOS_NAKED MOS_ISR_SAFE MosPollForSignal(MosSem * sem) {
    MOS_USED_PARAM(sem);
    asm volatile (
        "mov r3, #0\n"
        "RetryPS:\n"
        "ldrex r1, [r0]\n"
        "cbz r1, FailPS\n"
        "strex r2, r3, [r0]\n"
        "cmp r2, #0\n"
        "bne RetryPS\n"
        "dmb\n"
        "mov r0, r1\n"
        "bx lr\n"
        "FailPS:\n"
        "mov r0, #0\n"
        "bx lr"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MOS_NAKED MOS_ISR_SAFE MosRaiseSignal(MosSem * sem, u32 flags) {
    MOS_USED_PARAM(sem);
    MOS_USED_PARAM(flags);
    asm volatile (
        "push { lr }\n"
        "RetryRS:\n"
        "ldrex r2, [r0]\n"
        "orr r2, r2, r1\n"
        "strex r3, r2, [r0]\n"
        "cmp r3, #0\n"
        "bne RetryRS\n"
        "dmb\n"
        "bl ReleaseOnSem\n"
        "pop { pc }"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MosAssertAt(char * file, u32 line) {
    if (PrintfHook) (*PrintfHook)("Assertion failed in %s on line %u\n", file, line);
    if (RunningThread != NO_SUCH_THREAD)
        SetRunningThreadStateAndYield(THREAD_TIME_TO_STOP);
    // not always reachable
    while (1);
}
