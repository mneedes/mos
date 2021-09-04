
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

void MosInitThreadHeap(MosHeap * heap);

// Allocate threads, setting reference count to 1
bool MosAllocThread(MosThread ** thd, u32 stack_size);
bool MosAllocAndRunThread(MosThread ** thd, MosThreadPriority pri,
                          MosThreadEntry * entry, s32 arg, u32 stack_size);
// Increment reference count (used when sharing handles between threads)
bool MosIncThreadRefCount(MosThread ** thd);

// Decrement reference count (frees thread when reference count is zero)
// NOTE: A running thread should not have its reference count decremented.
bool MosDecThreadRefCount(MosThread ** thd);

#endif
