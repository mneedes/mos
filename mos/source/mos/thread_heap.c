
// Copyright 2020-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/thread_heap.h>

static MosHeap * pThreadHeap = NULL;
static MosMutex ThreadMutex;

static void FreeThread(MosThread * pThd) {
    MosFree(pThreadHeap, MosGetStackBottom(pThd));
    MosFree(pThreadHeap, pThd);
}

void MosInitThreadHeap(MosHeap * pHeap) {
    pThreadHeap = pHeap;
    MosInitMutex(&ThreadMutex);
}

bool MosAllocThread(MosThread ** _thd, u32 stackSize) {
    if (!pThreadHeap) return false;
    bool rtn = false;
    u8 * pStackBottom = (u8 *)MosAlloc(pThreadHeap, stackSize);
    if (pStackBottom == NULL) return false;
    MosThread * thd = (MosThread *)MosAlloc(pThreadHeap, sizeof(MosThread));
    if (thd == NULL) {
        MosFree(pThreadHeap, pStackBottom);
        return false;
    }
    MosSetStack(thd, pStackBottom, stackSize);
    MosLockMutex(&ThreadMutex);
    if (*_thd == NULL) {
        thd->refCnt = 1;
        *_thd = thd;
        rtn = true;
    } else FreeThread(thd);
    MosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool
MosAllocAndRunThread(MosThread ** _thd, MosThreadPriority pri,
                     MosThreadEntry * entry, s32 arg, u32 stack_size) {
    if (!MosAllocThread(_thd, stack_size)) return false;
    if (!MosInitAndRunThread(*_thd, pri, entry, arg, MosGetStackBottom(*_thd),
                             stack_size)) {
        MosDecThreadRefCount(_thd);
        return false;
    }
    return true;
}

bool MosIncThreadRefCount(MosThread ** _thd) {
    if (!pThreadHeap) return false;
    bool rtn = false;
    MosLockMutex(&ThreadMutex);
    MosThread * thd = *_thd;
    if (thd != NULL) {
        thd->refCnt++;
        rtn = true;
    }
    MosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool MosDecThreadRefCount(MosThread ** _thd) {
    if (!pThreadHeap) return false;
    bool rtn = false;
    MosLockMutex(&ThreadMutex);
    MosThread * thd = *_thd;
    if (thd != NULL && --thd->refCnt <= 0) {
        *_thd = NULL;
        FreeThread(thd);
        rtn = true;
    }
    MosUnlockMutex(&ThreadMutex);
    return rtn;
}

