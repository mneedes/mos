# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS micro-kernel for ARM M series that supports basic threading and IPC primitives.

It is a work in progress and is intended for "fun personal projects."

MOS was originally developed on a SGS-Thomson STM32F4Discovery and has also been tested on the STM32F767ZI Nucleo-144 board.  The HAL layer example should work immediately with the STM32F4Discovery or for similar boards with minimal modifications.

Design Goals:
* RTOS: Hard priorities and bounded execution
* Short critical sections: Interrupts are only disabled for a few instructions, BASEPRI is used for scheduler locking-allowing higher priority interrupts to operate unimpeded.
* Simple configuration with low use of conditional compilation.
* Tick reduction (_i.e.:_ the so-called "tickless" operation)
* Small code size (_e.g.:_ mos/kernel.c compiled size is < 5KB)
* Static kernel and (optionally) static application
* Easily modifiable

Included Primitives:
* Recursive mutex with priority inheritance
* Semaphores
* Message queues
* SysTick-based Timers

Included Optional Modules:
* Heap
* Logging
* Command shell
* Test bench

Supported toolchains / architectures:
* GCC
* ARM M3/M4/M7
* M4F/M7F (hardware floating point using lazy stacking)

Future
* C++ bindings
* M0/M0+/M1

Features it probably WILL NEVER have:
* MPU support
* Non-ARM architecture support
* Non-GCC toolchain support
