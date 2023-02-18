
// Copyright 2020-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/dynamic_kernel.h>

static MosHeap * pThreadHeap = NULL;
static MosMutex ThreadMutex;

// TODO:
// 1. Refactor files
// 2. When allocating, append new fields to end of MosThread (refCnt / etc).

static void FreeThread(MosThread * pThd) {
    if (pThreadHeap) {
        mosFree(pThreadHeap, mosGetStackBottom(pThd));
        mosFree(pThreadHeap, pThd);
    }
}

void mosSetThreadHeap(MosHeap * pHeap) {
    pThreadHeap = pHeap;
    mosInitMutex(&ThreadMutex);
}

bool mosAllocThread(MosThread ** ppThd, u32 stackSize) {
    if (!pThreadHeap) return false;
    bool rtn = false;
    u8 * pStackBottom = (u8 *)mosAlloc(pThreadHeap, stackSize);
    if (pStackBottom == NULL) return false;
    MosThread * pThd = (MosThread *)mosAlloc(pThreadHeap, sizeof(MosThread));
    if (pThd == NULL) {
        mosFree(pThreadHeap, pStackBottom);
        return false;
    }
    mosSetStack(pThd, pStackBottom, stackSize);
    mosLockMutex(&ThreadMutex);
    if (*ppThd == NULL) {
        pThd->refCnt = 1;
        *ppThd = pThd;
        rtn = true;
    } else FreeThread(pThd);
    mosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool
mosAllocAndRunThread(MosThread ** ppThd, MosThreadPriority pri,
                     MosThreadEntry * pEntry, s32 arg, u32 stackSize) {
    if (!mosAllocThread(ppThd, stackSize)) return false;
    if (!mosInitAndRunThread(*ppThd, pri, pEntry, arg, mosGetStackBottom(*ppThd),
                             stackSize)) {
        mosDecThreadRefCount(ppThd);
        return false;
    }
    return true;
}

bool mosIncThreadRefCount(MosThread ** ppThd) {
    bool rtn = false;
    mosLockMutex(&ThreadMutex);
    MosThread * pThd = *ppThd;
    if (pThd != NULL) {
        pThd->refCnt++;
        rtn = true;
    }
    mosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool mosDecThreadRefCount(MosThread ** ppThd) {
    bool rtn = false;
    mosLockMutex(&ThreadMutex);
    MosThread * pThd = *ppThd;
    if (pThd != NULL && --pThd->refCnt <= 0) {
        *ppThd = NULL;
        FreeThread(pThd);
        rtn = true;
    }
    mosUnlockMutex(&ThreadMutex);
    return rtn;
}
