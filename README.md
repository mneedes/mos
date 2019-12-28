# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS micro-kernel
for ARM M series that supports basic threading and IPC primitives.

It is intended for low thread count applications, e.g.: "fun personal projects."

MOS was originally developed on a SGS-Thomson STM32F4Discovery.  The provided
HAL layer example should work immediately with the STM32F4Discovery or
similar boards.  Otherwise you will need to create your own HAL to try it out.

Design Goals:
+ RTOS: Hard priorities and bounded execution
+ Thread timers based on SysTick
+ Recursive mutex with priority inheritance
+ Semaphores
+ Message queues
+ Tick reduction (the so-called "tickless" operation)
+ Simple configuration (mos_config.h)
+ Easily extendable
+ Optional modules (heap, trace)
+ Low usage of conditional compilation (i.e.: very few #ifdefs)
+ Kernel (mos.c) has small code size (currently ~4KB)
+ Include test bench
+ Sensible/Simple use of abstraction
+ C++ implementation

Supported toolchains / architectures:
+ GCC
+ ARM M3/M4 (M7 should work)
+ M4F (future--M4 with hard floating point)
+ M0/M0+/M1 (future--Essentially M3 code without ldrex/strex) 

Features it probably WILL NEVER have:
- MPU support
- Non-ARM architecture support
- Non-GCC toolchain support

