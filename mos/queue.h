
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
    u32 len;
    u32 tail;
    u32 head;
} MosQueue;

void MosInitQueue(MosQueue * queue, u32 * buf, u32 len);
void MosSendToQueue(MosQueue * queue, u32 data);
MOS_ISR_SAFE bool MosTrySendToQueue(MosQueue * queue, u32 data);
// Returns false on timeout, true if sent
bool MosSendToQueueOrTO(MosQueue * queue, u32 data, u32 ticks);
u32 MosReceiveFromQueue(MosQueue * queue);
MOS_ISR_SAFE bool MosTryReceiveFromQueue(MosQueue * queue, u32 * data);
// Returns false on timeout, true if received
bool MosReceiveFromQueueOrTO(MosQueue * queue, u32 * data, u32 ticks);

#endif
