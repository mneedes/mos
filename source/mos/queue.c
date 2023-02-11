
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Queues
//

#include <mos/queue.h>

MOS_ISR_SAFE static void CopyToTail(MosQueue * pQueue, const u32 * pData) {
    u32 mask = mosDisableInterrupts();
    for (u32 ix = 0; ix < pQueue->elmSize; ix++) *pQueue->pTail++ = *pData++;
    if (pQueue->pTail == pQueue->pEnd) pQueue->pTail = pQueue->pBegin;
    asm volatile ( "dmb" );
    mosEnableInterrupts(mask);
}

MOS_ISR_SAFE static void CopyFromHead(MosQueue * pQueue, u32 * pData) {
    u32 mask = mosDisableInterrupts();
    for (u32 ix = 0; ix < pQueue->elmSize; ix++) *pData++ = *pQueue->pHead++;
    if (pQueue->pHead == pQueue->pEnd) pQueue->pHead = pQueue->pBegin;
    asm volatile ( "dmb" );
    mosEnableInterrupts(mask);
}

void mosInitQueue(MosQueue * pQueue, void * pBuffer, u32 elmSize, u32 numElm) {
    mosAssert((elmSize & 0x3) == 0x0);
    pQueue->elmSize  = elmSize >> 2;
    pQueue->pBegin   = (u32 *)pBuffer;
    pQueue->pEnd     = pQueue->pBegin + (numElm * pQueue->elmSize);
    pQueue->pTail    = pQueue->pBegin;
    pQueue->pHead    = pQueue->pBegin;
    pQueue->pSignal  = NULL;
    mosInitSem(&pQueue->semTail, numElm);
    mosInitSem(&pQueue->semHead, 0);
}

void mosSetMultiQueueChannel(MosQueue * pQueue, MosSignal * pSignal, u16 channel) {
    pQueue->channel = channel;
    pQueue->pSignal = pSignal;
}

void mosSendToQueue(MosQueue * pQueue, const void * pData) {
    // After taking semaphore context has a "license to write one entry,"
    // but it still must wait if another context is trying to do the same
    // thing in a thread.
    mosWaitForSem(&pQueue->semTail);
    CopyToTail(pQueue, pData);
    mosIncrementSem(&pQueue->semHead);
    if (pQueue->pSignal) mosRaiseSignalForChannel(pQueue->pSignal, pQueue->channel);
}

MOS_ISR_SAFE bool mosTrySendToQueue(MosQueue * pQueue, const void * pData) {
    // MosTrySendToQueue() and MosTryReceiveFromQueue() are ISR safe since
    // they do not block and interrupts are locked out when queues are being
    // manipulated.
    if (!mosTrySem(&pQueue->semTail)) return false;
    CopyToTail(pQueue, pData);
    mosIncrementSem(&pQueue->semHead);
    if (pQueue->pSignal) mosRaiseSignalForChannel(pQueue->pSignal, pQueue->channel);
    return true;
}

bool mosSendToQueueOrTO(MosQueue * pQueue, const void * pData, u32 ticks) {
    if (mosWaitForSemOrTO(&pQueue->semTail, ticks)) {
        CopyToTail(pQueue, pData);
        mosIncrementSem(&pQueue->semHead);
        if (pQueue->pSignal) mosRaiseSignalForChannel(pQueue->pSignal, pQueue->channel);
        return true;
    }
    return false;
}

void mosReceiveFromQueue(MosQueue * pQueue, void * pData) {
    mosWaitForSem(&pQueue->semHead);
    CopyFromHead(pQueue, pData);
    mosIncrementSem(&pQueue->semTail);
}

MOS_ISR_SAFE bool mosTryReceiveFromQueue(MosQueue * pQueue, void * pData) {
    if (mosTrySem(&pQueue->semHead)) {
        CopyFromHead(pQueue, pData);
        mosIncrementSem(&pQueue->semTail);
        return true;
    }
    return false;
}

bool mosReceiveFromQueueOrTO(MosQueue * pQueue, void * pData, u32 ticks) {
    if (mosWaitForSemOrTO(&pQueue->semHead, ticks)) {
        CopyFromHead(pQueue, pData);
        mosIncrementSem(&pQueue->semTail);
        return true;
    }
    return false;
}

s16 mosWaitOnMultiQueue(MosSignal * pSignal, u32 * pFlags) {
    // Update flags in case some are still set, then block if needed
    *pFlags |= mosPollSignal(pSignal);
    if (*pFlags == 0) *pFlags = mosWaitForSignal(pSignal);
    return mosGetNextChannelFromFlags(pFlags);
}

s16 mosWaitOnMultiQueueOrTO(MosSignal * pSignal, u32 * pFlags, u32 ticks) {
    // Update flags in case some are still set, then block if needed
    *pFlags |= mosPollSignal(pSignal);
    if (*pFlags == 0) {
        *pFlags = mosWaitForSignalOrTO(pSignal, ticks);
        if (*pFlags == 0) return -1;
    }
    return mosGetNextChannelFromFlags(pFlags);
}
