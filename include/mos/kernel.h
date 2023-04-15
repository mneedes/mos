
// Copyright 2020-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/kernel.h
/// \brief Implemented as a wrapper around the static kernel, the dynamic kernel
///        implements dynamic allocation of kernel resources. Including the dynamic
///        kernel implicitly includes the static kernel and the allocator.
/// \note This is a work in progress...

#ifndef _MOS_KERNEL_H_
#define _MOS_KERNEL_H_

// The static kernel and the allocator
#include <mos/static_kernel.h>
#include <mos/allocator.h>

/// Thread local storage release function, run in the context of the thread destroying
/// the thread resources.
///
typedef void (MosThreadStorageReleaseFunc)(void * pData);

/// Initialize dynamic kernel and set heap used for system resources.
///
void mosInitDynamicKernel(MosHeap * pHeap);

// Dynamic Threads

/// Allocate a thread and increments its reference count to 1.
/// Use mosInitAndRunThread() to run the thread.
bool mosAllocThread(MosThread ** ppThd, u32 stackSize);
/// Allocate a thread, increment its reference count to 1 and run it.
///
bool mosAllocAndRunThread(MosThread ** ppThd, MosThreadPriority pri,
                          MosThreadEntry * pEntry, s32 arg, u32 stackSize);
/// Increment thread reference count. Every user of a shared thread handle should increment it.
/// Returns true if thread handle can be used.
bool mosIncThreadRefCount(MosThread ** ppThd);
/// Decrement reference count. If count goes to zero, deallocates thread resources.
///
bool mosDecThreadRefCount(MosThread ** ppThd);
/// Obtain a unique ID (for thread storage and TODO: other purposes)
///
MOS_ISR_SAFE u32 mosGetUniqueID(void);

// Thread Storage - allows libraries to store opaque data on a per-thread basis.

/// Set local storage for the current thread using a uniqueID for the given library.
///   Callback is invoked when thread resources are released.
///   Returns true on success.
bool mosSetThreadStorage(MosThread * pThread, u32 uniqueID, void * pData, MosThreadStorageReleaseFunc * pReleaseFunc);

/// Return pointer to thread local storage using the uniqueID, NULL if local storage under that ID doesn't exist.
///
void * mosGetThreadStorage(MosThread * pThread, u32 uniqueID);

#endif
