
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/heap.h
/// \brief
/// The MOS allocator is an efficient first-fit memory allocator.
///
/// It may be configured for power-of-2 block alignment and supports
/// allocation from multiple non-contiguous memory pools. It provides
/// rudimentary detection of block overrun and multiple frees.
//  This implementation has been extensively tested using random
//  test vectors.

#ifndef _MOS_HEAP_H_
#define _MOS_HEAP_H_

#include <mos/kernel.h>

typedef struct {
    MosMutex mtx;
    MosList  fl;
    u32      bytesFree;
    u32      minBytesFree;
    u16      alignMask;
    u16      flBlockCount;
    u16      minBlockSize;
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
/// TODO: Reallocate a block from the heap to the given size, only guaranteeing data
/// preservation if the block size is the same or smaller. If call fails existing block will NOT
/// be freed and NULL will be returned.
void * mosExchangeBlock(MosHeap * pHeap, void * pBlock, u32 newSize);
/// Return block back to the heap.
///
void mosFree(MosHeap * pHeap, void * pBlock);

#endif