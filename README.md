# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS microkernel for the ARM Cortex M series that supports basic threading and IPC primitives.

It is a work in progress and is intended for "fun personal projects."

MOS was originally developed on a STMicroelectronics STM32F4 Discovery and has been tested on the following boards:
* STM32F767ZI Nucleo-144 (ARM Cortex M7)
* STM32L562 Discovery Kit (ARM Cortex M33)
* STM32F4 Discovery (ARM Cortex M4F)
* STM32G071RB Nucleo (ARM Cortex M0+)

Design Goals:
* RTOS: Hard priorities, bounded execution and short critical sections.
* Simple configuration with low use of conditional compilation (no config.h files).
* Small code size (_e.g.:_ minimal compiled size is ~5KB).
* Baseline kernel is static with optional extensions.
* Extensions include best-effort allocator and dynamic kernel extension.
* Dynamic kernel extension supports thread-local storage.
* Easily customizable.
* Provide example BSPs and example applications.

Included Primitives:
* Recursive mutex with priority inheritance.
* Semaphores (counting / multi-bit binary).
* Message queues (multi-priority).
* SysTick-based timers.
* Atomic operations.

Optional Modules:
* Best-effort allocator with multiple non-contiguous region support.
* Dynamic kernel extension with thread-local storage.
* TrustZone security.
* Shared context (cooperative multitasking where clients share same thread and message queue--for tiny footprints).
* Command shell.

Supported toolchains / architectures:
* GCC
* ARM M0/M1/M0+ (Arch v6-M)
* ARM M3/M4/M7 (Arch v7-M)
* ARM M4F/M7F (Arch v7-M with hardware floating point using lazy stacking)
* ARM M23/M33/M55 (Arch v8-M) - TrustZone context switches.

Features it probably WILL NEVER have:
* Full MPU support
* Non-ARM architecture support
* Non-GCC toolchain support
