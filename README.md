# mos
MOS, or Maintainable RTOS, is a simple and lightweight RTOS micro-kernel
for ARM M series that supports basic threading and IPC primitives.

It is intended for low thread count applications, e.g.: "fun personal projects."

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
+ Small code size (currently ~4KB)
+ Includes Test bench
+ Sensible use of abstraction (especially if ported to C++)

Supported architectures:
+ ARM M3 and M4
+ M0 (future)

Features it probably WILL NEVER have:
- MPU support
- Non-ARM architecture support

