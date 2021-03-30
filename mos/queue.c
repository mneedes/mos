
//  Copyright 2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
//  Blocking Message Queues
//

#include <mos/queue.h>

void MosInitQueue(MosQueue * queue, u32 * buf, u32 len) {
    queue->buf = buf;
    queue->len = len;
    queue->head = 0;
    queue->tail = 0;
    MosInitSem(&queue->sem_head, 0);
    MosInitSem(&queue->sem_tail, len);
}

void MosSendToQueue(MosQueue * queue, u32 data) {
    // After taking semaphore context has a "license to write one entry,"
    // but it still must wait if another context is trying to do the same
    // thing in a thread.
    MosWaitForSem(&queue->sem_tail);
    asm volatile ( "cpsid if" );
    queue->buf[queue->tail] = data;
    if (++queue->tail >= queue->len) queue->tail = 0;
    asm volatile (
        "dmb\n\t"
        "cpsie if\n\t"
    );
    MosIncrementSem(&queue->sem_head);
}

bool MOS_ISR_SAFE MosTrySendToQueue(MosQueue * queue, u32 data) {
    // MosTrySendToQueue() and MosTryReceiveFromQueue() are ISR safe since
    // they do not block and interrupts are locked out when queues are being
    // manipulated.
    if (!MosTrySem(&queue->sem_tail)) return false;
    asm volatile ( "cpsid if" );
    queue->buf[queue->tail] = data;
    if (++queue->tail >= queue->len) queue->tail = 0;
    asm volatile (
        "dmb\n\t"
        "cpsie if\n\t"
    );
    MosIncrementSem(&queue->sem_head);
    return true;
}

bool MosSendToQueueOrTO(MosQueue * queue, u32 data, u32 ticks) {
    if (MosWaitForSemOrTO(&queue->sem_tail, ticks)) {
        asm volatile ( "cpsid if" );
        queue->buf[queue->tail] = data;
        if (++queue->tail >= queue->len) queue->tail = 0;
        asm volatile (
            "dmb\n\t"
            "cpsie if\n\t"
        );
        MosIncrementSem(&queue->sem_head);
        return true;
    }
    return false;
}

u32 MosReceiveFromQueue(MosQueue * queue) {
    MosWaitForSem(&queue->sem_head);
    asm volatile ( "cpsid if" );
    u32 data = queue->buf[queue->head];
    if (++queue->head >= queue->len) queue->head = 0;
    asm volatile (
        "dmb\n\t"
        "cpsie if\n\t"
    );
    MosIncrementSem(&queue->sem_tail);
    return data;
}

bool MOS_ISR_SAFE MosTryReceiveFromQueue(MosQueue * queue, u32 * data) {
    if (MosTrySem(&queue->sem_head)) {
        asm volatile ( "cpsid if" );
        *data = queue->buf[queue->head];
        if (++queue->head >= queue->len) queue->head = 0;
        asm volatile (
            "dmb\n\t"
            "cpsie if\n\t"
        );
        MosIncrementSem(&queue->sem_tail);
        return true;
    }
    return false;
}

bool MosReceiveFromQueueOrTO(MosQueue * queue, u32 * data, u32 ticks) {
    if (MosWaitForSemOrTO(&queue->sem_head, ticks)) {
        asm volatile ( "cpsid if" );
        *data = queue->buf[queue->head];
        if (++queue->head >= queue->len) queue->head = 0;
        asm volatile (
            "dmb\n\t"
            "cpsie if\n\t"
        );
        MosIncrementSem(&queue->sem_tail);
        return true;
    }
    return false;
}

