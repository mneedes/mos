
// Copyright 2020-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  thread_heap.h
/// \brief Dynamically allocated thread interface.
/// \note This is a work in progress...

#ifndef _MOS_THREAD_HEAP_H_
#define _MOS_THREAD_HEAP_H_

#include <mos/kernel.h>
#include <mos/heap.h>

/// Set heap threads are to be allocated from.
///
void MosSetThreadHeap(MosHeap * pHeap);
/// Allocate a thread and increments its reference count to 1.
/// Use MosInitAndRunThread() to run the thread.
/// TODO: FIX THIS API
bool MosAllocThread(MosThread ** ppThd, u32 stackSize);
/// Allocate a thread, increment its reference count to 1 and run it.
///
bool MosAllocAndRunThread(MosThread ** ppThd, MosThreadPriority pri,
                          MosThreadEntry * pEntry, s32 arg, u32 stackSize);
/// Increment thread reference count. Every user of a shared thread handle should increment it.
/// Returns true if thread handle can be used.
///
bool MosIncThreadRefCount(MosThread ** ppThd);
/// Decrement reference count (frees thread when reference count is zero).
///
bool MosDecThreadRefCount(MosThread ** ppThd);

#endif
