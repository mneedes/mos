# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS microkernel for the ARM Cortex M series that supports basic threading and IPC primitives.

It is a work in progress and is intended for "fun personal projects."

MOS was originally developed on a STMicroelectronics STM32F4 Discovery and has been tested on the following boards:
* STM32F767ZI Nucleo-144 (ARM Cortex M7)
* STM32L562 Discovery Kit (ARM Cortex M33)
* STM32F4 Discovery (ARM Cortex M4F)
* STM32G071RB Nucleo (ARM Cortex M0+)

Design Goals:
* RTOS: Hard priorities and bounded execution.
* Short critical sections.
* Simple configuration with low use of conditional compilation.
* Tick reduction (_i.e.:_ the so-called "tickless" operation).
* Small code size (_e.g.:_ mos/kernel*.c compiled size is ~5KB).
* Static kernel and (optionally) static application.
* Easily customizable.
* Provide example BSPs and example applications.

Included Primitives:
* Recursive mutex with priority inheritance
* Semaphores (counting / multi-bit binary)
* Message queues (multi-priority)
* SysTick-based timers

Included Optional Modules:
* Heap
* Shared context (cooperative multitasking where clients share same thread and message queue, intended for small memory footprints)
* Logging
* Command shell
* Test bench

Supported toolchains / architectures:
* GCC
* ARM M0/M1/M0+ (Arch v6-M)
* ARM M3/M4/M7 (Arch v7-M)
* ARM M4F/M7F (Arch v7-M with hardware floating point using lazy stacking)
* ARM M23/M33/M55 (Arch v8-M) - currently supporting a single security mode: secure or non-secure.
* TrustZone can be used with interrupts disabled.

Future
* Better documentation...
* Support Context switches in TrustZone
* C++ bindings

Features it probably WILL NEVER have:
* Full MPU support
* Non-ARM architecture support
* Non-GCC toolchain support
