
//  Copyright 2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  Blocking Message Queues
//

#include <mos/queue.h>

#define COPY_TO_TAIL \
    u32 * from = (u32 *) data; \
    asm volatile ( "cpsid if" ); \
    for (u32 ix = 0; ix < queue->elm_size; ix++) *queue->tail++ = *from++; \
    if (queue->tail == queue->end) queue->tail = queue->begin; \
    asm volatile ( \
        "dmb\n\t" \
        "cpsie if\n\t" \
    );

#define COPY_FROM_HEAD \
    u32 * to = (u32 *) data; \
    asm volatile ( "cpsid if" ); \
    for (u32 ix = 0; ix < queue->elm_size; ix++) *to++ = *queue->head++; \
    if (queue->head == queue->end) queue->head = queue->begin; \
    asm volatile ( \
        "dmb\n\t" \
        "cpsie if\n\t" \
    );

MOS_ISR_SAFE static void CopyToTail(MosQueue * queue, const u32 * data) {
    asm volatile ( "cpsid if" );
    for (u32 ix = 0; ix < queue->elm_size; ix++) *queue->tail++ = *data++;
    if (queue->tail == queue->end) queue->tail = queue->begin;
    asm volatile (
        "dmb\n\t"
        "cpsie if\n\t"
    );
}

MOS_ISR_SAFE static void CopyFromHead(MosQueue * queue, u32 * data) {
    asm volatile ( "cpsid if" );
    for (u32 ix = 0; ix < queue->elm_size; ix++) *data++ = *queue->head++;
    if (queue->head == queue->end) queue->head = queue->begin;
    asm volatile (
        "dmb\n\t"
        "cpsie if\n\t"
    );
}

void MosInitQueue(MosQueue * queue, void * begin, u32 elm_size, u32 num_elm) {
    MosAssert((elm_size & 0x3) == 0x0);
    queue->elm_size = elm_size >> 2;
    queue->begin    = (u32 *) begin;
    queue->end      = queue->begin + (num_elm * queue->elm_size);
    queue->tail     = queue->begin;
    queue->head     = queue->begin;
    MosInitSem(&queue->sem_tail, num_elm);
    MosInitSem(&queue->sem_head, 0);
}

void MosSendToQueue(MosQueue * queue, const void * data) {
    // After taking semaphore context has a "license to write one entry,"
    // but it still must wait if another context is trying to do the same
    // thing in a thread.
    MosWaitForSem(&queue->sem_tail);
    CopyToTail(queue, data);
    MosIncrementSem(&queue->sem_head);
}

MOS_ISR_SAFE bool MosTrySendToQueue(MosQueue * queue, const void * data) {
    // MosTrySendToQueue() and MosTryReceiveFromQueue() are ISR safe since
    // they do not block and interrupts are locked out when queues are being
    // manipulated.
    if (!MosTrySem(&queue->sem_tail)) return false;
    CopyToTail(queue, data);
    MosIncrementSem(&queue->sem_head);
    return true;
}

bool MosSendToQueueOrTO(MosQueue * queue, const void * data, u32 ticks) {
    if (MosWaitForSemOrTO(&queue->sem_tail, ticks)) {
        CopyToTail(queue, data);
        MosIncrementSem(&queue->sem_head);
        return true;
    }
    return false;
}

void MosReceiveFromQueue(MosQueue * queue, void * data) {
    MosWaitForSem(&queue->sem_head);
    CopyFromHead(queue, data);
    MosIncrementSem(&queue->sem_tail);
}

MOS_ISR_SAFE bool MosTryReceiveFromQueue(MosQueue * queue, void * data) {
    if (MosTrySem(&queue->sem_head)) {
        CopyFromHead(queue, data);
        MosIncrementSem(&queue->sem_tail);
        return true;
    }
    return false;
}

bool MosReceiveFromQueueOrTO(MosQueue * queue, void * data, u32 ticks) {
    if (MosWaitForSemOrTO(&queue->sem_head, ticks)) {
        CopyFromHead(queue, data);
        MosIncrementSem(&queue->sem_tail);
        return true;
    }
    return false;
}
