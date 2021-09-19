
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/queue.h
/// \brief MOS Message Queues
///
/// Blocking message queues featuring optional prioritized channels.
/// Allows for multiple writer AND multiple reader contexts.
/// Provides blocking and interrupt-safe non-blocking modes.

#ifndef _MOS_QUEUE_H_
#define _MOS_QUEUE_H_

#include <mos/kernel.h>

// Multi-writer / multi-reader blocking FIFO
typedef struct MosQueue {
    MosSem      sem_tail;
    MosSem      sem_head;
    u32       * begin;
    u32       * end;
    u32       * tail;
    u32       * head;
    u16         elm_size;
    u16         channel;
    MosSignal * signal;
} MosQueue;

//
// Message Queues
//

/// Set buffer to use for queue, queue element size and number of elements.
/// Invoke this before calling any other queue API functions.
/// \note Element size must be a multiple of 4 (32-bits).
void MosInitQueue(MosQueue * queue, void * buf, u32 elm_size, u32 num_elm);

/// Send message to queue, blocking if queue full.
///
void MosSendToQueue(MosQueue * queue, const void * data);

/// Attempt to send message to queue, non-blocking.
/// \return true if message sent.
MOS_ISR_SAFE bool MosTrySendToQueue(MosQueue * queue, const void * data);

/// Send message to queue with timeout.
/// \return true if message sent, false on timeout.
bool MosSendToQueueOrTO(MosQueue * queue, const void * data, u32 ticks);

/// Receive message from queue, blocking if queue empty.
///
void MosReceiveFromQueue(MosQueue * queue, void * data);

/// Attempt to receive message on queue.
/// \return true if message received, false if empty.
MOS_ISR_SAFE bool MosTryReceiveFromQueue(MosQueue * queue, void * data);

/// Receive message from queue, blocking if queue empty with timeout.
/// \return true if message received, false on timeout.
bool MosReceiveFromQueueOrTO(MosQueue * queue, void * data, u32 ticks);

/// Sets signal channel to raise when sending to queue.
/// Lower channel numbers have higher priorities.
/// \param channel channel number to set.  Signal bit is (1 << channel).
void MosSetMultiQueueChannel(MosQueue * queue, MosSignal * signal, u16 channel);

/// Wait on multiple queues of different priorities.
/// \return highest priority channel number and updated flags.
s16 MosWaitOnMultiQueue(MosSignal * signal, u32 * flags);

/// Wait on multiple queues with timeout.
/// \return highest priority channel number and updated flags, -1 for timeout.
s16 MosWaitOnMultiQueueOrTO(MosSignal * signal, u32 * flags, u32 ticks);

//
// Queues with 32-bit data
//

/// Initialize queue for 32-bit data
///
MOS_INLINE void MosInitQueue32(MosQueue * queue, u32 * buf, u32 num_elm) {
    MosInitQueue(queue, buf, sizeof(u32), num_elm);
}

/// Send to queue containing 32-bit data
///
MOS_INLINE void MosSendToQueue32(MosQueue * queue, u32 data) {
    MosSendToQueue(queue, &data);
}

/// Send to queue containing 32-bit data
///
MOS_ISR_SAFE MOS_INLINE bool MosTrySendToQueue32(MosQueue * queue, u32 data) {
    return MosTrySendToQueue(queue, &data);
}

/// Send message to queue with timeout.
/// \return true if message sent, false on timeout.
MOS_INLINE bool MosSendToQueue32OrTO(MosQueue * queue, u32 data, u32 ticks) {
    return MosSendToQueueOrTO(queue, &data, ticks);
}

/// Receive message from queue with 32-bit data, blocking if queue empty.
///
MOS_INLINE u32 MosReceiveFromQueue32(MosQueue * queue) {
    u32 data;
    MosReceiveFromQueue(queue, &data);
    return data;
}

/// Attempt to receive message on queue with 32-bit data.
/// \return true if message received, false if empty.
MOS_ISR_SAFE MOS_INLINE bool MosTryReceiveFromQueue32(MosQueue * queue, u32 * data) {
    return MosTryReceiveFromQueue(queue, data);
}

/// Receive message from queue with 32-bit data, blocking if queue empty with timeout.
/// \return true if message received, false on timeout.
MOS_INLINE bool MosReceiveFromQueue32OrTO(MosQueue * queue, u32 * data, u32 ticks) {
    return MosReceiveFromQueueOrTO(queue, data, ticks);
}

#endif
