
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// Miscellaneous
//

#ifndef _MOS_FIFO_H_
#define _MOS_FIFO_H_

#include "mos/kernel.h"

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
void MosInitFIFO32(MosFIFO32 * fifo, u32 * buf, u32 len);
bool MosWriteToFIFO32(MosFIFO32 * fifo, u32 data);
bool MosReadFromFIFO32(MosFIFO32 * fifo, u32 * data);

// Read head without removing entry
bool MosSnoopFIFO32(MosFIFO32 * fifo, u32 * data);

#endif