# Introduction

The Maintainable RTOS (MOS) microkernel is an exercise in writing a user-maintainable RTOS with a necessary and sufficient set of primitives.

MOS has a modular construction, the kernel can either stand alone or optionally use provided trace, shell, heap and dynamic thread modules. There are some dependencies: for example the shell module requires the trace module and the dynamic thread module requires the heap.

MOS is currently intended only for ARM Cortex M.

# Configuration

## MOS configuration file (mos_config.h)

Edit mos_config.h to suit application requirements.

## Exception handlers

The SysTick_Handler(), PendSV_Handler() and desired fault handlers should be properly assigned in BSP source and linker configuration files, otherwise MOS will not function correctly.

## Board Support Package (BSP) HAL (bsp_hal.h)

Edit bsp_hal.h file for the specific processor implementation and board features.

## HAL/BSP Initialization

Before invoking MosInit() the HAL or BSP should perform the following tasks:

1. Configure SysTick registers along with any required clocks and PLLs.
2. Configure NVIC Priority level and NVIC Priority Group registers.

Note that MosInit() will automatically set SysTick and PendSV to the lowest interrupt priority level. Interrupts may be disabled prior to running MosInit() and they will be reenabled automatically when MosRunScheduler() is invoked.

## HAL interface

# Scheduler Operation

## Yielding

Any time a thread blocks it will yield to the MOS scheduler by invoking the PendSV interrupt handler.  Note that Service (SVC) system calls are not utilized by MOS.

A thread may cooperatively yield at any time by invoking MosYieldThread().

## Round-Robin Thread Commutation

A separate priority queue is maintained for each thread priority level that MOS is configured for. Just before a thread is scheduled to run its entry is placed at the end of its priority queue (round-robin commutation).

## Tick Reduction

# Primitives

## Mutexes

MOS mutexes have the following properties:

1. Recursion:
 + Each mutex maintains a counter; every time a tread takes a mutex, the counter is incremented. Every time a mutex is given, the counter is decremented. A mutex is only released when the counter reaches zero.

2. Priority inheritance:

## Semaphores

MOS Semaphores

1. Maintain a 32-bit count.
2. Can be given or taken (non-blocking poll via MosTrySem()) from interrupt context.

The scheduler is only pended if the thread waiting for the semaphore (if any) has a higher priority than the current thread context.
