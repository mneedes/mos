
//  Copyright 2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#include "mos/thread_heap.h"

static MosHeap * ThreadHeap = NULL;
static MosMutex ThreadMutex;

static void FreeThread(MosThread * thd) {
    MosFree(ThreadHeap, MosGetStackBottom(thd));
    MosFree(ThreadHeap, thd);
}

void MosInitThreadHeap(MosHeap * heap) {
    ThreadHeap = heap;
    MosInitMutex(&ThreadMutex);
}

bool MosAllocThread(MosThread ** _thd, u32 stack_size) {
    if (!ThreadHeap) return false;
    bool rtn = false;
    u8 * stack_bottom = (u8 *) MosAlloc(ThreadHeap, stack_size);
    if (stack_bottom == NULL) return false;
    MosThread * thd = (MosThread *) MosAlloc(ThreadHeap, sizeof(MosThread));
    if (thd == NULL) {
        MosFree(ThreadHeap, stack_bottom);
        return false;
    }
    MosSetStack(thd, stack_bottom, stack_size);
    MosTakeMutex(&ThreadMutex);
    if (*_thd == NULL) {
        thd->ref_cnt = 1;
        *_thd = thd;
        rtn = true;
    } else FreeThread(thd);
    MosGiveMutex(&ThreadMutex);
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
    if (!ThreadHeap) return false;
    bool rtn = false;
    MosTakeMutex(&ThreadMutex);
    MosThread * thd = *_thd;
    if (thd != NULL) {
        thd->ref_cnt++;
        rtn = true;
    }
    MosGiveMutex(&ThreadMutex);
    return rtn;
}

bool MosDecThreadRefCount(MosThread ** _thd) {
    if (!ThreadHeap) return false;
    bool rtn = false;
    MosTakeMutex(&ThreadMutex);
    MosThread * thd = *_thd;
    if (thd != NULL && --thd->ref_cnt <= 0) {
        *_thd = NULL;
        FreeThread(thd);
        rtn = true;
    }
    MosGiveMutex(&ThreadMutex);
    return rtn;
}

