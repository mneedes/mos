
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#ifndef _MOS_FIFO_H_
#define _MOS_FIFO_H_

#include <mos/defs.h>

// Single-reader / single-writer non-blocking lock-free FIFO
//   NOTE: Usable depth is len - 1.
//   External mutex is required to support multiple readers or writers.
typedef struct {
    volatile u32 * buf;
    u32 len;
    volatile u32 tail;
    volatile u32 head;
} MosFIFO32;

// Non-blocking FIFO
MOS_ISR_SAFE void MosInitFIFO32(MosFIFO32 * fifo, u32 * buf, u32 len);
MOS_ISR_SAFE bool MosWriteToFIFO32(MosFIFO32 * fifo, u32 data);
MOS_ISR_SAFE bool MosReadFromFIFO32(MosFIFO32 * fifo, u32 * data);

// Read head without removing entry
MOS_ISR_SAFE bool MosSnoopFIFO32(MosFIFO32 * fifo, u32 * data);

#endif
