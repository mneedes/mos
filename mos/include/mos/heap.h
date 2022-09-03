
// Copyright 2019-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/heap.h
/// \brief
/// The MOS allocator is an efficient first-fit memory allocator.
///
/// It may be configured for power-of-2 block alignment and supports
/// allocation from multiple non-contiguous memory pools. It provides
/// rudimentary error detection for block over/underrun and multiple
/// frees (via MosAssert()). This implementation has been extensively
/// tested using randomly generated test vectors.

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
void MosInitHeap(MosHeap * pHeap, u16 alignment, u8 * pPool, u32 poolSize);
/// Add memory pool to initialized heap
///
void MosAddHeapPool(MosHeap * pHeap, u8 * pPool, u32 poolSize);
/// Allocate a block from the heap of a given size.
/// Returns NULL on failure.
void * MosAlloc(MosHeap * pHeap, u32 size);
/// Reallocate a block from the heap to the given new size, preserving existing
//  data (truncation occurs only if size decreases). If call fails existing block will
//  NOT be freed and NULL will be returned.
void * MosReAlloc(MosHeap * pHeap, void * pBlock, u32 newSize);
/// TODO: Reallocate a block from the heap to the given size, only guaranteeing data
/// preservation if the block size is decreased. If call fails existing block will NOT
/// be freed and NULL will be returned.
void * MosExchangeBlock(MosHeap * pHeap, void * pBlock, u32 newSize);
/// Return block back to the heap.
///
void MosFree(MosHeap * pHeap, void * pBlock);

#endif
