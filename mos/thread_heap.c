
//  Copyright 2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#include "mos/thread_heap.h"

static MosHeap * ThreadHeap = NULL;

static void FreeThread(MosThread * thd) {
    MosFree(ThreadHeap, MosGetStackBottom(thd));
    MosFree(ThreadHeap, thd);
}

void MosSetThreadHeap(MosHeap * heap) {
    ThreadHeap = heap;
}

MosThread *
MosAllocThread(u32 stack_size) {
    if (!ThreadHeap) return NULL;
    u8 * stack_bottom = (u8 *) MosAlloc(ThreadHeap, stack_size);
    if (stack_bottom == NULL) return NULL;
    MosThread * thd = (MosThread *) MosAlloc(ThreadHeap, sizeof(MosThread));
    if (thd == NULL) {
        MosFree(ThreadHeap, stack_bottom);
        return NULL;
    }
    MosSetStack(thd, stack_bottom, stack_size);
    return thd;
}

MosThread *
MosAllocAndRunThread(MosThreadPriority pri, MosThreadEntry * entry, s32 arg, u32 stack_size) {
    MosThread * thd = MosAllocThread(stack_size);
    if (thd == NULL) return NULL;
    if (!MosInitAndRunThread(thd, pri, entry, arg, MosGetStackBottom(thd), stack_size)) {
        FreeThread(thd);
        return NULL;
    }
    return thd;
}

void MosFreeThread(MosThread * thd) {
    if (!ThreadHeap) return;
    FreeThread(thd);
}
