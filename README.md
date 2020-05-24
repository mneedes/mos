# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS micro-kernel
for ARM M series that supports basic threading and IPC primitives.

It is intended for low thread count applications, e.g.: "fun personal projects."

MOS was originally developed on a SGS-Thomson STM32F4Discovery.  The provided
HAL layer example should work immediately with the STM32F4Discovery or
similar boards.  Otherwise you will need to create your own HAL to try it out.

Design Goals:
* RTOS: Hard priorities and bounded execution
* Simple configuration (mos_config.h)
* Low usage of conditional compilation
* Short critical sections:  Full interrupt locking only for short periods of time.  BASEPRI used for scheduler-only locking, allowing high priority interrupts to run.
* Small code size (mos.c microkernel size is < 5kb)
* Easily extendable
* Thread timers based on SysTick
* Tick reduction (_i.e.:_ the so-called "tickless" operation)

Supported Primitives:
* Recursive mutex with priority inheritance
* Semaphores
* Message queues
 
Optional modules:
* Heap
* Logging
* Command shell 
* Test bench

Future plans:
* C++ bindings

Supported toolchains / architectures:
+ GCC
+ ARM M3/M4 (M7 should work)
+ M4F (future--M4 with hard floating point)
+ M0/M0+/M1 (future--Essentially M3 code without ldrex/strex) 

Features it probably WILL NEVER have:
- MPU support
- Non-ARM architecture support
- Non-GCC toolchain support
