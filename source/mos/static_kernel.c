
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Static Microkernel
//

#include <bsp_hal.h>
#include <mos/static_kernel.h>
#include <mos/internal/arch.h>
#include <mos/internal/security.h>
#include <errno.h>

// TODO: Consolidate "TO" Timeout APIs to single API function.
// TODO: auto tick startup?
// TODO: smaller init for term handlers.

#define NO_SUCH_THREAD       NULL
#define STACK_FILL_VALUE     0xca110411

/* The parameter is really used, but tell compiler it is unused to reject warnings */
#define MOS_USED_PARAM(x)    MOS_UNUSED(x)

#define EVENT(e, v) \
    { if (MOS_ENABLE_EVENTS) (*pEventHook)((MOS_EVENT_ ## e), (v)); }

// Element types for polymorphic lists
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
    u32                 mtxCnt;
    ThreadState         state;
    error_t             errNo;
    MosLink             runLink;
    MosPmLink           tmrLink;
    MosList             stopQ;
    u32                 wakeTick;
    void              * pBlockedOn;
    MosThreadPriority   pri;
    MosThreadPriority   nomPri;
    u8                  timedOut;
    u8                  pad;
    s32                 rtnVal;
    MosThreadEntry    * pTermHandler;
    s32                 termArg;
    u8                * pStackBottom;
    u32                 stackSize;
    const char        * pName;
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
    s8                  secureContext;
    s8                  secureContextNew;
    u16                 pad2;
#endif
    void              * pUser;
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

// Hooks
static MosRawVPrintfHook * VPrintfHook = NULL;
static MosSleepHook * pSleepHook = NULL;
static MosWakeHook * pWakeHook = NULL;
static void DummyEventHook(MosEvent e, u32 v) { MOS_UNUSED(e); MOS_UNUSED(v); }
static MosEventHook * pEventHook = DummyEventHook;

// Threads and Events
static Thread * pRunningThread = NO_SUCH_THREAD;
static error_t * pErrNo;
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

// Interrupt low priority mask
static u8 IntPriMaskLow;
static u8 IntPriLow;

// Idle thread stack and initial dummy PSP stack frame storage
//  28 words is enough for a FP stack frame,
//     add a stack frame for idle thread stack initialization.
static u8 MOS_STACK_ALIGNED IdleStack[112 + sizeof(StackFrame)];

// Print buffer
static char (*RawPrintBuffer)[MOS_PRINT_BUFFER_SIZE] = NULL;

void mosRegisterRawVPrintfHook(MosRawVPrintfHook * hook, char (*buffer)[MOS_PRINT_BUFFER_SIZE]) {
    VPrintfHook = hook;
    RawPrintBuffer = buffer;
}
void mosRegisterSleepHook(MosSleepHook * pHook) { pSleepHook = pHook; }
void mosRegisterWakeHook(MosWakeHook * pHook) { pWakeHook = pHook; }
void mosRegisterEventHook(MosEventHook * pHook) { pEventHook = pHook; }

static MOS_INLINE void SetThreadState(Thread * pThd, ThreadState state) {
    asm volatile ( "dmb" );
    pThd->state = state;
}

MOS_ISR_SAFE static MOS_INLINE void YieldThread(void) {
    // Invoke PendSV handler to potentially perform context switch
    MOS_REG(ICSR) = MOS_REG_VALUE(ICSR_PENDSV);
    asm volatile ( "dsb" );
}

static MOS_INLINE void SetRunningThreadStateAndYield(ThreadState state) {
    asm volatile ( "dmb" );
    LockScheduler(IntPriMaskLow);
    pRunningThread->state = state;
    YieldThread();
    UnlockScheduler();
}

static void KPrintf(const char * pFmt, ...) {
    if (VPrintfHook) {
        va_list args;
        va_start(args, pFmt);
        (*VPrintfHook)(pFmt, args);
        va_end(args);
    }
}

#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
static void KPrint(void) {
    KPrintf(*RawPrintBuffer);
}
#endif

void mosAssertAt(char * pFile, u32 line) {
    KPrintf("Assertion failed in %s on line %u\n", pFile, line);
    MOS_INDUCE_CRASH();
    if (pRunningThread != NO_SUCH_THREAD)
        SetRunningThreadStateAndYield(THREAD_TIME_TO_STOP);
    // not always reachable
    while (1);
}

//
// Time / Timers
//

u32 mosGetTickCount(void) {
    return Tick.lower;
}

MOS_ISR_SAFE u64 mosGetCycleCount(void) {
    u32 mask = mosDisableInterrupts();
    s64 tmp = Tick.count;
    u32 val = MOS_REG(TICK_VAL);
    if (MOS_REG(TICK_CTRL) & MOS_REG_VALUE(TICK_FLAG)) {
        tmp = ++Tick.count;
        val = MOS_REG(TICK_VAL);
    }
    mosEnableInterrupts(mask);
    return (tmp * CyclesPerTick) - val;
}

MOS_ISR_SAFE void mosAdvanceTickCount(u32 ticks) {
    if (ticks) {
        u32 mask = mosDisableInterrupts();
        Tick.count += ticks;
        MOS_REG(ICSR) = MOS_REG_VALUE(ICSR_PENDST);
        mosEnableInterrupts(mask);
    }
}

MOS_ISR_SAFE static void MOS_USED SetTimeout(u32 ticks) {
    pRunningThread->wakeTick = Tick.lower + ticks;
}

MOS_ISR_SAFE void MOS_NAKED mosDelayMicroseconds(u32 usec) {
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

void mosInitTimer(MosTimer * pTmr, MosTimerCallback * pCallback) {
    mosInitPmLink(&pTmr->tmrLink, ELM_TIMER);
    pTmr->pCallback = pCallback;
}

static void AddTimer(MosTimer * pTmr) {
    // NOTE: Must lock scheduler before calling
    MosLink * pElm;
    u32 tickCount = mosGetTickCount();
    pTmr->wakeTick = tickCount + pTmr->ticks;
    for (pElm = TimerQueue.pNext; pElm != &TimerQueue; pElm = pElm->pNext) {
        if (((MosPmLink *)pElm)->type == ELM_THREAD) {
            Thread * pThd = container_of(pElm, Thread, tmrLink);
            s32 tmrRemTicks = (s32)pThd->wakeTick - tickCount;
            if ((s32)pTmr->ticks <= tmrRemTicks) break;
        } else {
            MosTimer * pTmrTmr = container_of(pElm, MosTimer, tmrLink);
            s32 tmrRemTicks = (s32)pTmrTmr->wakeTick - tickCount;
            if ((s32)pTmr->ticks <= tmrRemTicks) break;
        }
    }
    mosAddToListBefore(pElm, &pTmr->tmrLink.link);
}

void mosSetTimer(MosTimer * pTmr, u32 ticks, void * pUser) {
    LockScheduler(IntPriMaskLow);
    pTmr->ticks = ticks;
    pTmr->pUser = pUser;
    AddTimer(pTmr);
    UnlockScheduler();
}

void mosCancelTimer(MosTimer * pTmr) {
    LockScheduler(IntPriMaskLow);
    if (mosIsOnList(&pTmr->tmrLink.link))
        mosRemoveFromList(&pTmr->tmrLink.link);
    UnlockScheduler();
}

void mosResetTimer(MosTimer * pTmr) {
    LockScheduler(IntPriMaskLow);
    if (mosIsOnList(&pTmr->tmrLink.link))
        mosRemoveFromList(&pTmr->tmrLink.link);
    AddTimer(pTmr);
    UnlockScheduler();
}

//
// Threads
//

void mosDelayThread(u32 ticks) {
    if (ticks) {
        SetTimeout(ticks);
        SetRunningThreadStateAndYield(THREAD_WAIT_FOR_TICK);
    } else YieldThread();
}

// ThreadExit is invoked when a thread stops (returns from its natural entry point)
//   or after its termination handler returns (kill or exception)
static s32 ThreadExit(s32 rtnVal) {
    LockScheduler(IntPriMaskLow);
    pRunningThread->rtnVal = rtnVal;
    SetThreadState(pRunningThread, THREAD_STOPPED);
    asm volatile ( "dmb" );
    if (mosIsOnList(&pRunningThread->tmrLink.link))
        mosRemoveFromList(&pRunningThread->tmrLink.link);
    MosLink * pElmSave;
    for (MosLink * pElm = pRunningThread->stopQ.pNext;
            pElm != &pRunningThread->stopQ; pElm = pElmSave) {
        pElmSave = pElm->pNext;
        Thread * thd = container_of(pElm, Thread, runLink);
        mosRemoveFromList(pElm);
        mosAddToEndOfList(&RunQueues[thd->pri], &thd->runLink);
        if (mosIsOnList(&thd->tmrLink.link))
            mosRemoveFromList(&thd->tmrLink.link);
        SetThreadState(thd, THREAD_RUNNABLE);
    }
    mosRemoveFromList(&pRunningThread->runLink);
    YieldThread();
    UnlockScheduler();
    // Not reachable
    mosAssert(0);
    return 0;
}

MOS_ISR_SAFE static void
InitThread(Thread * pThd, MosThreadPriority pri, MosThreadEntry * pEntry, s32 arg,
               u8 * pStackBottom, u32 stackSize) {
    u8 * pSP = pStackBottom;
    // Ensure 8-byte alignment for ARM / varargs compatibility.
    pSP = (u8 *) ((u32)(pSP + stackSize - sizeof(u32)) & 0xfffffff8);
    // Place canary value at top and fill out initial frame
    *((u32 *)pSP) = STACK_FILL_VALUE;
    StackFrame * pSF = (StackFrame *)pSP - 1;
    pSF->PSR = 0x01000000;
    pSF->PC = (u32)pEntry;
    pSF->LR = (u32)ThreadExit;
    pSF->R12 = 0;
    pSF->HWSAVE[0] = arg;
    pSF->LR_EXC_RTN = ExcReturnInitial;
    // Either fill lower stack OR just place canary value at bottom
    u32 * pFill = (u32 *)pSF - 1;
    for (; pFill >= (u32 *)pStackBottom; pFill--) {
        *pFill = STACK_FILL_VALUE;
    }
    // Initialize context and state
    pThd->sp = (u32)pSF;
    pThd->mtxCnt = 0;
    pThd->errNo = 0;
    pThd->pri = pri;
    pThd->nomPri = pri;
    pThd->pTermHandler = ThreadExit;
    pThd->termArg = 0;
    pThd->pStackBottom = pStackBottom;
    pThd->stackSize = stackSize;
    pThd->pName = "";
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
    pThd->secureContext    = MOS_DEFAULT_SECURE_CONTEXT;
    pThd->secureContextNew = MOS_DEFAULT_SECURE_CONTEXT;
#endif
    pThd->pUser = NULL;
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
        s32 tickInterval = MaxTickInterval;
        if (!mosIsListEmpty(&TimerQueue)) {
            if (((MosPmLink *)TimerQueue.pNext)->type == ELM_THREAD) {
                Thread * thd = container_of(TimerQueue.pNext, Thread, tmrLink);
                tickInterval = (s32)thd->wakeTick - Tick.lower;
            } else {
                MosTimer * tmr = container_of(TimerQueue.pNext, MosTimer, tmrLink);
                tickInterval = (s32)tmr->wakeTick - Tick.lower;
            }
            if (tickInterval <= 0) {
                tickInterval = 1;
            } else if (tickInterval > MaxTickInterval) {
                tickInterval = MaxTickInterval;
            }
        }
        u32 load = 0;
        if (tickInterval > 1) {
            load = (tickInterval - 1) * CyclesPerTick + MOS_REG(TICK_VAL);
            MOS_REG(TICK_LOAD) = load;
            MOS_REG(TICK_VAL) = 0;
        }
        if (pSleepHook) (*pSleepHook)();
        MOS_REG(TICK_CTRL) = MOS_REG_VALUE(TICK_ENABLE);
        asm volatile (
            "dsb\n"
            "wfi" ::: "memory"
        );
        if (pWakeHook) (*pWakeHook)();
        if (load) {
            MOS_REG(TICK_CTRL) = MOS_REG_VALUE(TICK_DISABLE);
            if (MOS_REG(TICK_CTRL) & MOS_REG_VALUE(TICK_FLAG)) {
                MOS_REG(TICK_LOAD) = CyclesPerTick - (load - MOS_REG(TICK_VAL)) - 1;
            } else {
                u32 elapsed_cycles = tickInterval * CyclesPerTick - MOS_REG(TICK_VAL);
                tickInterval = elapsed_cycles / CyclesPerTick;
                MOS_REG(TICK_LOAD) = (tickInterval + 1) * CyclesPerTick - elapsed_cycles;
            }
            // TODO: What happens when LOAD is very small ? (Race condition between load and re-load)
            //   Perhaps need to manually launch SysTick and not reset VAL.
            //   Maybe better way to disable tick
            MOS_REG(TICK_VAL)  = 0;
            MOS_REG(TICK_CTRL) = MOS_REG_VALUE(TICK_ENABLE);
            MOS_REG(TICK_LOAD) = CyclesPerTick - 1;
            Tick.count += tickInterval;
        }
        asm volatile ( "dsb\n"
                       "cpsie i\n"
                       "isb" ::: "memory" );
    }
    return 0;
}

MosThread * mosGetRunningThread(void) {
    return (MosThread *)pRunningThread;
}

void mosGetStackStats(MosThread * _pThd, u32 * pStackSize, u32 * pStackUsage, u32 * pMaxStackUsage) {
    Thread * pThd = (Thread *)_pThd;
    LockScheduler(IntPriMaskLow);
    // Detect uninitialized thread state
    u32 state = pThd->state;
    if (state == THREAD_UNINIT || (state & THREAD_STATE_BASE_MASK) != THREAD_STATE_BASE) return;
    *pStackSize = pThd->stackSize;
    u8 * pStackTop = pThd->pStackBottom + *pStackSize;
    if (pThd == pRunningThread)
        *pStackUsage = mosGetStackDepth(pStackTop);
    else
        *pStackUsage = pStackTop - (u8 *)pThd->sp;
    u32 * pCheck = (u32 *)pThd->pStackBottom;
    while (*pCheck++ == STACK_FILL_VALUE);
    *pMaxStackUsage = pStackTop - (u8 *)pCheck + 4;
    UnlockScheduler();
}

u8 * mosGetStackBottom(MosThread * _pThd) {
    Thread * pThd = (Thread *)_pThd;
    u8 * pStackBottom = NULL;
    LockScheduler(IntPriMaskLow);
    if (pThd) pStackBottom = pThd->pStackBottom;
    else if (pRunningThread) pStackBottom = pRunningThread->pStackBottom;
    UnlockScheduler();
    return pStackBottom;
}

u32 mosGetStackSize(MosThread * _pThd) {
    Thread * pThd = (Thread *)_pThd;
    return pThd->stackSize;
}

void mosSetStack(MosThread * _pThd, u8 * pStackBottom, u32 stackSize) {
    Thread * pThd = (Thread *)_pThd;
    pThd->pStackBottom = pStackBottom;
    pThd->stackSize = stackSize;
}

void mosSetThreadName(MosThread * _pThd, const char * pName) {
    Thread * pThd = (Thread *)_pThd;
    pThd->pName = pName;
}

bool mosInitThread(MosThread * _pThd, MosThreadPriority pri,
                   MosThreadEntry * pEntry, s32 arg,
                   u8 * pStackBottom, u32 pStackSize) {
    Thread * pThd = (Thread *)_pThd;
    if (pThd == pRunningThread) return false;
    LockScheduler(IntPriMaskLow);
    // Detect uninitialized thread state variable
    if ((pThd->state & THREAD_STATE_BASE_MASK) != THREAD_STATE_BASE)
        pThd->state = THREAD_UNINIT;
    // Check thread state
    switch (pThd->state) {
    case THREAD_UNINIT:
    case THREAD_INIT:
        mosInitList(&pThd->stopQ);
        // fall through
    case THREAD_STOPPED:
        mosInitList(&pThd->runLink);
        mosInitPmLink(&pThd->tmrLink, ELM_THREAD);
        break;
    default:
        // Forcibly stop thread if running
        // This will run if thread is killed, stop queue
        //   is processed only after kill handler returns.
        if (mosIsOnList(&pThd->tmrLink.link))
            mosRemoveFromList(&pThd->tmrLink.link);
        // Lock because thread might be on semaphore pend queue
        _mosDisableInterrupts();
        mosRemoveFromList(&pThd->runLink);
        _mosEnableInterrupts();
        break;
    }
    SetThreadState(pThd, THREAD_UNINIT);
    UnlockScheduler();
    InitThread(pThd, pri, pEntry, arg, pStackBottom, pStackSize);
    SetThreadState(pThd, THREAD_INIT);
    return true;
}

bool mosRunThread(MosThread * _pThd) {
    Thread * pThd = (Thread *)_pThd;
    if (pThd->state == THREAD_INIT) {
        LockScheduler(IntPriMaskLow);
        SetThreadState(pThd, THREAD_RUNNABLE);
        if (pThd != &IdleThread)
            mosAddToEndOfList(&RunQueues[pThd->pri], &pThd->runLink);
        if (pRunningThread != NO_SUCH_THREAD && pThd->pri < pRunningThread->pri)
            YieldThread();
        UnlockScheduler();
        return true;
    }
    return false;
}

bool mosInitAndRunThread(MosThread * _pThd,  MosThreadPriority pri,
                         MosThreadEntry * pEntry, s32 arg, u8 * pStackBottom,
                         u32 pStackSize) {
    if (!mosInitThread(_pThd, pri, pEntry, arg, pStackBottom, pStackSize))
        return false;
    return mosRunThread(_pThd);
}

MosThreadState mosGetThreadState(MosThread * _pThd, s32 * rtnVal) {
    Thread * pThd = (Thread *)_pThd;
    MosThreadState state;
    LockScheduler(IntPriMaskLow);
    switch (pThd->state) {
    case THREAD_UNINIT:
    case THREAD_INIT:
        state = MOS_THREAD_NOT_STARTED;
        break;
    case THREAD_STOPPED:
        state = MOS_THREAD_STOPPED;
        if (rtnVal != NULL) *rtnVal = pThd->rtnVal;
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

MosThreadPriority mosGetThreadPriority(MosThread * _pThd) {
    Thread * pThd = (Thread *)_pThd;
    return pThd->pri;
}

// Sort thread into pend queue by priority
MOS_ISR_SAFE static void SortThreadByPriority(Thread * pThd, MosList * pPendQ) {
    mosRemoveFromList(&pThd->runLink);
    MosLink * pElm = pPendQ->pNext;
    for (; pElm != pPendQ; pElm = pElm->pNext) {
        Thread * _pThd = container_of(pElm, Thread, runLink);
        if (_pThd->pri > pThd->pri) break;
    }
    mosAddToListBefore(pElm, &pThd->runLink);
}

void mosChangeThreadPriority(MosThread * _pThd, MosThreadPriority newPri) {
    Thread * pThd = (Thread *)_pThd;
    LockScheduler(IntPriMaskLow);
    // Snapshot the running thread priority (in case it gets changed)
    MosThreadPriority currPri = 0;
    if (pRunningThread != NO_SUCH_THREAD) currPri = pRunningThread->pri;
    // Change current priority if priority inheritance isn't active
    //  -OR- if new priority is higher than priority inheritance priority
    if (pThd->pri == pThd->nomPri || newPri < pThd->pri) {
        pThd->pri = newPri;
        switch (pThd->state) {
        case THREAD_RUNNABLE:
            mosRemoveFromList(&pThd->runLink);
            mosAddToEndOfList(&RunQueues[newPri], &pThd->runLink);
            break;
        case THREAD_WAIT_FOR_MUTEX:
            SortThreadByPriority(pThd, &((MosMutex *)pThd->pBlockedOn)->pendQ);
            break;
        case THREAD_WAIT_FOR_SEM:
        case THREAD_WAIT_FOR_SEM_OR_TICK:
            SortThreadByPriority(pThd, &((MosSem *)pThd->pBlockedOn)->pendQ);
            break;
        default:
            break;
        }
    }
    // Always change nominal priority
    pThd->nomPri = newPri;
    // Yield if priority is lowered on currently running thread
    //  -OR- if other thread has a greater priority than running thread
    if (pThd == pRunningThread) {
        if (pThd->pri > currPri) YieldThread();
    } else if (pThd->pri < currPri) YieldThread();
    UnlockScheduler();
}

s32 mosWaitForThreadStop(MosThread * _pThd) {
    Thread * pThd = (Thread *)_pThd;
    LockScheduler(IntPriMaskLow);
    if (pThd->state > THREAD_STOPPED) {
        mosRemoveFromList(&pRunningThread->runLink);
        mosAddToEndOfList(&pThd->stopQ, &pRunningThread->runLink);
        SetThreadState(pRunningThread, THREAD_WAIT_FOR_STOP);
        YieldThread();
    }
    UnlockScheduler();
    return pThd->rtnVal;
}

bool mosWaitForThreadStopOrTO(MosThread * _pThd, s32 * pRtnVal, u32 ticks) {
    Thread * pThd = (Thread *)_pThd;
    SetTimeout(ticks);
    pRunningThread->timedOut = 0;
    LockScheduler(IntPriMaskLow);
    if (pThd->state > THREAD_STOPPED) {
        mosRemoveFromList(&pRunningThread->runLink);
        mosAddToEndOfList(&pThd->stopQ, &pRunningThread->runLink);
        SetThreadState(pRunningThread, THREAD_WAIT_FOR_STOP_OR_TICK);
        YieldThread();
    }
    UnlockScheduler();
    if (pRunningThread->timedOut) return false;
    *pRtnVal = pThd->rtnVal;
    return true;
}

void mosKillThread(MosThread * _pThd) {
    Thread * pThd = (Thread *)_pThd;
    if (pThd == pRunningThread) {
        SetRunningThreadStateAndYield(THREAD_TIME_TO_STOP);
        // Not reachable
        mosAssert(0);
    } else {
        // Snapshot the arguments, run thread stop handler, allowing thread
        //   to stop at its original run priority.
        LockScheduler(IntPriMaskLow);
        MosThreadPriority pri = pThd->pri;
        MosThreadEntry * pStopHandler = pThd->pTermHandler;
        s32 stopArg = pThd->termArg;
        u8 * pStackBottom = pThd->pStackBottom;
        u32 pStackSize = pThd->stackSize;
        UnlockScheduler();
        mosInitAndRunThread((MosThread *)pThd, pri, pStopHandler, stopArg,
                            pStackBottom, pStackSize);
    }
}

void mosSetTermHandler(MosThread * _pThd, MosThreadEntry * pEntry, s32 arg) {
    Thread * pThd = (Thread *)_pThd;
    LockScheduler(IntPriMaskLow);
    if (pEntry) pThd->pTermHandler = pEntry;
    else pThd->pTermHandler = ThreadExit;
    pThd->termArg = arg;
    UnlockScheduler();
}

void mosSetTermArg(MosThread * _pThd, s32 arg) {
    Thread * pThd = (Thread *)_pThd;
    pThd->termArg = arg;
}

//
// Initialization
//

void mosInit(void) {
    // Save errno pointer for use during context switch
    pErrNo = __errno();
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
    u8 priMask = MOS_REG(SHPR)(MOS_PENDSV_IRQ - 4);
    u8 nvicPriBits = 8 - __builtin_ctz(priMask);
    // If priority groups are enabled SysTick will be set to the
    //   2nd lowest priority group, and PendSV the lowest.
    u32 priBits = 7 - MOS_GET_PRI_GROUP_NUM;
    if (priBits > nvicPriBits) priBits = nvicPriBits;
    IntPriLow = (1 << priBits) - 1;
    u8 priSystick = priMask;
    // If there are sub-priority bits give SysTick higher sub-priority (one lower number).
    if (priBits < nvicPriBits) {
        // Clear lowest set bit in mask
        priSystick = priMask - (1 << (8 - nvicPriBits));
    }
    MOS_REG(SHPR)(MOS_SYSTICK_IRQ  - 4) = priSystick;
    IntPriMaskLow = priSystick;
#elif (MOS_ARCH_CAT == MOS_ARCH_ARM_CORTEX_M_BASE)
    // NOTE: BASEPRI isn't implemented on baseline architectures, hence IntPriMaskLow is not used
    // Set lowest preemption priority for SysTick and PendSV
    MOS_REG(SHPR3) = MOS_REG_VALUE(EXC_PRIORITY);
    // Only two implemented priority bits on baseline
    IntPriLow = 3;
#endif
#if (MOS_ARM_AUTODETECT_EXC_RETURN == true)
    // Detect security mode and set Exception Return accordingly
    if (MOS_REG(CPUID_NS) == 0) ExcReturnInitial = MOS_EXC_RETURN_UNSECURE;
#endif
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
    mosInitSem(&SecureContextCounter, MOS_NUM_SECURE_CONTEXTS);
#endif
    // Initialize empty queues
    for (MosThreadPriority pri = 0; pri < MOS_MAX_THREAD_PRIORITIES; pri++)
        mosInitList(&RunQueues[pri]);
    mosInitList(&ISREventQueue);
    mosInitList(&TimerQueue);
    // Create idle thread
    mosInitAndRunThread((MosThread *) &IdleThread, MOS_MAX_THREAD_PRIORITIES,
                        IdleThreadEntry, 0, IdleStack, sizeof(IdleStack));
}

//
// Scheduler
//

void mosRunScheduler(void) {
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
    _mosEnableInterruptsWithBarrier();
    // Not reachable
    mosAssert(0);
}

void SysTick_Handler(void) {
    _mosDisableInterrupts();
    if (MOS_REG(TICK_CTRL) & MOS_REG_VALUE(TICK_FLAG)) Tick.count += 1;
    _mosEnableInterrupts();
    if (pRunningThread == NO_SUCH_THREAD) return;
    // Process timer queue
    //  Timer queues can contain threads or message timers
    MosLink * pElmSave;
    for (MosLink * pElm = TimerQueue.pNext; pElm != &TimerQueue; pElm = pElmSave) {
        pElmSave = pElm->pNext;
        if (((MosPmLink *)pElm)->type == ELM_THREAD) {
            Thread * pThd = container_of(pElm, Thread, tmrLink);
            s32 remTicks = (s32)pThd->wakeTick - Tick.lower;
            if (remTicks <= 0) {
                mosRemoveFromList(pElm);
                if (pThd->state == THREAD_WAIT_FOR_SEM_OR_TICK) {
                    _mosDisableInterrupts();
                    if (mosIsOnList(&((MosSem *)pThd->pBlockedOn)->evtLink)) {
                        // Event occurred before timeout, just let it be processed
                        _mosEnableInterrupts();
                        continue;
                    } else {
                        mosRemoveFromList(&pThd->runLink);
                        _mosEnableInterrupts();
                    }
                } else mosRemoveFromList(&pThd->runLink);
                mosAddToEndOfList(&RunQueues[pThd->pri], &pThd->runLink);
                pThd->timedOut = 1;
                SetThreadState(pThd, THREAD_RUNNABLE);
            } else break;
        } else {
            MosTimer * pTmr = container_of(pElm, MosTimer, tmrLink);
            s32 remTicks = (s32)pTmr->wakeTick - Tick.lower;
            if (remTicks <= 0) {
                if ((pTmr->pCallback)(pTmr)) mosRemoveFromList(pElm);
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
    // Save SP and pErrNo context
    if (pRunningThread != NO_SUCH_THREAD) {
        pRunningThread->sp = sp;
        pRunningThread->errNo = *pErrNo;
    } else {
        pRunningThread = &IdleThread;
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
        _NSC_mosInitSecureContexts(KPrint, RawPrintBuffer);
#endif
    }
    // Update Running Thread state
    //   Threads can't directly kill themselves, so do it here.
    //   If thread needs to go onto timer queue, do it here.
    if (pRunningThread->state == THREAD_TIME_TO_STOP) {
        // Arrange death of running thread via kill handler
        if (mosIsOnList(&pRunningThread->tmrLink.link))
            mosRemoveFromList(&pRunningThread->tmrLink.link);
        InitThread(pRunningThread, pRunningThread->pri,
                   pRunningThread->pTermHandler, pRunningThread->termArg,
                   pRunningThread->pStackBottom, pRunningThread->stackSize);
        SetThreadState(pRunningThread, THREAD_RUNNABLE);
    } else if (pRunningThread->state & THREAD_STATE_TICK) {
        // Update running thread timer state (insertion sort in timer queue)
        s32 remTicks = (s32)pRunningThread->wakeTick - Tick.lower;
        MosLink * pElm;
        for (pElm = TimerQueue.pNext; pElm != &TimerQueue; pElm = pElm->pNext) {
            s32 wakeTick;
            if (((MosPmLink *)pElm)->type == ELM_THREAD) {
                Thread * pThd = container_of(pElm, Thread, tmrLink);
                wakeTick = (s32)pThd->wakeTick;
            } else {
                MosTimer * pTmr = container_of(pElm, MosTimer, tmrLink);
                wakeTick = (s32)pTmr->wakeTick;
            }
            s32 tmrRemTicks = wakeTick - Tick.lower;
            if (remTicks <= tmrRemTicks) break;
        }
        mosAddToListBefore(pElm, &pRunningThread->tmrLink.link);
        // If thread is only waiting for a tick
        if (pRunningThread->state == THREAD_WAIT_FOR_TICK)
            mosRemoveFromList(&pRunningThread->runLink);
    }
    // Process ISR event queue
    //  Event queue allows ISRs to signal semaphores without directly
    //  manipulating run queues, making critical sections shorter
    while (1) {
        _mosDisableInterrupts();
        if (!mosIsListEmpty(&ISREventQueue)) {
            MosLink * pElm = ISREventQueue.pNext;
            mosRemoveFromList(pElm);
            // Currently only semaphores are on event list
            MosSem * pSem = container_of(pElm, MosSem, evtLink);
            // Release thread if it is pending
            if (!mosIsListEmpty(&pSem->pendQ)) {
                MosLink * pElm = pSem->pendQ.pNext;
                mosRemoveFromList(pElm);
                _mosEnableInterrupts();
                Thread * pThd = container_of(pElm, Thread, runLink);
                mosAddToFrontOfList(&RunQueues[pThd->pri], pElm);
                if (mosIsOnList(&pThd->tmrLink.link))
                    mosRemoveFromList(&pThd->tmrLink.link);
                SetThreadState(pThd, THREAD_RUNNABLE);
            } else _mosEnableInterrupts();
        } else {
            _mosEnableInterrupts();
            break;
        }
    }
    // Process Priority Queues
    // Start scan at first thread of highest priority, looking for first
    //  thread of list, and if no threads are runnable schedule idle thread.
    Thread * runThd = NULL;
    for (MosThreadPriority pri = 0; pri < MOS_MAX_THREAD_PRIORITIES; pri++) {
        if (!mosIsListEmpty(&RunQueues[pri])) {
            runThd = container_of(RunQueues[pri].pNext, Thread, runLink);
            break;
        }
    }
    if (runThd) {
        // Round-robin
        if (!mosIsAtEndOfList(&RunQueues[runThd->pri], &runThd->runLink))
            mosMoveToEndOfList(&RunQueues[runThd->pri], &runThd->runLink);
    } else runThd = &IdleThread;
    if (MOS_ENABLE_SPLIM_SUPPORT) {
        asm volatile ( "msr psplim, %0" : : "r" (runThd->pStackBottom) );
    }
#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)
    // If there is a new secure context, only load the next context, don't save it.
    // otherwise only save/load the context if it is different.
    if (pRunningThread->secureContextNew != pRunningThread->secureContext) {
        _NSC_mosSwitchSecureContext(-1, runThd->secureContext);
        pRunningThread->secureContext = pRunningThread->secureContextNew;
    } else if (pRunningThread->secureContext != runThd->secureContext)
        _NSC_mosSwitchSecureContext(pRunningThread->secureContext, runThd->secureContext);
#endif
    // Set next thread ID and errno and return its stack pointer
    pRunningThread = runThd;
    *pErrNo = pRunningThread->errNo;
    EVENT(SCHEDULER_EXIT, 0);
    return (u32)pRunningThread->sp;
}

//
// Mutex
//

void mosInitMutex(MosMutex * pMtx) {
    pMtx->pOwner = NO_SUCH_THREAD;
    pMtx->depth = 0;
    mosInitList(&pMtx->pendQ);
}

void mosRestoreMutex(MosMutex * pMtx) {
    if (pMtx->pOwner == (void *)pRunningThread) {
        pMtx->depth = 1;
        mosUnlockMutex(pMtx);
    }
}

bool mosIsMutexOwner(MosMutex * pMtx) {
    return (pMtx->pOwner == (void *)pRunningThread);
}

//
// Semaphore
//

void mosInitSem(MosSem * pSem, u32 startValue) {
    pSem->value = startValue;
    mosInitList(&pSem->pendQ);
    mosInitList(&pSem->evtLink);
}

//
// Security
//

#if (MOS_ARM_RTOS_ON_NON_SECURE_SIDE == true)

// Reserve secure contexts for threads.
//   Scheduler must be locked out during reservation.
//   Scheduler is invoked to change the context.
void mosReserveSecureContext(void) {
    mosWaitForSem(&SecureContextCounter);
    LockScheduler(IntPriMaskLow);
    u32 newContext = __builtin_ctz(SecureContextReservation);
    pRunningThread->secureContextNew = newContext;
    SecureContextReservation &= ~(1 << newContext);
    // Yield so that this thread can immediately use new stack pointer
    YieldThread();
    UnlockScheduler();
}

bool mosTryReserveSecureContext(void) {
    if (mosTrySem(&SecureContextCounter)) {
        LockScheduler(IntPriMaskLow);
        u32 newContext = __builtin_ctz(SecureContextReservation);
        pRunningThread->secureContextNew = newContext;
        SecureContextReservation &= ~(1 << newContext);
        YieldThread();
        UnlockScheduler();
        return true;
    }
    return false;
}

// Revert all threads to default secure context
void mosReleaseSecureContext(void) {
    LockScheduler(IntPriMaskLow);
    u32 oldContext = pRunningThread->secureContext;
    // Reset pointer value for next thread (using current thread context)
    _NSC_mosResetSecureContext(oldContext);
    pRunningThread->secureContextNew = MOS_DEFAULT_SECURE_CONTEXT;
    SecureContextReservation |= (1 << oldContext);
    // Yield so that stack pointer is made available for next thread.
    YieldThread();
    UnlockScheduler();
    mosIncrementSem(&SecureContextCounter);
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
