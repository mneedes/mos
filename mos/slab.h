
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#ifndef _MOS_SLAB_H
#define _MOS_SLAB_H

#include <mos/heap.h>

// Application is responsible for maintaining enough free slabs for
//   anticipated ISR usage.

typedef struct {
    MosList part_q;
    MosList free_q;
    MosList full_q;
    u32 avail_blocks;
    MosHeap * heap;
    u32 block_size;
    u32 slab_size;
    u16 blocks_per_slab;
    u16 align_mask;
} MosPool;

void MosInitPool(MosPool * pool, MosHeap * heap, u32 blocks_per_slab,
                 u32 block_size, u16 alignment);
u32 MosAddSlabsToPool(MosPool * pool, u32 max_to_add);
u32 MosFreeUnallocatedSlabs(MosPool * pool, u32 max_to_remove);

MOS_ISR_SAFE void * MosAllocFromSlab(MosPool * pool);
MOS_ISR_SAFE void MosFreeToSlab(MosPool * pool, void * block);

#endif
