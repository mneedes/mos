# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS micro-kernel
for ARM M series that supports basic threading and IPC primitives.

It is currently intended for low thread count applications, e.g.: "fun personal projects."

MOS was originally developed on a SGS-Thomson STM32F4Discovery.  The provided
HAL layer example should work immediately with the STM32F4Discovery or
similar boards.  Otherwise you will need to create your own HAL to try it out.

Design Goals:
* RTOS: Hard priorities and bounded execution
* Short critical sections: Interrupts are only disabled for a few instructions, BASEPRI is used for scheduler locking-allowing higher priority interrupts to operate.
* Simple configuration with low use of conditional compilation.
* Tick reduction (_i.e.:_ the so-called "tickless" operation)
* Small code size (e.g.: mos.c microkernel compiled size is < ~5KB)
* Easily modifiable

Included Primitives:
* Recursive mutex with priority inheritance
* Semaphores
* Message queues
* Thread timers based on SysTick

Included Optional Modules:
* Heap
* Logging
* Command shell
* Test bench

Supported toolchains / architectures:
* GCC
* ARM M3/M4/M7
* M4F (future--M4 with hard floating point)

Future
* C++ bindings
* Hard floating point support
* M0/M0+/M1

Features it probably WILL NEVER have:
* MPU support
* Non-ARM architecture support
* Non-GCC toolchain support
