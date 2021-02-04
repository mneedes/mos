
//  Copyright 2019-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS Heap Allocation Methods:
//  TODO
//

#ifndef _MOS_HEAP_H_
#define _MOS_HEAP_H_

#include <mos/kernel.h>

typedef struct {
    MosMutex mtx;
    MosList fl;
    u32 bytes_free;
    u16 align_mask;
    u16 fl_block_cnt;
    u16 min_block_size;
    u16 double_free_cnt;
    u16 dead_canary_cnt;
} MosHeap;

void MosInitHeap(MosHeap * heap, u8 * data, u32 heap_size, u32 alignment);
void * MosAlloc(MosHeap * heap, u32 size);
void * MosReAlloc(MosHeap * heap, void * block, u32 new_size);
void MosFree(MosHeap * heap, void * block);

#if 0

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
void * MosReAlloc(MosHeap * heap, void * block, u32 new_size);
void MosFree(MosHeap * heap, void * block);

void * MosAllocBlock(MosHeap * heap, u32 size);
void * MosAllocOddBlock(MosHeap * heap, u32 size);
void * MosAllocShortLived(MosHeap * heap, u32 size);
#endif

#endif

