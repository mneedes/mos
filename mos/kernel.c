
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Microkernel
//

#include "mos_phal.h"
#include "mos/kernel.h"

#include <errno.h>

#if (MOS_FP_CONTEXT_SWITCHING == true)
  #if (__FPU_USED == 1U)
    #define ENABLE_FP_CONTEXT_SAVE    1
  #else
    #define ENABLE_FP_CONTEXT_SAVE    0
    #error "Processor does not support hardware floating point"
  #endif
#else
  #define ENABLE_FP_CONTEXT_SAVE    0
#endif

#define NO_SUCH_THREAD       NULL
#define NO_SUCH_PRI          0xffff
#define STACK_CANARY         0xba5eba11

#define EVENT(e, v) \
    { if (MOS_ENABLE_EVENTS) (*EventHook)((MOS_EVENT_ ## e), (v)); }

typedef enum {
    ELM_THREAD,
    ELM_TIMER,
    ELM_MUTEX,
    ELM_SEM,
    ELM_MUX,
} ElmType;

typedef struct {
    u32 SWSAVE[8];   // R4-R11
#if (ENABLE_FP_CONTEXT_SAVE == 1)
    u32 LR_EXC;      // Exception LR
#endif
    u32 HWSAVE[4];   // R0-R3
    u32 R12;         // R12
    u32 LR;          // R14
    u32 PC;          // R15
    u32 PSR;
#if (ENABLE_FP_CONTEXT_SAVE == 1)
    u32 ALIGNMENT_PAD;  // Not sure this is needed
#endif
} StackFrame;

typedef enum {
    THREAD_UNINIT = 0,
    THREAD_INIT,
    THREAD_STOPPED,
    THREAD_TIME_TO_DIE,
    THREAD_RUNNABLE,
    THREAD_WAIT_FOR_MUTEX,
    THREAD_WAIT_FOR_SEM,
    THREAD_WAIT_ON_MUX,
    THREAD_WAIT_FOR_STOP,
    THREAD_WAIT_FOR_TICK = 16,
    THREAD_WAIT_FOR_SEM_OR_TICK = THREAD_WAIT_FOR_SEM + 16,
    THREAD_WAIT_ON_MUX_OR_TICK = THREAD_WAIT_ON_MUX + 16,
    THREAD_WAIT_FOR_STOP_OR_TICK = THREAD_WAIT_FOR_STOP + 16,
} ThreadState;

typedef struct Thread {
    u32 sp;
    error_t err_no;
    ThreadState state;
    MosList run_q;
    MosListElm tmr_q;
    u32 wake_tick;
    MosThreadPriority pri;
    u16 stop_request;
    union {
        MosMutex * mtx;
        MosSem * sem;
        struct Thread * thd;
    } wait;
    MosMux * mux;
    s32 rtn_val;
    s32 mux_idx;
    MosHandler * kill_handler;
    s32 kill_arg;
    u8 * stack_bottom;
    u32 stack_size;
} Thread;

// Parameters
static u8 IntPriMaskLow;
static MosParams Params = {
   .version = MOS_TO_STR(MOS_VERSION),
   .thread_pri_hi = 0,
   .thread_pri_low = MOS_MAX_THREAD_PRIORITIES - 1,
   .int_pri_hi = 0,
   .int_pri_low = 0,
   .micro_sec_per_tick = MOS_MICRO_SEC_PER_TICK,
   .fp_support_en = ENABLE_FP_CONTEXT_SAVE
};

// Hooks
static MosRawPrintfHook * PrintfHook = NULL;
static MosSleepHook * SleepHook = NULL;
static MosWakeHook * WakeHook = NULL;
static void DummyEventHook(MosEvent e, u32 v) { }
static MosEventHook * EventHook = DummyEventHook;
static MosThreadFreeHook * ThreadFreeHook = NULL;

// Threads and Events
static Thread * RunningThread = NO_SUCH_THREAD;
static error_t * ErrNo;
static Thread IdleThread;
static MosList PriQueues[MOS_MAX_THREAD_PRIORITIES];
static u32 IntDisableCount = 0;

// Timers and Ticks
static MosList Timers;
static volatile u32 TickCount = MOS_START_TICK_COUNT;
static volatile u32 TickInterval = 1;
static u32 MaxTickInterval;
static u32 CyclesPerTick;
static u32 MOS_USED CyclesPerMicroSec;

// Idle thread stack and initial PSP safe storage
//  TODO: Might want to make this big enough to handle a FP stack frame in case FP used in init?
static u8 MOS_STACK_ALIGNED IdleStack[2 * sizeof(StackFrame)];

// Mask interrupts by priority, primarily for temporarily
//   disabling context switches.
static void MOS_INLINE SetBasePri(u32 pri) {
    asm volatile (
        "msr basepri, %0\n\t"
        "isb"
            : : "r" (pri) : "memory"
    );
}

void MosDisableInterrupts(void) {
    if (IntDisableCount++ == 0) {
        asm volatile ( "cpsid if" );
    }
}

void MosEnableInterrupts(void) {
    if (IntDisableCount == 0) return;
    if (--IntDisableCount == 0) {
        asm volatile ( "cpsie if" );
    }
}

u32 MOS_NAKED MosGetIRQNumber(void) {
    asm volatile (
        "mrs r0, psr\n\t"
        "and r0, #255\n\t"
        "bx lr\n\t"
            : : : "r0"
    );
}

static MOS_INLINE void SetThreadState(Thread * thd, ThreadState state) {
    asm volatile ( "dmb" );
    thd->state = state;
}

static MOS_INLINE void YieldThread() {
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

static void ThreadExit(s32 rtn_val) {
    SetBasePri(IntPriMaskLow);
    RunningThread->rtn_val = rtn_val;
    SetThreadState(RunningThread, THREAD_STOPPED);
    asm volatile ( "dmb" );
    if (MosIsOnList(&RunningThread->tmr_q.link))
        MosRemoveFromList(&RunningThread->tmr_q.link);
    MosRemoveFromList(&RunningThread->run_q);
    YieldThread();
    SetBasePri(0);
    // Not reachable
    while (1)
        ;
}

static s32 DefaultKillHandler(s32 arg) {
    return arg;
}

static void InitThread(Thread * thd, MosThreadPriority pri,
                       MosThreadEntry * entry, s32 arg,
                       u8 * stack_bottom, u32 stack_size) {
    // Initialize Stack
    *((u32 *) stack_bottom) = STACK_CANARY;
    StackFrame * sf = (StackFrame *) (stack_bottom + stack_size);
    sf--;
    // Set Thumb Mode
    sf->PSR = 0x01000000;
    sf->PC = (u32)entry;
    sf->LR = (u32)ThreadExit;
    sf->R12 = 0;
    sf->HWSAVE[0] = arg;
#if (ENABLE_FP_CONTEXT_SAVE == 1)
    sf->LR_EXC = 0xfffffffd;
#endif
    // Initialize context and state
    thd->sp = (u32)sf;
    thd->err_no = 0;
    thd->pri = pri;
    thd->stop_request = false;
    thd->kill_handler = DefaultKillHandler;
    thd->kill_arg = 0;
    thd->stack_bottom = stack_bottom;
    thd->stack_size = stack_size;
}

void SysTick_Handler(void) {
    if (RunningThread == NO_SUCH_THREAD) return;
    TickCount += TickInterval;
    YieldThread();
    EVENT(TICK, TickCount);
}

static bool CheckMux(Thread * thd, MosThreadPriority pri, bool first_scan) {
    bool found = false;
    MosMux * mux = thd->mux;
    for (u32 idx = 0; idx < mux->num; idx++) {
        MosSem * sem;
        switch (mux->entries[idx].type) {
        case MOS_WAIT_SEM:
            sem = mux->entries[idx].ptr.sem;
            break;
        case MOS_WAIT_RECV_QUEUE:
            sem = &mux->entries[idx].ptr.q->sem_head;
            break;
        case MOS_WAIT_SEND_QUEUE:
            sem = &mux->entries[idx].ptr.q->sem_tail;
            break;
        default:
            continue;
        }
        // Another thread of equal priority is waiting on a sem,
        //   keep tick running
        if (!first_scan) return true;
        // Lock interrupts since semaphore may be given from ISR
        asm volatile ( "cpsid if" );
        if (sem->count != 0) {
            if (first_scan) {
                sem->block_pri = NO_SUCH_PRI;
                if (!found) thd->mux_idx = idx;
            }
            found = true;
        } else if (sem->block_pri > pri) sem->block_pri = pri;
        asm volatile ( "cpsie if" );
    }
    return found;
}

static Thread *
ProcessThread(Thread * thd, MosThreadPriority pri, bool first_scan) {
    switch (thd->state & 0xF) {
    case THREAD_RUNNABLE:
        return thd;
    case THREAD_WAIT_FOR_MUTEX:
        {
            // Check mutex (nested priority inheritance algorithm)
            Thread * owner = (Thread *) thd->wait.mtx->owner;
            if (owner == NO_SUCH_THREAD) {
                if (first_scan) thd->wait.mtx->to_yield = false;
                return thd;
            } else {
                if (owner->pri > pri) {
                    // Owner thread is lower priority
                    if (owner->state == THREAD_RUNNABLE) {
                        if (first_scan) thd->wait.mtx->to_yield = true;
                        return owner;
                    } else return ProcessThread(owner, pri, first_scan);
                // Owner thread is equal priority, return non-NULL on 2nd scan
                //   to keep tick running
                } else if (!first_scan) return thd;
            }
        }
        break;
    case THREAD_WAIT_FOR_SEM:
        {
            // Another thread of equal priority is waiting on a sem,
            //   keep tick running
            if (!first_scan) return thd;
            // Lock interrupts since semaphore may be given from ISR
            asm volatile ( "cpsid if" );
            if (thd->wait.sem->count != 0) {
                thd->wait.sem->block_pri = NO_SUCH_PRI;
                asm volatile ( "cpsie if" );
                return thd;
            } else {
                if (thd->wait.sem->block_pri > pri) thd->wait.sem->block_pri = pri;
                asm volatile ( "cpsie if" );
            }
        }
        break;
    case THREAD_WAIT_ON_MUX:
        if (CheckMux(thd, pri, first_scan)) return thd;
        break;
    case THREAD_WAIT_FOR_STOP:
        if (thd->wait.thd->state == THREAD_STOPPED) return thd;
        break;
    default:
        break;
    }
    return NULL;
}

static Thread *
ScanThreads(MosThreadPriority * pri, MosList ** thd_elm, bool first_scan) {
    MosList * elm = *thd_elm;
    for (; *pri < MOS_MAX_THREAD_PRIORITIES;) {
        for (; elm != &PriQueues[*pri]; elm = elm->next) {
            Thread * thd = container_of(elm, Thread, run_q);
            Thread * run_thd = ProcessThread(thd, *pri, first_scan);
            if (run_thd) {
                *thd_elm = elm;
                return run_thd;
            }
        }
        if (!first_scan) break;
        (*pri)++;
        elm = PriQueues[*pri].next;
    }
    return NULL;
}

static u32 MOS_USED Scheduler(u32 sp) {
    EVENT(SCHEDULER_ENTRY, 0);
    // Save SP and ErrNo context
    if (RunningThread != NO_SUCH_THREAD) {
        RunningThread->sp = sp;
        RunningThread->err_no = *ErrNo;
    } else RunningThread = &IdleThread;
    // Obtain current tick count
    s32 adj_tick_count = TickCount;
    if (TickInterval > 1) {
        // Adjust TickCount / Remaining ticks
        asm volatile ( "cpsid if" );
        u32 load = SysTick->LOAD;
        u32 val = SysTick->VAL;
        asm volatile ( "cpsie if" );
        adj_tick_count = TickCount + (load - val) / CyclesPerTick;
    }
    if (RunningThread->state == THREAD_TIME_TO_DIE) {
        // Arrange death of running thread via kill handler
        if (MosIsOnList(&RunningThread->tmr_q.link))
            MosRemoveFromList(&RunningThread->tmr_q.link);
        InitThread(RunningThread, RunningThread->pri,
                   RunningThread->kill_handler, RunningThread->kill_arg,
                   RunningThread->stack_bottom, RunningThread->stack_size);
        SetThreadState(RunningThread, THREAD_RUNNABLE);
    } else if (RunningThread->state & THREAD_WAIT_FOR_TICK) {
        // Update running thread timer state (insertion sort in timer queue)
        s32 rem_ticks = (s32)RunningThread->wake_tick - adj_tick_count;
        MosList * tmr;
        for (tmr = Timers.next; tmr != &Timers; tmr = tmr->next) {
            s32 wake_tick;
            if (((MosListElm *)tmr)->type == ELM_THREAD) {
                Thread * thd = container_of(tmr, Thread, tmr_q);
                wake_tick = (s32)thd->wake_tick;
            } else {
                MosTimer * tmr_tmr = container_of(tmr, MosTimer, tmr_q);
                wake_tick = (s32)tmr_tmr->wake_tick;
            }
            s32 tmr_rem_ticks = wake_tick - adj_tick_count;
            if (rem_ticks <= tmr_rem_ticks) break;
        }
        MosAddToListBefore(tmr, &RunningThread->tmr_q.link);
    }
    // Process timer queue
    MosList * tmr_save;
    for (MosList * tmr = Timers.next; tmr != &Timers; tmr = tmr_save) {
        tmr_save = tmr->next;
        if (((MosListElm *)tmr)->type == ELM_THREAD) {
            Thread * thd = container_of(tmr, Thread, tmr_q);
            s32 rem_ticks = (s32)thd->wake_tick - adj_tick_count;
            if (rem_ticks <= 0) {
                // Signal timeout
                thd->wait.sem = NULL;
                thd->mux_idx = -1;
                SetThreadState(thd, THREAD_RUNNABLE);
                MosRemoveFromList(tmr);
            } else break;
        } else {
            MosTimer * tmr_tmr = container_of(tmr, MosTimer, tmr_q);
            s32 rem_ticks = (s32)tmr_tmr->wake_tick - adj_tick_count;
            if (rem_ticks <= 0) {
                if (MosSendToQueue(tmr_tmr->q, tmr_tmr->msg)) MosRemoveFromList(tmr);
            } else break;
        }
    }
    // Process Priority Queues
    // Start scan at first thread of highest priority, looking for one or two
    //   runnable threads (for tick enable), and if no threads are runnable
    //   schedule idle thread.
    bool tick_en = false;
    MosThreadPriority pri = 0;
    MosList * thd_elm = PriQueues[pri].next;
    Thread * run_thd = ScanThreads(&pri, &thd_elm, true);
    if (run_thd) {
        // Remove from timer and set thread runnable
        if ((run_thd->state & THREAD_WAIT_FOR_TICK) == THREAD_WAIT_FOR_TICK) {
            if (MosIsOnList(&run_thd->tmr_q.link))
                MosRemoveFromList(&run_thd->tmr_q.link);
        }
        SetThreadState(run_thd, THREAD_RUNNABLE);
        // Look for second potentially runnable thread
        if (ScanThreads(&pri, &thd_elm, false)) tick_en = true;
        // Move thread to end of priority queue (round-robin)
        if (!MosIsLastElement(&PriQueues[run_thd->pri], &run_thd->run_q))
            MosMoveToEndOfList(&PriQueues[run_thd->pri], &run_thd->run_q);
    } else run_thd = &IdleThread;
    // Determine next timer interval
    //   If more than 1 active thread, enable tick to commutate threads
    //   If there is an active timer, delay tick to next expiration up to max
    u32 next_tick_interval = 1;
    if (tick_en == false) {
        // This ensures that the LOAD value will fit in SysTick counter...
        s32 tmr_ticks_rem = 0x7fffffff;
        if (!MosIsListEmpty(&Timers)) {
            if (((MosListElm *)Timers.next)->type == ELM_THREAD) {
                Thread * thd = container_of(Timers.next, Thread, tmr_q);
                tmr_ticks_rem = (s32)thd->wake_tick - adj_tick_count;
            } else {
                MosTimer * tmr_tmr = container_of(Timers.next, MosTimer, tmr_q);
                tmr_ticks_rem = (s32)tmr_tmr->wake_tick - adj_tick_count;
            }
        }
        if (tmr_ticks_rem != 0x7fffffff) {
            // This ensures that the LOAD value will fit in SysTick counter...
            if (tmr_ticks_rem < MaxTickInterval)
                next_tick_interval = tmr_ticks_rem;
            else next_tick_interval = MaxTickInterval;
        } else if (MOS_KEEP_TICKS_RUNNING == false) {
            next_tick_interval = 0;
        } else next_tick_interval = MaxTickInterval;
    }
    if (TickInterval != next_tick_interval) {
        // Disable interrupts so LOAD and VAL sets and gets are atomic
        asm volatile ( "cpsid if" );
        if (next_tick_interval > 0) {
            // Reset timer with new tick increment
            SysTick->LOAD = (next_tick_interval * CyclesPerTick) - 1;
            SysTick->VAL = 0;
            asm volatile ( "cpsie if" );
            if (TickInterval == 0) SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
        } else {
            // Counter is free-running; ticks are no longer being accumulated,
            // but still set timer so adjusted tick calculation is more or less
            // accurate.
            SysTick->LOAD = CyclesPerTick - 1;
            SysTick->VAL = 0;
            asm volatile ( "cpsie if" );
            SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;
        }
        // Adjust tick count and change interval
        TickCount = adj_tick_count;
        TickInterval = next_tick_interval;
    }
    // Set next thread ID and errno and return its stack pointer
    RunningThread = run_thd;
    *ErrNo = RunningThread->err_no;
    EVENT(SCHEDULER_EXIT, 0);
    return (u32)RunningThread->sp;
}

#if (ENABLE_FP_CONTEXT_SAVE == 1)

void MOS_NAKED PendSV_Handler(void) {
    // Floating point context switch (lazy stacking)
    asm volatile (
        "mrs r0, psp\n\t"
        "tst lr, #16\n\t"
        "it eq\n\t"
        "vstmdbeq r0!, {s16-s31}\n\t"
        "stmdb r0!, {r4-r11, lr}\n\t"
        "bl Scheduler\n\t"
        "ldmfd r0!, {r4-r11, lr}\n\t"
        "tst lr, #16\n\t"
        "it eq\n\t"
        "vldmiaeq r0!, {s16-s31}\n\t"
        "msr psp, r0\n\t"
        "bx lr\n\t"
    );
}

#else

void MOS_NAKED PendSV_Handler(void) {
    // Vanilla context switch without floating point.
    asm volatile (
        "mrs r0, psp\n\t"
        "stmdb r0!, {r4-r11}\n\t"
        "bl Scheduler\n\t"
        "mvn lr, #2\n\t"
        "ldmfd r0!, {r4-r11}\n\t"
        "msr psp, r0\n\t"
        "bx lr\n\t"
    );
}

#endif

// TODO: Limit stack dump to top of stack / end of memory
// TODO: Clear fault bits after thread exception ?
// TODO: FPU stack? -- increase dump size
// TODO: Print thread name
static void MOS_USED FaultHandler(u32 * msp, u32 * psp, u32 psr, u32 lr) {
    char * fault_type[] = {
        "Hard", "Mem", "Bus", "Usage", "Imprecise Bus"
    };
    bool in_isr = ((lr & 0x8) == 0x0);
    if (PrintfHook) {
        u32 cfsr = SCB->CFSR;
        u32 fault_no = (psr & 0xf) - 3;
        if (fault_no == 2 && (cfsr & 0x400)) fault_no = 4;
        (*PrintfHook)("\n*** %s Fault %s", fault_type[fault_no],
                          in_isr ? "IN ISR " : "");
        if (RunningThread == NO_SUCH_THREAD) (*PrintfHook)("(Before MOS Run) ***\n");
        else {
            (*PrintfHook)("(Thread %08X) ***\n", RunningThread);
#if 0
            // Check for stack overflow
            for (u32 ix = 0; ix < MAX_THREADS; ix++) {
                if (Threads[ix].state != THREAD_UNINIT) {
                    if (*((u32 *)ThreadData[ix].stack_addr) != STACK_CANARY)
                        (*PrintfHook)("!!! ThreadID %u Stack overflow !!!\n", ix);
                }
            }
#endif
        }

        (*PrintfHook)(" HFSR: %08X  CFSR: %08X AFSR: %08X\n",
                          SCB->HFSR, cfsr, SCB->AFSR);
        (*PrintfHook)(" BFAR: %08X MMFAR: %08X\n\n", SCB->BFAR, SCB->MMFAR);

        bool use_psp = ((lr & 0x4) == 0x4);
        u32 * sp = use_psp ? psp : msp;
        (*PrintfHook)("%s Stack @%08X:\n", use_psp ? "Process" : "Main", (u32) sp);
        (*PrintfHook)(" %08X %08X %08X %08X  (R0 R1 R2 R3)\n",  sp[0], sp[1], sp[2], sp[3]);
        (*PrintfHook)(" %08X %08X %08X %08X (R12 LR PC PSR)\n", sp[4], sp[5], sp[6], sp[7]);
        sp += 8;
        for (u32 ix = 0; ix < 4; ix++, sp += 4) {
            (*PrintfHook)(" %08X %08X %08X %08X\n", sp[0], sp[1], sp[2], sp[3]);
        }
        (*PrintfHook)("\n");
    }
    if (RunningThread == NO_SUCH_THREAD || in_isr) {
        // Hang if fault occurred anywhere but in thread context
        while (1)
          ;
    } else {
        // Stop thread if fault occurred in thread context
        SetThreadState(RunningThread, THREAD_TIME_TO_DIE);
        YieldThread();
    }
}

void MOS_NAKED HardFault_Handler(void) {
    asm volatile (
        "mrs r0, msp\n\t"
        "mrs r1, psp\n\t"
        "mrs r2, psr\n\t"
        "mov r3, lr\n\t"
        "b FaultHandler\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MOS_NAKED MemManage_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

void MOS_NAKED BusFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

void MOS_NAKED UsageFault_Handler(void) {
    asm volatile (
        "b HardFault_Handler"
    );
}

u32 MosGetTickCount(void) {
    // Adjust tick count based on value in SysTick counter
    asm volatile ( "cpsid if" );
    u32 load = SysTick->LOAD;
    u32 val = SysTick->VAL;
    asm volatile ( "cpsie if" );
    u32 adj_tick_cnt = TickCount + (load - val) / CyclesPerTick;
    return adj_tick_cnt;
}

static void MOS_USED SetTimeout(u32 ticks) {
    // Adjust tick count based on value in SysTick counter
    asm volatile ( "cpsid if" );
    u32 load = SysTick->LOAD;
    u32 val = SysTick->VAL;
    asm volatile ( "cpsie if" );
    u32 adj_tick_cnt = TickCount + (load - val) / CyclesPerTick;
    RunningThread->wake_tick = adj_tick_cnt + ticks;
}

void MosDelayThread(u32 ticks) {
    SetTimeout(ticks);
    SetRunningThreadStateAndYield(THREAD_WAIT_FOR_TICK);
}

void MOS_NAKED MosDelayMicroSec(u32 usec) {
    asm volatile (
        "ldr r1, _CyclesPerMicroSec\n\t"
        "ldr r1, [r1]\n\t"
        "mul r0, r0, r1\n\t"
        "sub r0, #13\n\t"  // Overhead calibration
        "delay:\n\t"
#if (MOS_CYCLES_PER_INNER_LOOP == 3)
        "subs r0, #3\n\t"
#elif (MOS_CYCLES_PER_INNER_LOOP == 1)
        "subs r0, #1\n\t"
#else
#error "Invalid selection for inner loop cycles"
#endif
        "bgt delay\n\t"
        "bx lr\n\t"
        ".balign 4\n\t"
        "_CyclesPerMicroSec: .word CyclesPerMicroSec\n\t"
            : : : "r0", "r1"
    );
}

// Timers (work in progress, needs locks etc etc etc)

void MosInitTimer(MosTimer * timer, MosQueue * q) {
    MosInitListElm(&timer->tmr_q, ELM_TIMER);
    timer->q = q;
}

static void AddMessageTimer(MosTimer * timer) {
    MosList * tmr;
    SetBasePri(IntPriMaskLow);
    u32 tick_count = MosGetTickCount();
    timer->wake_tick = tick_count + timer->ticks;
    for (tmr = Timers.next; tmr != &Timers; tmr = tmr->next) {
        if (((MosListElm *)tmr)->type == ELM_THREAD) {
            Thread * thd = container_of(tmr, Thread, tmr_q);
            s32 tmr_rem_ticks = (s32)thd->wake_tick - tick_count;
            if (timer->ticks <= tmr_rem_ticks) break;
        } else {
            MosTimer * tmr_tmr = container_of(tmr, MosTimer, tmr_q);
            s32 tmr_rem_ticks = (s32)tmr_tmr->wake_tick - tick_count;
            if (timer->ticks <= tmr_rem_ticks) break;
        }
    }
    MosAddToListBefore(tmr, &timer->tmr_q.link);
    SetBasePri(0);
}

void MosSetTimer(MosTimer * timer, u32 ticks, u32 msg) {
    timer->ticks = ticks;
    timer->msg = msg;
    AddMessageTimer(timer);
}

void MosCancelTimer(MosTimer * timer) {
    SetBasePri(IntPriMaskLow);
    MosRemoveFromList(&timer->tmr_q.link);
    SetBasePri(0);
}

void MosResetTimer(MosTimer * timer) {
    MosCancelTimer(timer);
    AddMessageTimer(timer);
}

static s32 IdleThreadEntry(s32 arg) {
    while (1) {
        //TODO:  ThreadFreeHook?  if (ThreadFreeHook) (*ThreadFreeHook)(NULL);
        if (SleepHook) (*SleepHook)();
        asm volatile (
            "dsb\n\t"
            "wfi\n\t"
        );
        if (WakeHook) (*WakeHook)();
    }
    return 0;
}

void MosInit(void) {
    // Trap Divide By 0 and "Unintentional" Unaligned Accesses
    SCB->CCR |= (SCB_CCR_DIV_0_TRP_Msk | SCB_CCR_UNALIGN_TRP_Msk);
    // Enable Bus, Memory and Usage Faults in general
    SCB->SHCSR |= (SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_MEMFAULTENA_Msk |
                   SCB_SHCSR_USGFAULTENA_Msk);
#if (ENABLE_FP_CONTEXT_SAVE == 1)
    // Ensure lazy stacking is enabled (for floating point)
    FPU->FPCCR |= (FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk);
#else
    FPU->FPCCR &= ~(FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk);
#endif
    // Save errno pointer for use during context switch
    ErrNo = __errno();
    // Set up timers with tick-reduction
    CyclesPerTick = SysTick->LOAD + 1;
    MaxTickInterval = ((1 << 24) - 1) / CyclesPerTick;
    CyclesPerMicroSec = CyclesPerTick / MOS_MICRO_SEC_PER_TICK;
    // Set lowest preemption priority for SysTick and PendSV (highest number).
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
    // Initialize empty priority, event and timer queues
    for (MosThreadPriority pri = 0; pri < MOS_MAX_THREAD_PRIORITIES; pri++)
        MosInitList(&PriQueues[pri]);
    MosInitList(&Timers);
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
void MosRegisterThreadFreeHook(MosThreadFreeHook * hook) { ThreadFreeHook = hook; };

void MosRunScheduler(void) {
    // Ensure opaque thread structure has same size as internal structure
    MosAssert(sizeof(Thread) == sizeof(MosThread));
    // Start PSP in a safe place for first PendSV and then enable interrupts
    asm volatile (
        "ldr r0, psp_start\n\t"
        "msr psp, r0\n\t"
        "mov r0, #0\n\t"
        "msr basepri, r0\n\t"
        "cpsie if\n\t"
        "b SkipRS\n\t"
        ".balign 4\n\t"
        "psp_start: .word IdleStack + 64\n\t"
        "SkipRS:\n\t"
            : : : "r0"
    );
    // Invoke PendSV handler to start scheduler (first context switch)
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    asm volatile ( "isb" );
    // Not reachable
    while (1)
        ;
}

const MosParams * MosGetParams(void) {
    return (const MosParams *) &Params;
}

void MosYieldThread(void) {
    if (RunningThread == NO_SUCH_THREAD) return;
    // Invoke PendSV handler to potentially perform context switch
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    asm volatile ( "isb" );
}

MosThread * MosGetThread(void) {
    return (MosThread *)RunningThread;
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

// Might kick this to internal
void MosSetStack(MosThread * _thd, u8 * stack_bottom, u32 stack_size) {
    Thread * thd = (Thread *)_thd;
    thd->stack_bottom = stack_bottom;
    thd->stack_size = stack_size;
}

bool MosInitThread(MosThread * _thd, MosThreadPriority pri,
                   MosThreadEntry * entry, s32 arg,
                   u8 * stack_bottom, u32 stack_size) {
    Thread * thd = (Thread *)_thd;
    if (thd == RunningThread) return false;
    // Stop thread if running
    SetBasePri(IntPriMaskLow);
    ThreadState state = thd->state;
    switch (state) {
    case THREAD_UNINIT:
    case THREAD_INIT:
    case THREAD_STOPPED:
        MosInitList(&thd->run_q);
        MosInitListElm(&thd->tmr_q, ELM_THREAD);
        break;
    default:
        if (MosIsOnList(&thd->tmr_q.link))
            MosRemoveFromList(&thd->tmr_q.link);
        MosRemoveFromList(&thd->run_q);
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
            MosAddToList(&PriQueues[thd->pri], &thd->run_q);
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
    if (!MosInitThread(_thd, pri, entry, arg, stack_bottom, stack_size)) return false;
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
    case THREAD_TIME_TO_DIE:
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

void MosChangeThreadPriority(MosThread * _thd, MosThreadPriority pri) {
    Thread * thd = (Thread *)_thd;
    SetBasePri(IntPriMaskLow);
    if (thd->pri == pri) {
        SetBasePri(0);
        return;
    }
    thd->pri = pri;
    MosRemoveFromList(&thd->run_q);
    MosAddToList(&PriQueues[pri], &thd->run_q);
    if (RunningThread != NO_SUCH_THREAD && pri < thd->pri)
        YieldThread();
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
    RunningThread->wait.thd = thd;
    SetRunningThreadStateAndYield(THREAD_WAIT_FOR_STOP);
    return thd->rtn_val;
}

bool MosWaitForThreadStopOrTO(MosThread * _thd, s32 * rtn_val, u32 ticks) {
    Thread * thd = (Thread *)_thd;
    SetTimeout(ticks);
    RunningThread->wait.thd = thd;
    SetRunningThreadStateAndYield(THREAD_WAIT_FOR_STOP_OR_TICK);
    if (thd->wait.thd == NULL) return false;
    *rtn_val = thd->rtn_val;
    return true;
}

void MosKillThread(MosThread * _thd) {
    Thread * thd = (Thread *)_thd;
    if (thd == RunningThread) {
        SetRunningThreadStateAndYield(THREAD_TIME_TO_DIE);
        // not reachable
        while (1)
            ;
    }
    else MosInitAndRunThread((MosThread *) thd, thd->pri, thd->kill_handler,
                             thd->kill_arg, thd->stack_bottom,
                             thd->stack_size);
}

void MosSetKillHandler(MosThread * _thd, MosHandler * handler, s32 arg) {
    Thread * thd = (Thread *)_thd;
    SetBasePri(IntPriMaskLow);
    if (handler) thd->kill_handler = handler;
    else thd->kill_handler = DefaultKillHandler;
    thd->kill_arg = arg;
    SetBasePri(0);
}

void MosSetKillArg(MosThread * _thd, s32 arg) {
    Thread * thd = (Thread *)_thd;
    thd->kill_arg = arg;
}

void MosInitMutex(MosMutex * mtx) {
    mtx->owner = NO_SUCH_THREAD;
    mtx->depth = 0;
    mtx->to_yield = false;
}

static void MOS_USED BlockOnMutex(MosMutex * mtx) {
    RunningThread->wait.mtx = mtx;
    SetRunningThreadStateAndYield(THREAD_WAIT_FOR_MUTEX);
}

void MOS_NAKED MosTakeMutex(MosMutex * mtx) {
    asm volatile (
        "ldr r1, _ThreadID\n\t"
        "ldr r1, [r1]\n\t"
        "RetryTM:\n\t"
        "ldrex r2, [r0]\n\t"
        "cmp r2, r1\n\t"
        "beq IncTM\n\t"
        "cmp r2, #0\n\t"
        "bne BlockTM\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryTM\n\t"
        "IncTM:\n\t"
        "ldr r2, [r0, #4]\n\t"
        "add r2, #1\n\t"
        "str r2, [r0, #4]\n\t"
        "dmb\n\t"
        "bx lr\n\t"
        "BlockTM:\n\t"
        "push {r0, r1, lr}\n\t"
        "bl BlockOnMutex\n\t"
        "pop {r0, r1, lr}\n\t"
        "b RetryTM\n\t"
        ".balign 4\n\t"
        "_ThreadID: .word RunningThread\n\t"
            // Explicit clobber list prevents compiler from making
            // assumptions about registers not being changed as
            // this assembly code calls a C function.  Normally C
            // ABI treats r0-r3 as input or scratch registers.
            : : : "r0", "r1", "r2", "r3"
    );
}

bool MOS_NAKED MosTryMutex(MosMutex * mtx) {
    asm volatile (
        "ldr r1, _ThreadID2\n\t"
        "ldr r1, [r1]\n\t"
        "RetryTRM:\n\t"
        "ldrex r2, [r0]\n\t"
        "cmp r2, r1\n\t"
        "beq IncTRM\n\t"
        "cmp r2, #0\n\t"
        "bne FailTRM\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryTRM\n\t"
        "IncTRM:\n\t"
        "ldr r2, [r0, #4]\n\t"
        "add r2, #1\n\t"
        "str r2, [r0, #4]\n\t"
        "dmb\n\t"
        "mov r1, #1\n\t"
        "bx lr\n\t"
        "FailTRM:\n\t"
        "mov r0, #0\n\t"
        "bx lr\n\t"
        ".balign 4\n\t"
        "_ThreadID2: .word RunningThread\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MOS_NAKED MosGiveMutex(MosMutex * mtx) {
    asm volatile (
        "ldr r1, [r0, #4]\n\t"
        "sub r1, #1\n\t"
        "str r1, [r0, #4]\n\t"
        "cbnz r1, SkipGM\n\t"
        "mov r1, #0\n\t"
        "RetryGM:\n\t"
        "ldrex r2, [r0]\n\t"     // Not sure this is needed
        "strex r2, r1, [r0]\n\t" //  ... just store 0 ?
        "cmp r2, #0\n\t"
        "bne RetryGM\n\t"
        "ldr r1, [r0, #8]\n\t"
        "cbz r1, SkipGM\n\t"
        "push { lr }\n\t"
        "bl MosYieldThread\n\t"
        "pop { lr }\n\t"
        "SkipGM:\n\t"
        "bx lr\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MosRestoreMutex(MosMutex * mtx) {
    if (mtx->owner == (void *)RunningThread) {
        mtx->depth = 1;
        MosGiveMutex(mtx);
    }
}

bool MosIsMutexOwner(MosMutex * mtx) {
    return (mtx->owner == (void *)RunningThread);
}

void MosInitSem(MosSem * sem, u32 start_count) {
    sem->count = start_count;
    sem->block_pri = NO_SUCH_PRI;
}

static void MOS_USED BlockOnSem(MosSem * sem) {
    RunningThread->wait.sem = sem;
    SetRunningThreadStateAndYield(THREAD_WAIT_FOR_SEM);
}

void MOS_NAKED MosTakeSem(MosSem * sem) {
    asm volatile (
        "RetryTS:\n\t"
        "ldrex r1, [r0]\n\t"
        "cbz r1, BlockTS\n\t"
        "sub r1, #1\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryTS\n\t"
        "dmb\n\t"
        "bx lr\n\t"
        "BlockTS:\n\t"
        "push {r0, lr}\n\t"
        "bl BlockOnSem\n\t"
        "pop {r0, lr}\n\t"
        "b RetryTS\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

static bool MOS_USED BlockOnSemOrTO(MosSem * sem) {
    RunningThread->wait.sem = sem;
    SetRunningThreadStateAndYield(THREAD_WAIT_FOR_SEM_OR_TICK);
    if (!RunningThread->wait.sem) return true;
    return false;
}

bool MOS_NAKED MosTakeSemOrTO(MosSem * sem, u32 ticks) {
    asm volatile (
        "push {r0, lr}\n\t"
        "mov r0, r1\n\t"
        "bl SetTimeout\n\t"
        "pop {r0, lr}\n\t"
        "RetryTSTO:\n\t"
        "ldrex r1, [r0]\n\t"
        "cbz r1, BlockTSTO\n\t"
        "sub r1, #1\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryTSTO\n\t"
        "dmb\n\t"
        "mov r0, #1\n\t"
        "bx lr\n\t"
        "BlockTSTO:\n\t"
        "push {r0, lr}\n\t"
        "bl BlockOnSemOrTO\n\t"
        "cmp r0, #0\n\t"
        "pop {r0, lr}\n\t"
        "beq RetryTSTO\n\t"
        "mov r0, #0\n\t"
        "bx lr\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

bool MOS_NAKED MosTrySem(MosSem * sem) {
    asm volatile (
        "RetryTRS:\n\t"
        "ldrex r1, [r0]\n\t"
        "cbz r1, FailTRS\n\t"
        "sub r1, #1\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryTRS\n\t"
        "dmb\n\t"
        "mov r0, #1\n\t"
        "bx lr\n\t"
        "FailTRS:\n\t"
        "mov r0, #0\n\t"
        "bx lr\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

static void MOS_USED YieldOnSem(MosSem * sem) {
    SetBasePri(IntPriMaskLow);
    if (sem->block_pri < RunningThread->pri) YieldThread();
    SetBasePri(0);
}

void MOS_NAKED MosGiveSem(MosSem * sem) {
    asm volatile (
        "push { lr }\n\t"
        "RetryGS:\n\t"
        "ldrex r1, [r0]\n\t"
        "add r1, #1\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryGS\n\t"
        "dmb\n\t"
        "bl YieldOnSem\n\t"
        "pop { pc }\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MosInitQueue(MosQueue * queue, u32 * buf, u32 len) {
    queue->buf = buf;
    queue->len = len;
    queue->head = 0;
    queue->tail = 0;
    MosInitSem(&queue->sem_head, 0);
    MosInitSem(&queue->sem_tail, len);
}

// MosSendToQueue() is ISR safe since it disables interrupts for ALL send
// to queue methods.  Receive methods are not ISR safe and can instead use
// PRIMASK since only context switch should be disabled.

bool MosSendToQueue(MosQueue * queue, u32 data) {
    // After taking semaphore context has a "license to write one entry,"
    // but it still must wait if another context is trying to do the same
    // thing in a thread or ISR.
    u32 irq = MosGetIRQNumber();
    if (!irq) MosTakeSem(&queue->sem_tail);
    else if (!MosTrySem(&queue->sem_tail)) return false;
    asm volatile ( "cpsid if" );
    queue->buf[queue->tail] = data;
    if (++queue->tail >= queue->len) queue->tail = 0;
    asm volatile (
        "dmb\n\t"
        "cpsie if\n\t"
    );
    MosGiveSem(&queue->sem_head);
    return true;
}

bool MosSendToQueueOrTO(MosQueue * queue, u32 data, u32 ticks) {
    if (MosTakeSemOrTO(&queue->sem_tail, ticks)) {
        asm volatile ( "cpsid if" );
        queue->buf[queue->tail] = data;
        if (++queue->tail >= queue->len) queue->tail = 0;
        asm volatile (
            "dmb\n\t"
            "cpsie if\n\t"
        );
        MosGiveSem(&queue->sem_head);
        return true;
    }
    return false;
}

u32 MosReceiveFromQueue(MosQueue * queue) {
    MosTakeSem(&queue->sem_head);
    SetBasePri(IntPriMaskLow);
    u32 data = queue->buf[queue->head];
    if (++queue->head >= queue->len) queue->head = 0;
    SetBasePri(0);
    MosGiveSem(&queue->sem_tail);
    return data;
}

bool MosTryReceiveFromQueue(MosQueue * queue, u32 * data) {
    if (MosTrySem(&queue->sem_head)) {
        SetBasePri(IntPriMaskLow);
        *data = queue->buf[queue->head];
        if (++queue->head >= queue->len) queue->head = 0;
        SetBasePri(0);
        MosGiveSem(&queue->sem_tail);
        return true;
    }
    return false;
}

bool MosReceiveFromQueueOrTO(MosQueue * queue, u32 * data, u32 ticks) {
    if (MosTakeSemOrTO(&queue->sem_head, ticks)) {
        SetBasePri(IntPriMaskLow);
        *data = queue->buf[queue->head];
        if (++queue->head >= queue->len) queue->head = 0;
        SetBasePri(0);
        MosGiveSem(&queue->sem_tail);
        return true;
    }
    return false;
}

void MosInitMux(MosMux * mux) {
    mux->num = 0;
}

void MosSetActiveMux(MosMux * mux, MosMuxEntry * entries, u32 len) {
    mux->entries = entries;
    mux->num = len;
    RunningThread->mux = mux;
}

u32 MosWaitOnMux(MosMux * mux) {
    SetRunningThreadStateAndYield(THREAD_WAIT_ON_MUX);
    return (u32)RunningThread->mux_idx;
}

bool MosWaitOnMuxOrTO(MosMux * mux, u32 * idx, u32 ticks) {
    SetTimeout(ticks);
    SetRunningThreadStateAndYield(THREAD_WAIT_ON_MUX_OR_TICK);
    if (RunningThread->mux_idx < 0) return false;
    *idx = (u32)RunningThread->mux_idx;
    return true;
}

void MosAssertAt(char * file, u32 line) {
    if (PrintfHook) (*PrintfHook)("Assertion failed in %s on line %u\n", file, line);
    if (RunningThread != NO_SUCH_THREAD)
        SetRunningThreadStateAndYield(THREAD_TIME_TO_DIE);
    // not always reachable
    while (1)
        ;
}
