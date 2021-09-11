# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS micro-kernel for ARM M series that supports basic threading and IPC primitives.

It is a work in progress and is intended for "fun personal projects."

MOS was originally developed on a SGS-Thomson STM32F4Discovery and has been tested on the STM32F767ZI Nucleo-144 and STM32L562 Discovery Kit.  The HAL layer example should work immediately with the STM32F4Discovery or for similar boards with minimal modifications.

Design Goals:
* RTOS: Hard priorities and bounded execution
* Short critical sections: Interrupts are disabled for short sections.
* Simple configuration with low use of conditional compilation.
* Tick reduction (_i.e.:_ the so-called "tickless" operation)
* Small code size (_e.g.:_ mos/kernel*.c compiled size is ~5KB)
* Static kernel and (optionally) static application
* Easily modifiable

Included Primitives:
* Recursive mutex with priority inheritance
* Semaphores (counting / multi-bit binary)
* Message queues
* SysTick-based Timers

Included Optional Modules:
* Heap
* Shared Context (multiple clients sharing same thread and message queue)
* Logging
* Command shell
* Test bench

Supported toolchains / architectures:
* GCC
* ARM M0/M1/M0+ (Arch v6-M)
* ARM M3/M4/M7 (Arch v7-M)
* ARM M4F/M7F (Arch v7-M with hardware floating point using lazy stacking)
* ARM M23/M33/M55 (Arch v8-M) - currently supports single mode, either secure or non-secure.

Future
* Better documentation...
* Context switchs in TrustZone
* C++ bindings

Features it probably WILL NEVER have:
* Full MPU support
* Non-ARM architecture support
* Non-GCC toolchain support
