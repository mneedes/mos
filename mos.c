
//  Copyright 2019 Matthew Christopher Needes
//  Licensed under the terms and conditions contained within the LICENSE
//  file (the "License") included under this distribution.  You may not use
//  this file except in compliance with the License.

//
// MOS Microkernel
//

#include "mos_phal.h"
#include "mos.h"

#if (__FPU_USED == 1U)
// TODO: Can use this to enable CM3 vs CM4F
#endif

// TODO: Measure timer "jitter"
// TODO: Suppress yields on semaphores if no thread is waiting.
// TODO: Thread kill / Kill handler

#define MOS_IDLE_THREAD_ID      0
#define MOS_MAX_THREADS         (MOS_MAX_APP_THREADS + 1)
#define MOS_NO_THREADS          -1
#define MOS_MAX_S32             0x7FFFFFFF

#define MOS_SHIFT_PRI(pri)      ((pri) << (8 - __NVIC_PRIO_BITS))
#define MOS_LOW_INT_PRI         ((1 << __NVIC_PRIO_BITS) - 1)
#define MOS_LOW_INT_PRI_MASK    MOS_SHIFT_PRI(MOS_LOW_INT_PRI - 1)

typedef struct {
    u32 SWSAVE[8];   // R11-R4
    u32 HWSAVE[4];   // R3-R0
    u32 R12;         // R12
    u32 LR;          // R14
    u32 PC;          // R15
    u32 xPSR;
} StackFrame;

typedef enum {
    THREAD_UNINIT,
    THREAD_INIT,
    THREAD_RUNNABLE,
    THREAD_WAIT_FOR_TICK,    // Use a mask for WAIT_FOR_TICK ?
    THREAD_WAIT_FOR_MUTEX,
    THREAD_WAIT_FOR_SEM,
    THREAD_WAIT_FOR_SEM_OR_TICK, // ...
    THREAD_WAIT_ON_MUX,
    THREAD_WAIT_ON_MUX_OR_TICK, // ...
    THREAD_WAIT_FOR_STOP,
    THREAD_WAIT_FOR_STOP_OR_TICK, // ...
    //THREAD_KILL_PENDING,
    THREAD_STOPPED
} ThreadState;

typedef struct Thread {
    u32 sp;
    ThreadState state;
    MosList run_q;
    MosList tmr_q;
    MosList evt_q;
    MosThreadPriority pri;
    MosThreadID id;
    u32 wake_tick;
    MosMutex * wait_mutex;
    MosSem * wait_sem;
    MosWaitMux * wait_mux;
    struct Thread * wait_thd;
    s16 int_disable_cnt;
    u16 stop_request;
    s32 mux_idx;
    s32 rtn_val;
} Thread;

// Parameters
static MosParams Params;

// Threads and Events
static volatile MosThreadID RunningThreadID = MOS_NO_THREADS;
static Thread Threads[MOS_MAX_THREADS];
static MosList PriQueues[MOS_MAX_THREAD_PRIORITIES];
static MosList Events;

// Timers and Ticks
static MosList Timers;
static volatile u32 TickCount = MOS_START_TICK_COUNT;
static volatile u32 TickInterval = 1;
static u32 MaxTickInterval;
static u32 CyclesPerTick;
static u32 MOS_USED CyclesPerMicroSec;
static MosTimerHook * TimerHook = NULL;

// Idle thread stack and initial PSP safe storage
static u8 MOS_STACK_ALIGNED MosIdleStack[2 * sizeof(StackFrame)];

// Mask interrupts by priority, primarily for temporarily
//   disabling context switches.
static void MOS_INLINE MosSetBasePri(u32 pri) {
    asm volatile (
        "msr basepri, %0"
            : : "r" (pri) : "memory"
    );
}

void MosDisableInterrupts(void) {
    u32 irq = MosGetIRQNumber();
    if (!irq && ++Threads[RunningThreadID].int_disable_cnt > 1) return;
    asm volatile ( "cpsid if" );
}

void MosEnableInterrupts(void) {
    u32 irq = MosGetIRQNumber();
    if (!irq && --Threads[RunningThreadID].int_disable_cnt > 0) return;
    asm volatile ( "cpsie if" );
}

u32 MOS_NAKED MosGetIRQNumber(void) {
    asm volatile (
        "mrs r0, psr\n\t"
        "and r0, #255\n\t"
        "bx lr\n\t"
            : : : "r0"
    );
}

static MOS_INLINE void MosSetThreadState(MosThreadID id, ThreadState state) {
    asm volatile ( "dmb" );
    Threads[id].state = state;
}

void SysTick_Handler(void) {
    if (RunningThreadID == MOS_NO_THREADS) return;
    TickCount += TickInterval;
    MosYieldThread();
    if (TimerHook) (*TimerHook)(TickCount);
}

static bool CheckWaitMux(Thread *thd) {
    MosWaitMux *mux = thd->wait_mux;
    for (u32 idx = 0; idx < mux->num; idx++) {
        MosSem *sem;
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
            thd->mux_idx = idx;
            return true;
        }
    }
    return false;
}

static u32 MOS_USED Scheduler(u32 sp) {
    //#define GPIO_BASE  0x40020C00
    //MOS_VOL_U32(GPIO_BASE + 24) = 32768;   // 16384 is another one
    // Save SP
    if (RunningThreadID != MOS_NO_THREADS)
        Threads[RunningThreadID].sp = sp;
    s32 adj_tick_count = TickCount;
    if (TickInterval > 1) {
        // Adjust TickCount / Remaining ticks
        asm volatile ( "cpsid if" );
        u32 load = SysTick->LOAD;
        u32 val = SysTick->VAL;
        asm volatile ( "cpsie if" );
        adj_tick_count = TickCount + (load - val) / CyclesPerTick;
    }
    // Process event queue
    MosList *evt_save;
    for (MosList *evt = Events.next; evt != &Events; evt = evt_save) {
        Thread *thd = container_of(evt, Thread, evt_q);
        if (thd->state == THREAD_WAIT_FOR_TICK ||
            thd->state == THREAD_WAIT_FOR_SEM_OR_TICK ||
            thd->state == THREAD_WAIT_ON_MUX_OR_TICK ||
            thd->state == THREAD_WAIT_FOR_STOP_OR_TICK) {
            // Insertion sort in timer queue
            s32 rem_ticks = (s32)thd->wake_tick - adj_tick_count;
            MosList *tmr;
            for (tmr = Timers.next; tmr != &Timers; tmr = tmr->next) {
                Thread *tmr_thd = container_of(tmr, Thread, tmr_q);
                s32 tmr_rem_ticks = (s32)tmr_thd->wake_tick - adj_tick_count;
                if (rem_ticks <= tmr_rem_ticks) break;
            }
            MosAddToListBefore(tmr, &thd->tmr_q);
        }
        evt_save = evt->next;
        MosRemoveFromList(evt);
    }
    // Process timer queue
    MosList *tmr_save;
    s32 tmr_ticks_rem = MOS_MAX_S32;
    for (MosList *tmr = Timers.next; tmr != &Timers; tmr = tmr_save) {
        tmr_save = tmr->next;
        Thread *thd = container_of(tmr, Thread, tmr_q);
        s32 rem_ticks = (s32)thd->wake_tick - adj_tick_count;
        if (rem_ticks <= 0) {
            // Clear these to signal timeout
            thd->wait_sem = NULL;
            thd->mux_idx = -1;
            MosSetThreadState(thd->id, THREAD_RUNNABLE);
            MosRemoveFromList(tmr);
        } else {
            // List is sorted, so stop here
            tmr_ticks_rem = rem_ticks;
            break;
        }
    }
    u32 runnable_cnt = 0;
    // Process Priority Queues
    Thread *run_thd = &Threads[MOS_IDLE_THREAD_ID];
    for (MosThreadPriority pri = 0; pri < MOS_MAX_THREAD_PRIORITIES; pri++) {
        MosList *elm;
        for (elm = PriQueues[pri].next; elm != &PriQueues[pri]; elm = elm->next) {
            Thread *thd = container_of(elm, Thread, run_q);
            switch (thd->state) {
            case THREAD_WAIT_FOR_MUTEX: {
                    // Check mutex and perform priority inheritance if necessary
                    MosThreadID owner = thd->wait_mutex->owner;
                    if (thd->wait_mutex->owner == MOS_NO_THREADS) {
                        thd->wait_mutex->is_waiting = false;
                        MosSetThreadState(thd->id, THREAD_RUNNABLE);
                    }
                    else {
                        thd->wait_mutex->is_waiting = true;
                        // Unusual if thread owning mutex is not runnable
                        if (Threads[owner].state == THREAD_RUNNABLE)
                            thd = &Threads[owner];
                        else continue;
                    }
                }
                break;
            case THREAD_WAIT_FOR_SEM:
            case THREAD_WAIT_FOR_SEM_OR_TICK:
                if (*thd->wait_sem == 0) continue;
                else {
                    if (MosIsOnList(&thd->tmr_q))
                        MosRemoveFromList(&thd->tmr_q);
                    MosSetThreadState(thd->id, THREAD_RUNNABLE);
                }
                break;
            case THREAD_WAIT_ON_MUX:
            case THREAD_WAIT_ON_MUX_OR_TICK:
                if (CheckWaitMux(thd)) {
                    if (MosIsOnList(&thd->tmr_q))
                        MosRemoveFromList(&thd->tmr_q);
                    MosSetThreadState(thd->id, THREAD_RUNNABLE);
                } else continue;
                break;
            case THREAD_WAIT_FOR_STOP:
            case THREAD_WAIT_FOR_STOP_OR_TICK:
                if (thd->wait_thd->state != THREAD_STOPPED)
                    continue;
                else {
                    if (MosIsOnList(&thd->tmr_q))
                        MosRemoveFromList(&thd->tmr_q);
                    MosSetThreadState(thd->id, THREAD_RUNNABLE);
                }
                break;
            case THREAD_RUNNABLE:
                break;
            default:
                continue;
            }
            // Save first runnable thread, keep looking for possible second
            //   runnable at same priority level to help determine next
            //   tick interval.
            if (++runnable_cnt == 1) run_thd = thd;
            else break;
        }
        if (runnable_cnt > 0) {
            // Move thread to end of priority queue (round-robin)
            if (!MosIsLastElement(&PriQueues[run_thd->pri], &run_thd->run_q))
                MosMoveToEndOfList(&PriQueues[run_thd->pri], &run_thd->run_q);
            break;
        }
    }
    u32 next_tick_interval = 1;
    // Determine next timer interval
    //   If more than 1 active thread, enable tick to commutate threads
    //   If there is an active timer, delay tick to next expiration up to max
    if (runnable_cnt <= 1) {
        if (tmr_ticks_rem != MOS_MAX_S32) {
            // This ensures that the LOAD value will fit in SysTick counter...
            if (tmr_ticks_rem < MaxTickInterval)
                next_tick_interval = tmr_ticks_rem;
            else next_tick_interval = MaxTickInterval;
        } else {
            if (MOS_KEEP_TICKS_RUNNING == false) next_tick_interval = 0;
            else next_tick_interval = MaxTickInterval;
        }
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
    // Set next thread ID and return its stack pointer
    RunningThreadID = run_thd->id;
    //MOS_VOL_U32(GPIO_BASE + 24) = 32768 << 16;
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

u32 MosGetTickCount(void) {
    // Adjust tick count based on value in SysTick counter
    asm volatile ( "cpsid if" );
    u32 load = SysTick->LOAD;
    u32 val = SysTick->VAL;
    asm volatile ( "cpsie if" );
    u32 adj_tick_cnt = TickCount + (load - val) / CyclesPerTick;
    return adj_tick_cnt;
}

static void MOS_USED MosSetTimeout(u32 ticks) {
    // Adjust tick count based on value in SysTick counter
    asm volatile ( "cpsid if" );
    u32 load = SysTick->LOAD;
    u32 val = SysTick->VAL;
    asm volatile ( "cpsie if" );
    u32 adj_tick_cnt = TickCount + (load - val) / CyclesPerTick;
    MosSetBasePri(MOS_LOW_INT_PRI_MASK);
    Threads[RunningThreadID].wake_tick = adj_tick_cnt + ticks;
    if (!MosIsOnList(&Threads[RunningThreadID].evt_q))
        MosAddToList(&Events, &Threads[RunningThreadID].evt_q);
    MosSetBasePri(0);
}

void MosDelayThread(u32 ticks) {
    MosSetTimeout(ticks);
    MosSetThreadState(RunningThreadID, THREAD_WAIT_FOR_TICK);
    MosYieldThread();
}

void MOS_NAKED MosDelayMicroSec(u32 usec) {
    asm volatile (
        "ldr r1, _CyclesPerMicroSec\n\t"
        "ldr r1, [r1]\n\t"
        "mul r0, r0, r1\n\t"
        "sub r0, #13\n\t"
        "delay:\n\t"
        "subs r0, #3\n\t"
        "bgt delay\n\t"
        "bx lr\n\t"
        "_CyclesPerMicroSec:\n\t"
        ".word CyclesPerMicroSec\n\t"
            : : : "r0", "r1"
    );
}

void MosRegisterTimerHook(MosTimerHook *timer_hook) {
    TimerHook = timer_hook;
}

static s32 MosIdleThread(s32 arg) {
    while (1) {
        asm volatile (
            "dsb\n\t"
            "wfi\n\t"
        );
    }
    return 0;
}

// NOTE: SysTick should be set up by HAL before running.
void MosInit(void) {
    // Set up timers with tick-reduction
    CyclesPerTick = SysTick->LOAD + 1;
    MaxTickInterval = ((1 << 24) - 1) / CyclesPerTick;
    CyclesPerMicroSec = CyclesPerTick / MOS_MICRO_SEC_PER_TICK;
    // Set lowest priority for PendSV and Tick
    //  NOTE: The lower the number the higher the priority
    //  FIXME: This code assumes there are zero bits allocated to subgroups
    u32 pg = NVIC_GetPriorityGrouping();
    NVIC_SetPriority(PendSV_IRQn, NVIC_EncodePriority(pg, MOS_LOW_INT_PRI, 0));
    NVIC_SetPriority(SysTick_IRQn, NVIC_EncodePriority(pg, MOS_LOW_INT_PRI, 0));
    // Initialize empty priority, event and timer queues
    for (MosThreadPriority pri = 0; pri < MOS_MAX_THREAD_PRIORITIES; pri++)
        MosInitList(&PriQueues[pri]);
    MosInitList(&Events);
    MosInitList(&Timers);
    // Create idle thread
    MosInitAndRunThread(MOS_IDLE_THREAD_ID, MOS_MAX_THREAD_PRIORITIES,
                        MosIdleThread, 0, MosIdleStack, sizeof(MosIdleStack));
    // Fill out parameters
    Params.max_app_threads = MOS_MAX_APP_THREADS;
    Params.thread_pri_hi = 0;
    Params.thread_pri_low = MOS_MAX_THREAD_PRIORITIES - 1;
    Params.int_pri_hi = 0;
    Params.int_pri_low = MOS_LOW_INT_PRI;
    Params.micro_sec_per_tick = MOS_MICRO_SEC_PER_TICK;
}

void MosRunScheduler(void) {
    // Start PSP in a safe place for first PendSV and then enable interrupts
    asm volatile (
        "ldr r0, psp_start\n\t"
        "msr psp, r0\n\t"
        "mov r0, #0\n\t"
        "msr basepri, r0\n\t"
        "cpsie if\n\t"
        "psp_start: .word MosIdleStack + 64\n\t"
            : : : "r0"
    );
    // Invoke PendSV handler to potentially perform context switch
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    // Not reachable
    while (1)
        ;
}

const MosParams * MosGetParams(void) {
    return (const MosParams *)&Params;
}

void MosYieldThread(void) {
    if (RunningThreadID == MOS_NO_THREADS) return;
    // Invoke PendSV handler to potentially perform context switch
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
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
        if (rtn_val != NULL) *rtn_val = Threads[id].rtn_val;
        break;
    default:
        if (Threads[id].stop_request) state = MOS_THREAD_STOP_REQUEST;
        else state = MOS_THREAD_RUNNING;
        break;
    }
    return state;
}

static void MosThreadExit(s32 rtn_val) {
    MosSetBasePri(MOS_LOW_INT_PRI_MASK);
    Threads[RunningThreadID].rtn_val = rtn_val;
    MosSetThreadState(RunningThreadID, THREAD_STOPPED);
    MosRemoveFromList(&Threads[RunningThreadID].run_q);
    MosSetBasePri(0);
    MosYieldThread();
    // Not reachable
    while (1)
        ;
}

s32 MosInitThread(MosThreadID id, MosThreadPriority pri,
                  MosThreadEntry * entry, s32 arg,
                  u8 * s_addr, u32 s_size) {
    ThreadState state = Threads[id].state;
    if (state != THREAD_UNINIT && state != THREAD_STOPPED) return -1;
    StackFrame * sf = (StackFrame *) (s_addr + s_size);
    sf--;
    // Set Thumb Mode
    sf->xPSR = 0x01000000;
    sf->PC = (u32)entry;
    sf->LR = (u32)MosThreadExit;
    sf->R12 = 0;
    sf->HWSAVE[0] = arg;
    Threads[id].sp = (u32)sf;
    Threads[id].pri = pri;
    Threads[id].id = id;
    Threads[id].stop_request = false;
    Threads[id].int_disable_cnt = 0;
    MosInitList(&Threads[id].evt_q);
    MosInitList(&Threads[id].tmr_q);
    MosInitList(&Threads[id].run_q);
    MosSetThreadState(id, THREAD_INIT);
    return id;
}

s32 MosRunThread(MosThreadID id) {
    if (Threads[id].state == THREAD_INIT) {
        MosSetBasePri(MOS_LOW_INT_PRI_MASK);
        MosSetThreadState(id, THREAD_RUNNABLE);
        if (id != MOS_IDLE_THREAD_ID)
            MosAddToList(&PriQueues[Threads[id].pri], &Threads[id].run_q);
        MosSetBasePri(0);
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
    MosSetBasePri(MOS_LOW_INT_PRI_MASK);
    Threads[id].pri = pri;
    MosRemoveFromList(&Threads[id].run_q);
    MosAddToList(&PriQueues[pri], &Threads[id].run_q);
    MosSetBasePri(0);
    if (pri < Threads[RunningThreadID].pri) MosYieldThread();
}

void MosRequestThreadStop(MosThreadID id) {
    Threads[id].stop_request = true;
}

bool MosIsStopRequested(void) {
    return (bool)Threads[RunningThreadID].stop_request;
}

//void MosKillThread(MosThreadID id) {
//    MosSetThreadState(id, THREAD_KILL_PENDING);
//    // Set event for thread
//    if (RunningThreadID == id) MosYieldThread();
//}

s32 MosWaitForThreadStop(MosThreadID id) {
    Threads[RunningThreadID].wait_thd = &Threads[id];
    MosSetThreadState(RunningThreadID, THREAD_WAIT_FOR_STOP);
    MosYieldThread();
    return Threads[id].rtn_val;
}

bool MosWaitForThreadStopOrTO(MosThreadID id, s32 * rtn_val, u32 ticks) {
    MosSetTimeout(ticks);
    Threads[RunningThreadID].wait_thd = &Threads[id];
    MosSetThreadState(RunningThreadID, THREAD_WAIT_FOR_STOP_OR_TICK);
    MosYieldThread();
    return Threads[id].rtn_val;
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
    // For MosIsElementOnList()
    elm_rem->prev = elm_rem;
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
    mtx->owner = MOS_NO_THREADS;
    mtx->depth = 0;
    mtx->is_waiting = false;
}

static void MOS_USED MosBlockOnMutex(MosMutex * mtx) {
    Threads[RunningThreadID].wait_mutex = mtx;
    MosSetThreadState(RunningThreadID, THREAD_WAIT_FOR_MUTEX);
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
        "bl MosBlockOnMutex\n\t"
        "pop {r0, r1, lr}\n\t"
        "b RetryTM\n\t"
        "_ThreadID:\n\t"
        ".word RunningThreadID\n\t"
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
        "_ThreadID2:\n\t"
        ".word RunningThreadID\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MOS_NAKED MosGiveMutex(MosMutex * mtx) {
    asm volatile (
        "ldr r1, [r0, #4]\n\t"
        "sub r1, #1\n\t"
        "str r1, [r0, #4]\n\t"
        "cmp r1, #0\n\t"
        "bne SkipGM\n\t"
        "mov r1, #-1\n\t"
        "RetryGM:\n\t"
        "ldrex r2, [r0]\n\t"     // Not sure this is needed
        "strex r2, r1, [r0]\n\t" //  ... just store -1 ?
        "cmp r2, #0\n\t"
        "bne RetryGM\n\t"
        "ldr r1, [r0, #8]\n\t"
        "cmp r1, #0\n\t"
        "beq SkipGM\n\t"
        "push { lr }\n\t"
        "bl MosYieldThread\n\t"
        "pop { lr }\n\t"
        "SkipGM:\n\t"
        "bx lr\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

void MosInitSem(MosSem * sem, u32 start_count) {
    *sem = start_count;
}

static void MOS_USED MosBlockOnSem(MosSem * sem) {
    Threads[RunningThreadID].wait_sem = sem;
    MosSetThreadState(RunningThreadID, THREAD_WAIT_FOR_SEM);
    MosYieldThread();
}

void MOS_NAKED MosTakeSem(MosSem * sem) {
    asm volatile (
        "RetryTS:\n\t"
        "ldrex r1, [r0]\n\t"
        "cmp r1, #0\n\t"
        "beq BlockTS\n\t"
        "sub r1, #1\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryTS\n\t"
        "dmb\n\t"
        "bx lr\n\t"
        "BlockTS:\n\t"
        "push {r0, lr}\n\t"
        "bl MosBlockOnSem\n\t"
        "pop {r0, lr}\n\t"
        "b RetryTS\n\t"
            : : : "r0", "r1", "r2", "r3"
    );
}

static bool MOS_USED MosBlockOnSemOrTO(MosSem * sem) {
    Threads[RunningThreadID].wait_sem = sem;
    MosSetThreadState(RunningThreadID, THREAD_WAIT_FOR_SEM_OR_TICK);
    MosYieldThread();
    if (!Threads[RunningThreadID].wait_sem) return true;
    return false;
}

bool MOS_NAKED MosTakeSemOrTO(MosSem * sem, u32 ticks) {
    asm volatile (
        "push {r0, lr}\n\t"
        "mov r0, r1\n\t"
        "bl MosSetTimeout\n\t"
        "pop {r0, lr}\n\t"
        "RetryTSTO:\n\t"
        "ldrex r1, [r0]\n\t"
        "cmp r1, #0\n\t"
        "beq BlockTSTO\n\t"
        "sub r1, #1\n\t"
        "strex r2, r1, [r0]\n\t"
        "cmp r2, #0\n\t"
        "bne RetryTSTO\n\t"
        "dmb\n\t"
        "mov r0, #1\n\t"
        "bx lr\n\t"
        "BlockTSTO:\n\t"
        "push {r0, lr}\n\t"
        "bl MosBlockOnSemOrTO\n\t"
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
        "cmp r1, #0\n\t"
        "beq FailTRS\n\t"
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

// MosSendToQueue() is ISR safe, and therefore has a critical section that
// disables interrupts for ALL send to queue methods.  Receive methods
// are not ISR safe and can use PRIMASK since only context switch should
// be disabled.

bool MosSendToQueue(MosQueue * queue, u32 data) {
    // After taking semaphore context has a "license to write one entry,"
    // but it still should wait if another context is trying to do the same
    // thing in either thread or ISR context.
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
    MosSetBasePri(MOS_LOW_INT_PRI_MASK);
    u32 data = queue->buf[queue->head];
    if (++queue->head >= queue->len) queue->head = 0;
    MosSetBasePri(0);
    MosGiveSem(&queue->sem_tail);
    return data;
}

bool MosTryReceiveFromQueue(MosQueue * queue, u32 * data) {
    if (MosTrySem(&queue->sem_head)) {
        MosSetBasePri(MOS_LOW_INT_PRI_MASK);
        *data = queue->buf[queue->head];
        if (++queue->head >= queue->len) queue->head = 0;
        MosSetBasePri(0);
        MosGiveSem(&queue->sem_tail);
        return true;
    }
    return false;
}

bool MosReceiveFromQueueOrTO(MosQueue * queue, u32 * data, u32 ticks) {
    if (MosTakeSemOrTO(&queue->sem_head, ticks)) {
        MosSetBasePri(MOS_LOW_INT_PRI_MASK);
        *data = queue->buf[queue->head];
        if (++queue->head >= queue->len) queue->head = 0;
        MosSetBasePri(0);
        MosGiveSem(&queue->sem_tail);
        return true;
    }
    return false;
}

void MosInitWaitMux(MosWaitMux *mux) {
    mux->num = 0;
}

void MosSetActiveMux(MosWaitMux * mux, MosWaitMuxEntry * entries, u32 len) {
    mux->entries = entries;
    mux->num = len;
    Threads[RunningThreadID].wait_mux = mux;
}

u32 MosWaitOnMux(MosWaitMux * mux) {
    MosSetThreadState(RunningThreadID, THREAD_WAIT_ON_MUX);
    MosYieldThread();
    return (u32)Threads[RunningThreadID].mux_idx;
}

bool MosWaitOnMuxOrTO(MosWaitMux * mux, u32 * idx, u32 ticks) {
    MosSetTimeout(ticks);
    MosSetThreadState(RunningThreadID, THREAD_WAIT_ON_MUX_OR_TICK);
    MosYieldThread();
    if (Threads[RunningThreadID].mux_idx < 0) return false;
    *idx = (u32)Threads[RunningThreadID].mux_idx;
    return true;
}
