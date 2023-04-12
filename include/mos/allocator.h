
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/allocator.h
/// \brief
/// The MOS allocator is a simple deterministic best-effort memory allocator.
///
/// Pools may be configured to return blocks with power-of-2 memory alignment.
/// Includes support for allocation from multiple non-contiguous memory pools.
/// It provides rudimentary detection of block overrun and multiple frees.
/// This implementation has been extensively tested using random test vectors.

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

/// Initialize heap with an optional memory pool.
///  Set pPool to NULL to defer adding initial pool.
void mosInitHeap(MosHeap * pHeap, u16 alignment, u8 * pPool, u32 poolSize);
/// Add memory pool to initialized heap
///
void mosAddHeapPool(MosHeap * pHeap, u8 * pPool, u32 poolSize);
/// Allocate a block from the heap of a given size.
/// Returns NULL on failure.
void * mosAlloc(MosHeap * pHeap, u32 size);
/// Reallocate a block from the heap to the given new size, preserving existing
//  data (truncation occurs only if size decreases). If call fails existing block will
//  NOT be freed and NULL will be returned.
void * mosRealloc(MosHeap * pHeap, void * pBlock, u32 newSize);
/// Return block back to the heap.
///
void mosFree(MosHeap * pHeap, void * pBlock);
/// Get the max block size heap metric for evaluating fragmentation.
///
u32 mosGetBiggestAvailableChunk(MosHeap * pHeap);

#endif
