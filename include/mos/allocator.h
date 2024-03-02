
// Copyright 2019-2024 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/allocator.h
///
/// Please see the applicable allocator C files for detailed
///   descriptions of the allocation algorithms.

#ifndef _MOS_ALLOCATOR_H_
#define _MOS_ALLOCATOR_H_

#include <mos/static_kernel.h>

typedef struct {
    MosMutex  mtx;
    MosList * pBins;
    u32       binMask;
    u32       bytesFree;
    u32       minBytesFree;
    u16       alignMask;
    u16       minBlockSize;
} MosHeap;

/// Callback function used for walking through allocations.
/// pBlock is NULL and tag is 0 for an unallocated blocks.
/// Default tag value is zero.
typedef void (mosHeapCallbackFunc)(void * pBlock, u16 tag, u32 size);

/// Initialize heap with the first memory pool
///
void mosInitHeap(MosHeap * pHeap, u16 alignment, u8 * pPool, u32 poolSize);

/// Add additional memory pool to heap
///
void mosAddHeapPool(MosHeap * pHeap, u8 * pPool, u32 poolSize);

/// Allocate a block from the heap of a given size.
/// Returns NULL on failure.
void * mosAlloc(MosHeap * pHeap, u32 size);

/// Reallocate a block from the heap to the given new size, preserving existing
/// data (truncation occurs only if size decreases). If call fails existing block will
//  NOT be freed and NULL will be returned. If pBlock is NULL, behavior is just like
//  mosAlloc, if newSize is zero, then the block is freed and NULL returned.
void * mosRealloc(MosHeap * pHeap, void * pBlock, u32 newSize);

/// Return block back to the heap.
///
void mosFree(MosHeap * pHeap, void * pBlock);

/// Set the debug tag of an already allocated block.
/// pBlock must have been returned by mosAlloc() or mosRealloc().
/// The default block tag is zero.
void mosTagAllocatedBlock(void * pBlock, u16 tag);

/// Walk through heap, invoking callback on each allocated block or unallocated chunk.
///
void mosWalkHeap(MosHeap * pHeap, mosHeapCallbackFunc * pCallbackFunc);

/// Get the max block size heap metric for evaluating fragmentation.
///
u32 mosGetBiggestAvailableChunk(MosHeap * pHeap);

#endif
