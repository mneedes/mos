
// Copyright 2020-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Thread Heap Library (Dynamic thread interface)
//

#ifndef _MOS_THREAD_HEAP_H_
#define _MOS_THREAD_HEAP_H_

#include <mos/kernel.h>
#include <mos/heap.h>

void MosInitThreadHeap(MosHeap * pHeap);

// Allocate threads, setting reference count to 1
bool MosAllocThread(MosThread ** ppThd, u32 stackSize);
bool MosAllocAndRunThread(MosThread ** ppThd, MosThreadPriority pri,
                          MosThreadEntry * pEntry, s32 arg, u32 stackSize);
// Increment reference count (used when sharing handles between threads)
bool MosIncThreadRefCount(MosThread ** ppThd);

// Decrement reference count (frees thread when reference count is zero)
// NOTE: A running thread should not have its reference count decremented.
bool MosDecThreadRefCount(MosThread ** ppThd);

#endif
