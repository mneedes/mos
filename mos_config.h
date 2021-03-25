
//  Copyright 2019-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS Configuration File
//    MOS is intended to be easy to maintain, so rather than providing tons
//    of options here and cluttering the code with #ifdefs, instead consider
//    modifying the microkernel itself to suit system requirements.
//

#ifndef _MOS_CONFIG_H_
#define _MOS_CONFIG_H_

// Thread priorities <=> [0 ... MOS_MAX_THREAD_PRIORITIES - 1].
// The lower the number the higher the priority
#define MOS_MAX_THREAD_PRIORITIES   4

// Tick rate
#define MOS_MICRO_SEC_PER_TICK      1000

// Starting tick count
//   Set to early rollover to always test rollovers,
//   Set to one for more intuitive time keeping
#define MOS_START_TICK_COUNT        0xFFFFFFFFFFFFFF00
//#define MOS_START_TICK_COUNT      0x0000000000000001

// Enable floating point context switch support
//  Note that if false, floating point might still work
//  if confined to a single context.
#define MOS_FP_CONTEXT_SWITCHING    true

// Monitor maximum stack usage on context switches
//  Adds some CPU overhead if enabled.
#define MOS_STACK_USAGE_MONITOR     false

// Keep tick interrupt running at slowest rate to maintain
// time even when there are no timer events scheduled.
#define MOS_KEEP_TICKS_RUNNING      false

// Enable events (required for MOS profiling)
#define MOS_ENABLE_EVENTS           false

// Enable "unintentional" alignment faults
// Recommend true for small from-scratch projects,
// false for large, pre-existing code bases.
#define MOS_ENABLE_UNALIGN_FAULTS   false

// Enable breakpoint in exceptions if debugger detected
#define MOS_BKPT_IN_EXCEPTIONS      false

#endif
