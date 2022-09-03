
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Queues
//

#include <mos/queue.h>

MOS_ISR_SAFE static void CopyToTail(MosQueue * pQueue, const u32 * pData) {
    u32 mask = MosDisableInterrupts();
    for (u32 ix = 0; ix < pQueue->elmSize; ix++) *pQueue->pTail++ = *pData++;
    if (pQueue->pTail == pQueue->pEnd) pQueue->pTail = pQueue->pBegin;
    asm volatile ( "dmb" );
    MosEnableInterrupts(mask);
}

MOS_ISR_SAFE static void CopyFromHead(MosQueue * pQueue, u32 * pData) {
    u32 mask = MosDisableInterrupts();
    for (u32 ix = 0; ix < pQueue->elmSize; ix++) *pData++ = *pQueue->pHead++;
    if (pQueue->pHead == pQueue->pEnd) pQueue->pHead = pQueue->pBegin;
    asm volatile ( "dmb" );
    MosEnableInterrupts(mask);
}

void MosInitQueue(MosQueue * pQueue, void * pBuffer, u32 elmSize, u32 numElm) {
    MosAssert((elmSize & 0x3) == 0x0);
    pQueue->elmSize  = elmSize >> 2;
    pQueue->pBegin   = (u32 *)pBuffer;
    pQueue->pEnd     = pQueue->pBegin + (numElm * pQueue->elmSize);
    pQueue->pTail    = pQueue->pBegin;
    pQueue->pHead    = pQueue->pBegin;
    pQueue->pSignal  = NULL;
    MosInitSem(&pQueue->semTail, numElm);
    MosInitSem(&pQueue->semHead, 0);
}

void MosSetMultiQueueChannel(MosQueue * pQueue, MosSignal * pSignal, u16 channel) {
    pQueue->channel = channel;
    pQueue->pSignal = pSignal;
}

void MosSendToQueue(MosQueue * pQueue, const void * pData) {
    // After taking semaphore context has a "license to write one entry,"
    // but it still must wait if another context is trying to do the same
    // thing in a thread.
    MosWaitForSem(&pQueue->semTail);
    CopyToTail(pQueue, pData);
    MosIncrementSem(&pQueue->semHead);
    if (pQueue->pSignal) MosRaiseSignalForChannel(pQueue->pSignal, pQueue->channel);
}

MOS_ISR_SAFE bool MosTrySendToQueue(MosQueue * pQueue, const void * pData) {
    // MosTrySendToQueue() and MosTryReceiveFromQueue() are ISR safe since
    // they do not block and interrupts are locked out when queues are being
    // manipulated.
    if (!MosTrySem(&pQueue->semTail)) return false;
    CopyToTail(pQueue, pData);
    MosIncrementSem(&pQueue->semHead);
    if (pQueue->pSignal) MosRaiseSignalForChannel(pQueue->pSignal, pQueue->channel);
    return true;
}

bool MosSendToQueueOrTO(MosQueue * pQueue, const void * pData, u32 ticks) {
    if (MosWaitForSemOrTO(&pQueue->semTail, ticks)) {
        CopyToTail(pQueue, pData);
        MosIncrementSem(&pQueue->semHead);
        if (pQueue->pSignal) MosRaiseSignalForChannel(pQueue->pSignal, pQueue->channel);
        return true;
    }
    return false;
}

void MosReceiveFromQueue(MosQueue * pQueue, void * pData) {
    MosWaitForSem(&pQueue->semHead);
    CopyFromHead(pQueue, pData);
    MosIncrementSem(&pQueue->semTail);
}

MOS_ISR_SAFE bool MosTryReceiveFromQueue(MosQueue * pQueue, void * pData) {
    if (MosTrySem(&pQueue->semHead)) {
        CopyFromHead(pQueue, pData);
        MosIncrementSem(&pQueue->semTail);
        return true;
    }
    return false;
}

bool MosReceiveFromQueueOrTO(MosQueue * pQueue, void * pData, u32 ticks) {
    if (MosWaitForSemOrTO(&pQueue->semHead, ticks)) {
        CopyFromHead(pQueue, pData);
        MosIncrementSem(&pQueue->semTail);
        return true;
    }
    return false;
}

s16 MosWaitOnMultiQueue(MosSignal * pSignal, u32 * pFlags) {
    // Update flags in case some are still set, then block if needed
    *pFlags |= MosPollSignal(pSignal);
    if (*pFlags == 0) *pFlags = MosWaitForSignal(pSignal);
    return MosGetNextChannelFromFlags(pFlags);
}

s16 MosWaitOnMultiQueueOrTO(MosSignal * pSignal, u32 * pFlags, u32 ticks) {
    // Update flags in case some are still set, then block if needed
    *pFlags |= MosPollSignal(pSignal);
    if (*pFlags == 0) {
        *pFlags = MosWaitForSignalOrTO(pSignal, ticks);
        if (*pFlags == 0) return -1;
    }
    return MosGetNextChannelFromFlags(pFlags);
}
