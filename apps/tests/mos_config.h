
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos_config.h
/// \brief MOS Configuration File
///
/// MOS is intended to be easy to maintain, so rather than providing tons
/// of options here and cluttering the code with #ifdefs, instead consider
/// modifying the microkernel itself to suit system requirements.
//

#ifndef _MOS_CONFIG_H_
#define _MOS_CONFIG_H_

/// Thread priorities <=> [0 ... MOS_MAX_THREAD_PRIORITIES - 1].
/// The lower the number the higher the priority
#define MOS_MAX_THREAD_PRIORITIES   4

/// Interrupt tick rate
///
#define MOS_MICRO_SEC_PER_TICK      1000

/// Monitor maximum stack usage on context switches.
///
#define MOS_STACK_USAGE_MONITOR     true

/// Enable events (required for MOS profiling).
///
#define MOS_ENABLE_EVENTS           false

/// Enable "unintentional" alignment faults.
/// Recommend true for small from-scratch projects,
/// false for large, pre-existing code bases.
#define MOS_ENABLE_UNALIGN_FAULTS   false

/// Hang on exceptions.
/// Generally set to true unless this is a testbench.
/// Can be used in systems with watchdog timer reset to reboot
#define MOS_HANG_ON_EXCEPTIONS      false

#endif
