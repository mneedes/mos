
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Queues
//

#ifndef _MOS_QUEUE_H_
#define _MOS_QUEUE_H_

#include <mos/kernel.h>

// Multi-writer / multi-reader blocking FIFO
typedef struct MosQueue {
    MosSem   sem_tail;
    MosSem   sem_head;
    u32    * begin;
    u32    * end;
    u32    * tail;
    u32    * head;
    u16      elm_size;
} MosQueue;

// General Queue

// NOTE: Element size must be a multiple of 32-bits
void MosInitQueue(MosQueue * queue, void * buf, u32 elm_size, u32 num_elm);
void MosSendToQueue(MosQueue * queue, const void * data);
MOS_ISR_SAFE bool MosTrySendToQueue(MosQueue * queue, const void * data);
// Returns false on timeout, true if sent
bool MosSendToQueueOrTO(MosQueue * queue, const void * data, u32 ticks);
void MosReceiveFromQueue(MosQueue * queue, void * data);
MOS_ISR_SAFE bool MosTryReceiveFromQueue(MosQueue * queue, void * data);
// Returns false on timeout, true if received
bool MosReceiveFromQueueOrTO(MosQueue * queue, void * data, u32 ticks);

// Easy to use queues with 32 bit data

MOS_INLINE void MosInitQueue32(MosQueue * queue, u32 * buf, u32 num_elm) {
    MosInitQueue(queue, buf, sizeof(u32), num_elm);
}
MOS_INLINE void MosSendToQueue32(MosQueue * queue, u32 data) {
    MosSendToQueue(queue, &data);
}
MOS_ISR_SAFE MOS_INLINE bool MosTrySendToQueue32(MosQueue * queue, u32 data) {
    return MosTrySendToQueue(queue, &data);
}
MOS_INLINE bool MosSendToQueue32OrTO(MosQueue * queue, u32 data, u32 ticks) {
    return MosSendToQueueOrTO(queue, &data, ticks);
}
MOS_INLINE u32 MosReceiveFromQueue32(MosQueue * queue) {
    u32 data;
    MosReceiveFromQueue(queue, &data);
    return data;
}
MOS_ISR_SAFE MOS_INLINE bool MosTryReceiveFromQueue32(MosQueue * queue, u32 * data) {
    return MosTryReceiveFromQueue(queue, data);
}
MOS_INLINE bool MosReceiveFromQueue32OrTO(MosQueue * queue, u32 * data, u32 ticks) {
    return MosReceiveFromQueueOrTO(queue, data, ticks);
}

#endif
