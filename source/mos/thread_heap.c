
// Copyright 2020-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/thread_heap.h>

static MosHeap * pThreadHeap = NULL;
static MosMutex ThreadMutex;

static void FreeThread(MosThread * pThd) {
    if (pThreadHeap) {
        MosFree(pThreadHeap, MosGetStackBottom(pThd));
        MosFree(pThreadHeap, pThd);
    }
}

void MosSetThreadHeap(MosHeap * pHeap) {
    pThreadHeap = pHeap;
    MosInitMutex(&ThreadMutex);
}

bool MosAllocThread(MosThread ** ppThd, u32 stackSize) {
    if (!pThreadHeap) return false;
    bool rtn = false;
    u8 * pStackBottom = (u8 *)MosAlloc(pThreadHeap, stackSize);
    if (pStackBottom == NULL) return false;
    MosThread * pThd = (MosThread *)MosAlloc(pThreadHeap, sizeof(MosThread));
    if (pThd == NULL) {
        MosFree(pThreadHeap, pStackBottom);
        return false;
    }
    MosSetStack(pThd, pStackBottom, stackSize);
    MosLockMutex(&ThreadMutex);
    if (*ppThd == NULL) {
        pThd->refCnt = 1;
        *ppThd = pThd;
        rtn = true;
    } else FreeThread(pThd);
    MosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool
MosAllocAndRunThread(MosThread ** ppThd, MosThreadPriority pri,
                     MosThreadEntry * pEntry, s32 arg, u32 stackSize) {
    if (!MosAllocThread(ppThd, stackSize)) return false;
    if (!MosInitAndRunThread(*ppThd, pri, pEntry, arg, MosGetStackBottom(*ppThd),
                             stackSize)) {
        MosDecThreadRefCount(ppThd);
        return false;
    }
    return true;
}

bool MosIncThreadRefCount(MosThread ** ppThd) {
    bool rtn = false;
    MosLockMutex(&ThreadMutex);
    MosThread * pThd = *ppThd;
    if (pThd != NULL) {
        pThd->refCnt++;
        rtn = true;
    }
    MosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool MosDecThreadRefCount(MosThread ** ppThd) {
    bool rtn = false;
    MosLockMutex(&ThreadMutex);
    MosThread * pThd = *ppThd;
    if (pThd != NULL && --pThd->refCnt <= 0) {
        *ppThd = NULL;
        FreeThread(pThd);
        rtn = true;
    }
    MosUnlockMutex(&ThreadMutex);
    return rtn;
}
