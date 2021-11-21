
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/slab.h>

/*
 * MOS Slab Allocator
 *
 * Slab layout:
 *
 *  |slab struct| align pad |block 1 ... block 2 ... |
 *
 * Allocated blocks:
 *
 *  |slab_ptr|         payload            |slab ptr...
 *           ^                            ^
 *       alignment                        +- next block
 *
 *    Actual alignment = max(requested alignment, pointer size)
 *
 * Freed block:
 *
 *  |slab ptr| (free-list link)           |slab ptr...
 *           ^                            ^
 *       alignment                        +- next block
 */

enum {
    MIN_PAYLOAD_SIZE = sizeof(MosList),
    MIN_ALIGNMENT = sizeof(u32)
};

typedef struct {
    MosList blk_q;
    MosLink slab_link;
    u32 avail_blocks;
    u8 align_pad_and_blocks[0];
} Slab;

typedef struct {
    Slab * slab;
    union { // <--  Alignment guaranteed here
        MosLink fl_link;
        u8 payload[0];
    };
} Block;

MOS_STATIC_ASSERT(Block, sizeof(Block) == 12);

void MosInitPool(MosPool * pool, MosHeap * heap, u32 blocks_per_slab, u32 block_size, u16 alignment) {
    // Alignment must be a power of 2, and at a minimum should be the pointer size.
    alignment = (alignment > sizeof(void *)) ? alignment : sizeof(void *);
    pool->align_mask = alignment - 1;
    MosAssert((alignment & pool->align_mask) == 0);
    // Actual block size has overhead bytes and minimum size
    if (block_size < MIN_PAYLOAD_SIZE) block_size = MIN_PAYLOAD_SIZE;
    block_size = MOS_ALIGN32(block_size + sizeof(Slab *), pool->align_mask);
    pool->block_size = block_size;
    u32 heap_alignment = heap->align_mask + 1;
    u32 max_align_offset = alignment;
    if (heap_alignment < alignment) {
        max_align_offset -= heap_alignment;
    } else max_align_offset -= MIN_ALIGNMENT;
    // Allocate over-sized slab to provide enough adjustment room to guarantee block alignment.
    pool->slab_size = sizeof(Slab) + max_align_offset + block_size * blocks_per_slab;
    MosAssert(blocks_per_slab > 0);
    pool->blocks_per_slab = blocks_per_slab;
    pool->avail_blocks = 0;
    pool->heap = heap;
    MosInitList(&pool->part_q);
    MosInitList(&pool->free_q);
    MosInitList(&pool->full_q);
}

u32 MosAddSlabsToPool(MosPool * pool, u32 max_to_add) {
    u32 slabs_added_cnt = 0;
    for (; slabs_added_cnt < max_to_add; slabs_added_cnt++) {
        // Allocate slab
        Slab * slab = MosAlloc(pool->heap, pool->slab_size);
        if (!slab) break;
        MosInitList(&slab->blk_q);
        MosInitList(&slab->slab_link);
        // Align here
        u8 * buf = (u8 *) slab + sizeof(Slab) + sizeof(Slab *);
        buf = (u8 *) MOS_ALIGN_PTR(buf, pool->align_mask) - sizeof(Slab *);
        // Link up blocks
        for (u32 ix = 0; ix < pool->blocks_per_slab; ix++) {
            Block * block = (Block *) buf;
            block->slab = slab;
            MosAddToList(&slab->blk_q, &block->fl_link);
            buf += pool->block_size;
        }
        slab->avail_blocks = pool->blocks_per_slab;
        _MosDisableInterrupts();
        MosAddToList(&pool->free_q, &slab->slab_link);
        pool->avail_blocks += pool->blocks_per_slab;
        _MosEnableInterrupts();
    }
    return slabs_added_cnt;
}

u32 MosFreeUnallocatedSlabs(MosPool * pool, u32 max_to_remove) {
    // Remove free slabs
    u32 slabs_removed_cnt = 0;
    while (1) {
        if (slabs_removed_cnt == max_to_remove) break;
        _MosDisableInterrupts();
        if (MosIsListEmpty(&pool->free_q)) {
            _MosEnableInterrupts();
            break;
        }
        pool->avail_blocks -= pool->blocks_per_slab;
        MosList * elm = pool->free_q.next;
        MosRemoveFromList(elm);
        _MosEnableInterrupts();
        MosFree(pool->heap, container_of(elm, Slab, slab_link));
        slabs_removed_cnt++;
    }
    return slabs_removed_cnt;
}

MOS_ISR_SAFE void * MosAllocFromSlab(MosPool * pool) {
    u32 mask = MosDisableInterrupts();
    if (pool->avail_blocks) {
        pool->avail_blocks--;
        Slab * slab;
        if (!MosIsListEmpty(&pool->part_q)) {
            slab = container_of(pool->part_q.next, Slab, slab_link);
            if (--slab->avail_blocks == 0) {
                MosRemoveFromList(&slab->slab_link);
                MosAddToList(&pool->full_q, &slab->slab_link);
            }
        } else {
            slab = container_of(pool->free_q.next, Slab, slab_link);
            slab->avail_blocks--;
            MosRemoveFromList(&slab->slab_link);
            MosAddToList(&pool->part_q, &slab->slab_link);
        }
        MosList * elm = slab->blk_q.next;
        MosRemoveFromList(elm);
        Block * block = container_of(elm, Block, fl_link);
        MosEnableInterrupts(mask);
        return &block->payload;
    }
    MosEnableInterrupts(mask);
    return NULL;
}

MOS_ISR_SAFE void MosFreeToSlab(MosPool * pool, void * _block) {
    Block * block = (Block *) ((u32 *) _block - 1);
    u32 mask = MosDisableInterrupts();
    pool->avail_blocks++;
    MosAddToList(&block->slab->blk_q, &block->fl_link);
    block->slab->avail_blocks++;
    if (block->slab->avail_blocks == 1) {
        MosRemoveFromList(&block->slab->slab_link);
        MosAddToList(&pool->part_q, &block->slab->slab_link);
    } else if (block->slab->avail_blocks == pool->blocks_per_slab) {
        MosRemoveFromList(&block->slab->slab_link);
        MosAddToList(&pool->free_q, &block->slab->slab_link);
    }
    MosEnableInterrupts(mask);
}
