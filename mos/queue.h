
//  Copyright 2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  MOS Queues
//

#ifndef _MOS_QUEUE_H_
#define _MOS_QUEUE_H_

#include <mos/kernel.h>

// Multi-writer / multi-reader blocking FIFO
typedef struct MosQueue {
    MosSem sem_tail;
    MosSem sem_head;
    u32 * buf;
    u32 num_elm;
    u32 tail;
    u32 head;
} MosQueue;

// Queue for 32-bit data

void MosInitQueue32(MosQueue * queue, u32 * buf, u32 num_elm);
void MosSendToQueue32(MosQueue * queue, u32 data);
MOS_ISR_SAFE bool MosTrySendToQueue32(MosQueue * queue, u32 data);
// Returns false on timeout, true if sent
bool MosSendToQueue32OrTO(MosQueue * queue, u32 data, u32 ticks);
u32 MosReceiveFromQueue32(MosQueue * queue);
MOS_ISR_SAFE bool MosTryReceiveFromQueue32(MosQueue * queue, u32 * data);
// Returns false on timeout, true if received
bool MosReceiveFromQueue32OrTO(MosQueue * queue, u32 * data, u32 ticks);

// General Queue
#if 0
void MosInitQueue32(MosQueue * queue, u32 * buf, u32 elm_size, u32 num_elm);
void MosSendToQueue32(MosQueue * queue, const u32 * data);
MOS_ISR_SAFE bool MosTrySendToQueue32(MosQueue * queue, const u32 * data);
// Returns false on timeout, true if sent
bool MosSendToQueue32OrTO(MosQueue * queue, const u32 * data, u32 ticks);
u32 MosReceiveFromQueue32(MosQueue * queue);
MOS_ISR_SAFE bool MosTryReceiveFromQueue32(MosQueue * queue, u32 * data);
// Returns false on timeout, true if received
bool MosReceiveFromQueue32OrTO(MosQueue * queue, u32 * data, u32 ticks);
#endif

#endif
