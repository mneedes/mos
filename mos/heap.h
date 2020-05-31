
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOSH - MOS Heap Allocation Methods:
//  1. MoshReserveBlockSize() / MoshAllocBlock() / MoshFree()
//     Allocation from a set of reserved block sizes.    Once a block has been
//     allocated using this method it can be returned and reallocated but its
//     size cannot change.  This method is deterministic.
//  2. MoshAllocOddBlock() / MoshFree()
//     Allocation of objects outside of the reserved sizes.  Once a
//     block has been allocated using this method it can be returned and
//     reallocated but its size cannot change.  Latency here scales with the
//     number of unallocated objects and therefore is generally not
//     deterministic.
//  3. MoshAlloc() / MoshFree()
//     Automatic allocation.  Uses method (1) if requested size fits in a
//     reserved block size.  Uses method (2) if the size is larger than any
//     reserved block size.
//  4. MoshAllocShortLived() / MoshFree()
//     Allocation of short-lived data (any size).  These blocks should
//     be returned "within a tick or two" to prevent fragmentation. These
//     blocks can be of any size.

#ifndef _MOS_HEAP_H_
#define _MOS_HEAP_H_

#include "mos/kernel.h"

#define MOSH_HEAP_ALIGNED      MOS_STACK_ALIGNED
#define MOSH_HEAP_ALIGNMENT    MOS_STACK_ALIGNMENT

typedef struct {
    MosMutex mtx;
    u8 * pit;
    u8 * bot;
    MosList bsl;      // block size list
    MosList bsl_free; // block descriptor free list
    MosList osl;      // odd size list
    MosList sl;       // short-lived list
    u32 max_bs;       // Largest block size
} MoshHeap;

// Initialize heap with maximum number of reserved block sizes (nbs)
// Pit shall be aligned to MOSH_HEAP_ALIGNMENT.
void MoshInitHeap(MoshHeap * heap, u8 * pit, u32 size, u8 nbs);
bool MoshReserveBlockSize(MoshHeap * heap, u32 bs);

void * MoshAlloc(MoshHeap * heap, u32 size);
void MoshFree(MoshHeap * heap, void * block);

void * MoshAllocBlock(MoshHeap * heap, u32 size);
void * MoshAllocOddBlock(MoshHeap * heap, u32 size);
void * MoshAllocShortLived(MoshHeap * heap, u32 size);

#endif
