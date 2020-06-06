
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

#if (__FPU_USED == 1U)
// TODO: Can use this to enable floating point context save for CM4F
#endif

// TODO: Suppress yields on semaphores if no thread is waiting.  Unlike Mutex
//       Semaphore yields may be tricky since they can happen during high
//       priority interrupts that are interrupting PendSV handler.
// TODO: Timer queue --> For threads / Timer queue --> Timer messages to threads

#define MAX_THREADS      (MOS_MAX_APP_THREADS + 1)
#define NO_SUCH_THREAD    -1
#define STACK_CANARY      0xba5eba11

#define EVENT(e, v) \
    { if (MOS_ENABLE_EVENTS) (*EventHook)((MOS_EVENT_ ## e), (v)); }

typedef struct {
    u32 SWSAVE[8];   // R11-R4
    u32 HWSAVE[4];   // R3-R0
    u32 R12;         // R12
    u32 LR;          // R14
    u32 PC;          // R15
    u32 xPSR;
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
    MosList tmr_q;
    u32 wake_tick;
    MosThreadID id;
    MosThreadPriority pri;
    union {
        MosMutex * mtx;
        MosSem * sem;
        struct Thread * thd;
    } wait;
    MosMux * mux;
} Thread;

typedef struct {
    u16 stop_request;
    s32 rtn_val;
    s32 mux_idx;
    MosHandler * kill_handler;
    s32 kill_arg;
    u8 * stack_addr;
    u32 stack_size;
} ThreadAuxData;

// Parameters
static u8 IntPriMaskLow;
static MosParams Params = { .version = MOS_TO_STR(MOS_VERSION) };

// Hooks
static MosRawPrintfHook * PrintfHook = NULL;
static MosSleepHook * SleepHook = NULL;
static MosWakeHook * WakeHook = NULL;
static void DummyEventHook(MosEvent e, u32 v) { }
static MosEventHook * EventHook = DummyEventHook;

// Threads and Events
static volatile MosThreadID RunningThreadID = NO_SUCH_THREAD;
static volatile error_t * ErrNo;
static Thread Threads[MAX_THREADS];
static MosList PriQueues[MOS_MAX_THREAD_PRIORITIES];
static ThreadAuxData ThreadData[MAX_THREADS];
static u32 IntDisableCount = 0;

// Timers and Ticks
static MosList Timers;
static volatile u32 TickCount = MOS_START_TICK_COUNT;
static volatile u32 TickInterval = 1;
static u32 MaxTickInterval;
static u32 CyclesPerTick;
static u32 MOS_USED CyclesPerMicroSec;

// Idle thread stack and initial PSP safe storage
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

static MOS_INLINE void SetThreadState(MosThreadID id, ThreadState state) {
    asm volatile ( "dmb" );
    Threads[id].state = state;
}

static void ThreadExit(s32 rtn_val) {
    SetBasePri(IntPriMaskLow);
    ThreadData[RunningThreadID].rtn_val = rtn_val;
    SetThreadState(RunningThreadID, THREAD_STOPPED);
    if (MosIsOnList(&Threads[RunningThreadID].tmr_q))
        MosRemoveFromList(&Threads[RunningThreadID].tmr_q);
    MosRemoveFromList(&Threads[RunningThreadID].run_q);
    SetBasePri(0);
    MosYieldThread();
    // Not reachable
    while (1)
        ;
}

static s32 DefaultKillHandler(s32 arg) {
    return arg;
}

static void InitThread(MosThreadID id, MosThreadPriority pri,
                       MosThreadEntry * entry, s32 arg,
                       u8 * s_addr, u32 s_size) {
    // Initialize aux data
    ThreadData[id].stop_request = false;
    ThreadData[id].kill_handler = DefaultKillHandler;
    ThreadData[id].kill_arg = 0;
    ThreadData[id].stack_addr = s_addr;
    ThreadData[id].stack_size = s_size;
    // Initialize Stack
    *((u32 *) s_addr) = STACK_CANARY;
    StackFrame * sf = (StackFrame *) (s_addr + s_size);
    sf--;
    // Set Thumb Mode
    sf->xPSR = 0x01000000;
    sf->PC = (u32)entry;
    sf->LR = (u32)ThreadExit;
    sf->R12 = 0;
    sf->HWSAVE[0] = arg;
    // Initialize context and state
    Threads[id].sp = (u32)sf;
    Threads[id].err_no = 0;
    Threads[id].id = id;
    Threads[id].pri = pri;
}

void SysTick_Handler(void) {
    if (RunningThreadID == NO_SUCH_THREAD) return;
    TickCount += TickInterval;
    MosYieldThread();
    EVENT(TICK, TickCount);
}

static bool CheckMux(Thread * thd) {
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
        if (*sem > 0) {
            ThreadData[thd->id].mux_idx = idx;
            return true;
        }
    }
    return false;
}

static Thread *
ProcessThread(Thread * thd, MosThreadPriority pri, bool first_scan) {
    switch (thd->state & 0xF) {
    case THREAD_RUNNABLE:
        return thd;
    case THREAD_WAIT_FOR_MUTEX: {
            // Check mutex (nested priority inheritance algorithm)
            MosThreadID owner = thd->wait.mtx->owner;
            if (thd->wait.mtx->owner == NO_SUCH_THREAD) {
                if (first_scan) thd->wait.mtx->to_yield = false;
                return thd;
            } else {
                if (Threads[owner].pri > pri) {
                    // Owner thread is lower priority
                    if (Threads[owner].state == THREAD_RUNNABLE) {
                        if (first_scan) thd->wait.mtx->to_yield = true;
                        return &Threads[owner];
                    } else return ProcessThread(&Threads[owner], pri, first_scan);
                } else if (!first_scan) return thd;
            }
        }
        break;
    case THREAD_WAIT_FOR_SEM:
        if (*thd->wait.sem != 0) return thd;
        break;
    case THREAD_WAIT_ON_MUX:
        if (CheckMux(thd)) return thd;
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
    if (RunningThreadID != NO_SUCH_THREAD) {
        Threads[RunningThreadID].sp = sp;
        Threads[RunningThreadID].err_no = *ErrNo;
    } else RunningThreadID = MOS_IDLE_THREAD_ID;
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
    if (Threads[RunningThreadID].state == THREAD_TIME_TO_DIE) {
        // Arrange death of running thread via kill handler
        if (MosIsOnList(&Threads[RunningThreadID].tmr_q))
            MosRemoveFromList(&Threads[RunningThreadID].tmr_q);
        InitThread(RunningThreadID, Threads[RunningThreadID].pri,
                   ThreadData[RunningThreadID].kill_handler,
                   ThreadData[RunningThreadID].kill_arg,
                   ThreadData[RunningThreadID].stack_addr,
                   ThreadData[RunningThreadID].stack_size);
        SetThreadState(RunningThreadID, THREAD_RUNNABLE);
    } else if (Threads[RunningThreadID].state & THREAD_WAIT_FOR_TICK) {
        // Update running thread timer state
        // Insertion sort in timer queue
        s32 rem_ticks = (s32)Threads[RunningThreadID].wake_tick - adj_tick_count;
        MosList * tmr;
        for (tmr = Timers.next; tmr != &Timers; tmr = tmr->next) {
            Thread * tmr_thd = container_of(tmr, Thread, tmr_q);
            s32 tmr_rem_ticks = (s32)tmr_thd->wake_tick - adj_tick_count;
            if (rem_ticks <= tmr_rem_ticks) break;
        }
        MosAddToListBefore(tmr, &Threads[RunningThreadID].tmr_q);
    }
    // Process timer queue
    MosList * tmr_save;
    for (MosList * tmr = Timers.next; tmr != &Timers; tmr = tmr_save) {
        tmr_save = tmr->next;
        Thread * thd = container_of(tmr, Thread, tmr_q);
        s32 rem_ticks = (s32)thd->wake_tick - adj_tick_count;
        if (rem_ticks <= 0) {
            // Signal timeout
            thd->wait.sem = NULL;
            ThreadData[thd->id].mux_idx = -1;
            SetThreadState(thd->id, THREAD_RUNNABLE);
            MosRemoveFromList(tmr);
        } else break;
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
            if (MosIsOnList(&run_thd->tmr_q))
                MosRemoveFromList(&run_thd->tmr_q);
        }
        SetThreadState(run_thd->id, THREAD_RUNNABLE);
        // Look for second potentially runnable thread
        if (ScanThreads(&pri, &thd_elm, false)) tick_en = true;
        // Move thread to end of priority queue (round-robin)
        if (!MosIsLastElement(&PriQueues[run_thd->pri], &run_thd->run_q))
            MosMoveToEndOfList(&PriQueues[run_thd->pri], &run_thd->run_q);
    } else run_thd = &Threads[MOS_IDLE_THREAD_ID];
    // Determine next timer interval
    //   If more than 1 active thread, enable tick to commutate threads
    //   If there is an active timer, delay tick to next expiration up to max
    u32 next_tick_interval = 1;
    if (tick_en == false) {
        if (!MosIsListEmpty(&Timers)) {
            Thread * thd = container_of(Timers.next, Thread, tmr_q);
            s32 tmr_ticks_rem = (s32)thd->wake_tick - adj_tick_count;
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
    RunningThreadID = run_thd->id;
    *ErrNo = Threads[RunningThreadID].err_no;
    EVENT(SCHEDULER_EXIT, 0);
    return (u32)Threads[RunningThreadID].sp;
}

void MOS_NAKED PendSV_Handler(void) {
    // M3 context switch.
    // TODO: M4F requires floating point register save/restore
    asm volatile (
        "mrs r0, psp\n\t"
        "stmdb r0!, {r4-r11}\n\t"
        "bl Scheduler\n\t"
        "mvn r14, #2\n\t"
        "ldmfd r0!, {r4-r11}\n\t"
        "msr psp, r0\n\t"
        "bx lr\n\t"
    );
}

// TODO: Limit stack dump to end of memory
static void MOS_USED FaultHandler(u32 fault_no, u32 * sp, bool in_isr) {
    char * fault_type[] = {
        "Hard", "MemManage", "Bus", "Usage"
    };
    if (PrintfHook) {
        (*PrintfHook)("*** %s Fault %s", fault_type[fault_no - 3],
                      in_isr ? "IN ISR " : "");
        if (RunningThreadID == NO_SUCH_THREAD) (*PrintfHook)("(Before MOS Run) ***\n");
        else {
            (*PrintfHook)("(ThreadID %u) ***\n", RunningThreadID);
            // Check for stack overflow
            for (u32 ix = 0; ix < MAX_THREADS; ix++) {
                if (Threads[ix].state != THREAD_UNINIT) {
                    if (*((u32 *)ThreadData[ix].stack_addr) != STACK_CANARY)
                        (*PrintfHook)("!!! ThreadID %u Stack overflow !!!\n", ix);
                }
            }
        }
        (*PrintfHook)(" HFSR: %08X  CFSR: %08X AFSR: %08X\n", SCB->HFSR,
                      SCB->CFSR, SCB->AFSR);
        (*PrintfHook)(" BFAR: %08X MMFAR: %08X\n", SCB->BFAR, SCB->MMFAR);
        (*PrintfHook)("Stack @%08X:\n", (u32) sp);
        for (u32 ix = 0; ix < 16; ix++) {
            (*PrintfHook)(" %08X", sp[ix]);
            if ((ix & 0x3) == 0x3) (*PrintfHook)("\n");
        }
    }
    if (RunningThreadID == NO_SUCH_THREAD || in_isr) {
        // Hang if fault occurred anywhere but in thread context
        while (1)
          ;
    } else {
        // Stop thread if fault occurred in thread context
        SetThreadState(RunningThreadID, THREAD_TIME_TO_DIE);
        MosYieldThread();
    }
}

void MOS_NAKED HardFault_Handler(void) {
    asm volatile (
        "mrs r0, psr\n\t"
        "and r0, #255\n\t"
        "tst lr, #4\n\t"
        "ite eq\n\t"
        "mrseq r1, msp\n\t"
        "mrsne r1, psp\n\t"
        "tst lr, #8\n\t"
        "ite eq\n\t"
        "moveq r2, #1\n\t"
        "movne r2, #0\n\t"
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
    Threads[RunningThreadID].wake_tick = adj_tick_cnt + ticks;
}

void MosDelayThread(u32 ticks) {
    SetTimeout(ticks);
    SetThreadState(RunningThreadID, THREAD_WAIT_FOR_TICK);
    MosYieldThread();
}

void MOS_NAKED MosDelayMicroSec(u32 usec) {
    asm volatile (
        "ldr r1, _CyclesPerMicroSec\n\t"
        "ldr r1, [r1]\n\t"
        "mul r0, r0, r1\n\t"
        "sub r0, #13\n\t"  // Overhead calibration
        "delay:\n\t"
        "subs r0, #3\n\t"  // Cycles per loop iteration
        "bgt delay\n\t"
        "bx lr\n\t"
        ".balign 4\n\t"
        "_CyclesPerMicroSec: .word CyclesPerMicroSec\n\t"
            : : : "r0", "r1"
    );
}

static s32 IdleThread(s32 arg) {
    while (1) {
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
    // Enable Bus, Memory and Usage Faults in general
    SCB->SHCSR |= (SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_MEMFAULTENA_Msk |
                   SCB_SHCSR_USGFAULTENA_Msk);
    // Trap Divide By 0 and "Unintentional" Unaligned Accesses
    SCB->CCR |= (SCB_CCR_DIV_0_TRP_Msk | SCB_CCR_UNALIGN_TRP_Msk);
    // Cache errno pointer for use during context switch
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
    MosInitAndRunThread(MOS_IDLE_THREAD_ID, MOS_MAX_THREAD_PRIORITIES,
                        IdleThread, 0, IdleStack, sizeof(IdleStack));
    // Fill out parameters
    Params.max_app_threads = MOS_MAX_APP_THREADS;
    Params.thread_pri_hi = 0;
    Params.thread_pri_low = MOS_MAX_THREAD_PRIORITIES - 1;
    Params.int_pri_hi = 0;
    Params.int_pri_low = pri_low;
    Params.micro_sec_per_tick = MOS_MICRO_SEC_PER_TICK;
}

void MosRegisterRawPrintfHook(MosRawPrintfHook * hook) { PrintfHook = hook; }
void MosRegisterSleepHook(MosSleepHook * hook) { SleepHook = hook; }
void MosRegisterWakeHook(MosWakeHook * hook) { WakeHook = hook; }
void MosRegisterEventHook(MosEventHook * hook) { EventHook = hook; }

void MosRunScheduler(void) {
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
    if (RunningThreadID == NO_SUCH_THREAD) return;
    // Invoke PendSV handler to potentially perform context switch
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    asm volatile ( "isb" );
}

MosThreadID MosGetThreadID(void) {
    return RunningThreadID;
}

MosThreadState MosGetThreadState(MosThreadID id, s32 * rtn_val) {
    MosThreadState state;
    switch (Threads[id].state) {
    case THREAD_UNINIT:
    case THREAD_INIT:
        state = MOS_THREAD_NOT_STARTED;
        break;
    case THREAD_STOPPED:
        state = MOS_THREAD_STOPPED;
        if (rtn_val != NULL) *rtn_val = ThreadData[id].rtn_val;
        break;
    default:
        if (ThreadData[id].stop_request) state = MOS_THREAD_STOP_REQUEST;
        else state = MOS_THREAD_RUNNING;
        break;
    }
    return state;
}

s32 MosInitThread(MosThreadID id, MosThreadPriority pri,
                  MosThreadEntry * entry, s32 arg,
                  u8 * s_addr, u32 s_size) {
    if (id == RunningThreadID) return -1;
    // Stop thread if running
    SetBasePri(IntPriMaskLow);
    ThreadState state = Threads[id].state;
    switch (state) {
    case THREAD_UNINIT:
    case THREAD_INIT:
    case THREAD_STOPPED:
        MosInitList(&Threads[id].run_q);
        MosInitList(&Threads[id].tmr_q);
        break;
    default:
        if (MosIsOnList(&Threads[id].tmr_q))
            MosRemoveFromList(&Threads[id].tmr_q);
        MosRemoveFromList(&Threads[id].run_q);
        break;
    }
    SetThreadState(id, THREAD_UNINIT);
    SetBasePri(0);
    InitThread(id, pri, entry, arg, s_addr, s_size);
    SetThreadState(id, THREAD_INIT);
    return id;
}

s32 MosRunThread(MosThreadID id) {
    if (Threads[id].state == THREAD_INIT) {
        SetBasePri(IntPriMaskLow);
        SetThreadState(id, THREAD_RUNNABLE);
        if (id != MOS_IDLE_THREAD_ID)
            MosAddToList(&PriQueues[Threads[id].pri], &Threads[id].run_q);
        SetBasePri(0);
        if (RunningThreadID > -1 &&
                Threads[id].pri < Threads[RunningThreadID].pri)
            MosYieldThread();
        return id;
    }
    return -1;
}

s32 MosInitAndRunThread(MosThreadID id,  MosThreadPriority pri,
                        MosThreadEntry * entry, s32 arg, u8 * s_addr,
                        u32 s_size) {
    MosInitThread(id, pri, entry, arg, s_addr, s_size);
    return MosRunThread(id);
}

void MosChangeThreadPriority(MosThreadID id, MosThreadPriority pri) {
    if (Threads[id].pri == pri) return;
    SetBasePri(IntPriMaskLow);
    Threads[id].pri = pri;
    MosRemoveFromList(&Threads[id].run_q);
    MosAddToList(&PriQueues[pri], &Threads[id].run_q);
    SetBasePri(0);
    if (RunningThreadID > -1 && pri < Threads[RunningThreadID].pri)
        MosYieldThread();
}

void MosRequestThreadStop(MosThreadID id) {
    ThreadData[id].stop_request = true;
}

bool MosIsStopRequested(void) {
    return (bool)ThreadData[RunningThreadID].stop_request;
}

s32 MosWaitForThreadStop(MosThreadID id) {
    Threads[RunningThreadID].wait.thd = &Threads[id];
    SetThreadState(RunningThreadID, THREAD_WAIT_FOR_STOP);
    MosYieldThread();
    return ThreadData[id].rtn_val;
}

bool MosWaitForThreadStopOrTO(MosThreadID id, s32 * rtn_val, u32 ticks) {
    SetTimeout(ticks);
    Threads[RunningThreadID].wait.thd = &Threads[id];
    SetThreadState(RunningThreadID, THREAD_WAIT_FOR_STOP_OR_TICK);
    MosYieldThread();
    if (Threads[RunningThreadID].wait.thd == NULL) return false;
    *rtn_val = ThreadData[id].rtn_val;
    return true;
}

void MosKillThread(MosThreadID id) {
    if (id == RunningThreadID) {
        SetThreadState(RunningThreadID, THREAD_TIME_TO_DIE);
        MosYieldThread();
        // not reachable
        while (1)
            ;
    }
    else MosInitAndRunThread(id, Threads[id].pri, ThreadData[id].kill_handler,
                             ThreadData[id].kill_arg, ThreadData[id].stack_addr,
                             ThreadData[id].stack_size);
}

void MosSetKillHandler(MosThreadID id, MosHandler * handler, s32 arg) {
    SetBasePri(IntPriMaskLow);
    if (handler) ThreadData[id].kill_handler = handler;
    else ThreadData[id].kill_handler = DefaultKillHandler;
    ThreadData[id].kill_arg = arg;
    SetBasePri(0);
}

void MosSetKillArg(MosThreadID id, s32 arg) {
    ThreadData[id].kill_arg = arg;
}

void MosInitList(MosList * list) {
    list->prev = list;
    list->next = list;
}

void MosAddToList(MosList * list, MosList * elm_add) {
    elm_add->prev = list->prev;
    elm_add->next = list;
    list->prev->next = elm_add;
    list->prev = elm_add;
}

void MosAddToListAfter(MosList * list, MosList * elm_add) {
    elm_add->prev = list;
    elm_add->next = list->next;
    list->next->prev = elm_add;
    list->next = elm_add;
}

void MosRemoveFromList(MosList * elm_rem) {
    elm_rem->next->prev = elm_rem->prev;
    elm_rem->prev->next = elm_rem->next;
    // For MosIsElementOnList() and safety
    elm_rem->prev = elm_rem;
    elm_rem->next = elm_rem;
}

void MosMoveToEndOfList(MosList * elm_exist, MosList * elm_move) {
    // Remove element
    elm_move->next->prev = elm_move->prev;
    elm_move->prev->next = elm_move->next;
    // Add to end of list
    elm_move->prev = elm_exist->prev;
    elm_move->next = elm_exist;
    elm_exist->prev->next = elm_move;
    elm_exist->prev = elm_move;
}

void MosInitMutex(MosMutex * mtx) {
    mtx->owner = NO_SUCH_THREAD;
    mtx->depth = 0;
    mtx->to_yield = false;
}

static void MOS_USED BlockOnMutex(MosMutex * mtx) {
    Threads[RunningThreadID].wait.mtx = mtx;
    SetThreadState(RunningThreadID, THREAD_WAIT_FOR_MUTEX);
    MosYieldThread();
}

void MOS_NAKED MosTakeMutex(MosMutex * mtx) {
    asm volatile (
        "ldr r1, _ThreadID\n\t"
        "ldr r1, [r1]\n\t"
        "RetryTM:\n\t"
        "ldrex r2, [r0]\n\t"
        "cmp r2, r1\n\t"
        "beq IncTM\n\t"
        "cmp r2, #-1\n\t"
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
        "_ThreadID: .word RunningThreadID\n\t"
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
        "cmp r2, #-1\n\t"
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
        "_ThreadID2: .word RunningThreadID\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MOS_NAKED MosGiveMutex(MosMutex * mtx) {
    asm volatile (
        "ldr r1, [r0, #4]\n\t"
        "sub r1, #1\n\t"
        "str r1, [r0, #4]\n\t"
        "cbnz r1, SkipGM\n\t"
        "mov r1, #-1\n\t"
        "RetryGM:\n\t"
        "ldrex r2, [r0]\n\t"     // Not sure this is needed
        "strex r2, r1, [r0]\n\t" //  ... just store -1 ?
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
    if (mtx->owner == RunningThreadID) {
        mtx->depth = 1;
        MosGiveMutex(mtx);
    }
}

bool MosIsMutexOwner(MosMutex * mtx) {
    return (mtx->owner == RunningThreadID);
}

void MosInitSem(MosSem * sem, u32 start_count) {
    *sem = start_count;
}

static void MOS_USED BlockOnSem(MosSem * sem) {
    Threads[RunningThreadID].wait.sem = sem;
    SetThreadState(RunningThreadID, THREAD_WAIT_FOR_SEM);
    MosYieldThread();
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
    Threads[RunningThreadID].wait.sem = sem;
    SetThreadState(RunningThreadID, THREAD_WAIT_FOR_SEM_OR_TICK);
    MosYieldThread();
    if (!Threads[RunningThreadID].wait.sem) return true;
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

void MOS_NAKED MosGiveSem(MosSem * sem) {
    asm volatile (
        "push { lr }\n\t"
        "RetryGS:\n\t"
        "ldrex r1, [r0]\n\t"
        "add r1, #1\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryGS\n\t"
        "cmp r1, #1\n\t"
        "dmb\n\t"
        "blt SkipGS\n\t"
        "bl MosYieldThread\n\t"
        "SkipGS:\n\t"
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
    Threads[RunningThreadID].mux = mux;
}

u32 MosWaitOnMux(MosMux * mux) {
    SetThreadState(RunningThreadID, THREAD_WAIT_ON_MUX);
    MosYieldThread();
    return (u32)ThreadData[RunningThreadID].mux_idx;
}

bool MosWaitOnMuxOrTO(MosMux * mux, u32 * idx, u32 ticks) {
    SetTimeout(ticks);
    SetThreadState(RunningThreadID, THREAD_WAIT_ON_MUX_OR_TICK);
    MosYieldThread();
    if (ThreadData[RunningThreadID].mux_idx < 0) return false;
    *idx = (u32)ThreadData[RunningThreadID].mux_idx;
    return true;
}

void MosAssertAt(char * file, u32 line) {
    if (PrintfHook) (*PrintfHook)("Assertion failed in %s on line %u\n", file, line);
    SetThreadState(RunningThreadID, THREAD_TIME_TO_DIE);
    MosYieldThread();
    // not reachable
    while (1)
        ;
}
