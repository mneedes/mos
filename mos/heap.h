
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS Heap Allocation Methods:
//  1. MosReserveBlockSize() / MosAllocBlock() / MosFree()
//     Allocation from a set of reserved block sizes.  Once a block has been
//     allocated using this method it can be returned and reallocated but its
//     size cannot change.  This method is deterministic.
//  2. MosAllocOddBlock() / MosFree()
//     Allocation of objects outside of the reserved sizes.  Once a
//     block has been allocated using this method it can be returned and
//     reallocated but its size cannot change.  Latency here scales with the
//     number of unallocated objects and therefore is generally not
//     deterministic.
//  3. MosAlloc() / MosFree()
//     Automatic allocation.  Uses method (1) if requested size fits in a
//     reserved block size.  Uses method (2) if the size is larger than any
//     reserved block size.
//  4. MosAllocShortLived() / MosFree()
//     Allocation of short-lived data (any size).  These blocks should
//     be returned "within a tick or two" to prevent fragmentation. These
//     blocks can be of any size.

//  All block sizes must be reserved prior to using heap

#ifndef _MOS_HEAP_H_
#define _MOS_HEAP_H_

#include "mos/kernel.h"

#define MOS_HEAP_ALIGNED      MOS_STACK_ALIGNED
#define MOS_HEAP_ALIGNMENT    MOS_STACK_ALIGNMENT

typedef struct {
    MosMutex mtx;
    u8 * pit;
    u8 * bot;
    MosList bsl;      // block size list
    MosList bsl_free; // block descriptor free list
    MosList osl;      // odd size list
    MosList sl;       // short-lived list
    u32 max_bs;       // Largest block size
} MosHeap;

// Initialize heap with maximum number of reserved block sizes (nbs)
// Pit shall be aligned to MOSH_HEAP_ALIGNMENT.
void MosInitHeap(MosHeap * heap, u8 * pit, u32 size, u8 nbs);
bool MosReserveBlockSize(MosHeap * heap, u32 bs);

void * MosAlloc(MosHeap * heap, u32 size);
void MosFree(MosHeap * heap, void * block);

void * MosAllocBlock(MosHeap * heap, u32 size);
void * MosAllocOddBlock(MosHeap * heap, u32 size);
void * MosAllocShortLived(MosHeap * heap, u32 size);

#endif
