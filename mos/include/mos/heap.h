
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
    MosList  fl;
    u32      bytesFree;
    u32      minBytesFree;
    u16      alignMask;
    u16      flBlockCount;
    u16      minBlockSize;
} MosHeap;

void MosInitHeap(MosHeap * pHeap, u8 * pData, u32 heapSize, u32 alignment);
void * MosAlloc(MosHeap * pHeap, u32 size);
void * MosReAlloc(MosHeap * pHeap, void * pBlock, u32 newSize);
void * MosReAllocWithoutCopy(MosHeap * pHeap, void * pBlock, u32 newSize);
void MosFree(MosHeap * pHeap, void * pBlock);

#endif
