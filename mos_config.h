
//  Copyright 2019 Matthew C Needes
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

// Application threads IDs <=> [1 ... MOS_MAX_APP_THREADS].
// Thread 0 is reserved for the idle thread.
#define MOS_MAX_APP_THREADS         6

// Thread priorities <=> [0 ... MOS_MAX_THREAD_PRIORITIES - 1].
// The lower the number the higher the priority
#define MOS_MAX_THREAD_PRIORITIES   4

// Tick rate
#define MOS_MICRO_SEC_PER_TICK      1000

// Starting tick count
//   Set to early rollover to always test rollovers,
//   Set to zero for more intuitive time keeping
#define MOS_START_TICK_COUNT        0xFFFFFF00
//#define MOS_START_TICK_COUNT        0x00000000

// Keep tick interrupt running at slowest rate to maintain
// time even when there are no timer events scheduled.
#define MOS_KEEP_TICKS_RUNNING      false

// Maximum number of supported block sizes for MOSH heap
#define MOSH_MAX_HEAP_BLOCK_SIZES   6

// Length of internal buffer for terminal messages
#define MOST_PRINT_BUFFER_SIZE      128

// Maximum number of arguments on shell command line 
#define MOST_CMD_MAX_ARGC           10

#endif
