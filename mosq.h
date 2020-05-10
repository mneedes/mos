
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// Miscellaneous
//

#ifndef _MOSQ_H_
#define _MOSQ_H_

// Single-reader / single-writer non-blocking lock-free FIFO
//   NOTE: Usable depth is len - 1.
//   External mutex is required to support multiple readers or writers.
typedef struct {
    volatile u32 * buf;
    u32 len;
    volatile u32 tail;
    volatile u32 head;
} MosqFIFO;

// Non-blocking FIFO
void MosqInitFIFO(MosqFIFO * fifo, u32 * buf, u32 len);
bool MosqWriteToFIFO(MosqFIFO * fifo, u32 data);
bool MosqReadFromFIFO(MosqFIFO * fifo, u32 * data);

// Read head without removing entry
bool MosqSnoopFIFO(MosqFIFO * fifo, u32 * data);

#endif
