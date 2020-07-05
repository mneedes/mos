
# Introduction

..Intended for ARM M3/M4/M7 Series

This so far has been an exercise to see how well a simple scheduler implementing tick reduction could perform assuming:

1. A fixed maximum number of threads.
2. Scheduler polls state of blocked threads instead of being event driven.
3. Yield flags are maintained instead of wait lists on the blocked resources such as semaphores and mutex.

## MOS configuration (mos_config.h)

## Processor HAL (mos_phal.h)

Edit mos_phal.h file to include the configuration file for the specific processor implementation including CMSIS definitions.

## HAL/BSP

Before invoking MosInit() the HAL or BSP should perform the following tasks:

1. Configure SysTick registers along with any required clocks and PLLs.
2. Configure NVIC Priority level and NVIC Priority Group registers.

Note that MosInit() will automatically set SysTick and PendSV to the lowest interrupt priority level. Interrupts may be disabled prior to running MosInit() and they will be reenabled automatically when MosRunScheduler() is invoked.

## HAL interface

# Scheduler Operation

## Yielding

Any time a thread blocks it will yield to the MOS scheduler by invoking the PendSV interrupt handler.  Note that Service (SVC) system calls are not utilized by MOS.

A thread may cooperatively yield at any time by invoking MosYieldThread().

## Round-Robin RT

A separate priority queue is maintained for each thread priority level that MOS is configured for. Just before a thread is scheduled to run its entry is placed at the end of its priority queue (round-robin commutation).

Threads are scanned (polled) from highest to lowest priority to determine the next thread ready to run (first scan).  Threads are scanned a second time but ONLY in the same priority as the ready-to-run thread.  The purpose of the second scan is to determine whether tick reduction should be enabled.

## Tick Reduction

Tick is enabled if there are 2 or more threads that are ready to run on the highest priority, or if there is 1 thread ready-to-run or at least 1 thread waiting on a mutex or semaphore.  In the case of mutex this may not be a big issue if mutexes are held for a short period of time, however this is not the most optimal operation for a semaphore that is not incremented very often.

If a wait list were maintained for each resource then the above algorithm could be optimized and less ticks would result.

# Primitives

## Mutexes

MOS mutexes have the following properties:

1. Recursion:
 + Each mutex maintains a counter; every time a tread takes a mutex, the counter is incremented. Every time a mutex is given, the counter is decremented. A mutex is only released when the counter reaches zero.

2. Indirect multiple-level priority inheritance:
 + The priority of the (lower priority) mutex-owning threads are not changed directly, rather the owning threads are run in place of higher priority threads when the higher priority threads are scheduled to run. Round-robin thread commutation is still preserved at the high priority if multiple threads are ready to run.
 + The scheduler is recursive and therefore capable of performing multiple levels of priority inheritance in the rare situations there are multiple threads at several priorities potentially deadlocked.

If a higher priority thread is attempting to obtain a mutex owned by a lower priority thread, the scheduler will set the "to_yield" flag on the mutex indicating that when it is released the thread should immediately yield; thus running the scheduler and potentially running the high priority thread.

If a thread at a given priority is attempting to obtain a mutex owned by a thread of the same priority, the to_yield flag will *NOT* be set. The scheduler will just naturally run the owning thread until it releases the mutex. This avoids unnecessary yields and reduces scheduler overhead. It also potentially smooths out round-robin commutation.  NOTE: For this to work the tick must be enabled even if only one thread is ready to run at the highest priority. Otherwise the thread needing the mutex will not get to run in a timely fashion.

## Semaphores

MOS Semaphores

1. Maintain a 32-bit count.
2. Can be given or taken (non-blocking poll via MosTrySem()) from interrupt context.

The scheduler is only pended if the thread waiting for the semaphore (if any) has a higher priority than the current thread context.
