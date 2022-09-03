
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/heap.h
/// \brief MOS Heap

#ifndef _MOS_HEAP_H_
#define _MOS_HEAP_H_

#include <mos/kernel.h>

typedef struct {
    MosMutex mtx;
    MosList fl;
    u32 bytes_free;
    u32 min_bytes_free;
    u16 align_mask;
    u16 fl_block_cnt;
    u16 min_block_size;
} MosHeap;

void MosInitHeap(MosHeap * pHeap, u8 * pData, u32 heapSize, u32 alignment);
void * MosAlloc(MosHeap * pHeap, u32 size);
void * MosReAlloc(MosHeap * pHeap, void * pBlock, u32 newSize);
void MosFree(MosHeap * pHeap, void * pBlock);

#endif
