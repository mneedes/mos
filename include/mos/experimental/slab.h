
// Copyright 2021-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#ifndef _MOS_SLAB_H
#define _MOS_SLAB_H

#include <mos/heap.h>

// Application is responsible for maintaining enough free slabs for
//   anticipated ISR usage.

typedef struct {
    MosList   partQ;
    MosList   freeQ;
    MosList   fullQ;
    u32       availBlocks;
    MosHeap * pHeap;
    u32       blockSize;
    u32       slabSize;
    u16       blocksPerSlab;
    u16       alignMask;
} MosPool;

void mosInitPool(MosPool * pPool, MosHeap * pHeap, u32 blocksPerSlab,
                 u32 blockSize, u16 alignment);
u32 mosAddSlabsToPool(MosPool * pPool, u32 maxToAdd);
u32 mosFreeUnallocatedSlabs(MosPool * pPool, u32 maxToRemove);

MOS_ISR_SAFE void * mosAllocFromSlab(MosPool * pPool);
MOS_ISR_SAFE void mosFreeToSlab(MosPool * pPool, void * pBlock);

#endif
