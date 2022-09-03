
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/heap.h>

/*
 * MOS General Purpose Allocator
 *
 * Allocated blocks:
 *
 *  |canary|size_p| size |      payload       |canary|size_p| size...
 *                       ^                    ^
 *                   alignment                +- next block
 *
 *    Actual alignment = max(requested alignment, pointer size)
 *    Implicit link sizes = sizeof(payload) + sizeof(Link)
 *    LSB of size/size_p gets set to indicate block is allocated
 *
 * Freed block:
 *
 *  |canary|size_p| size | (free-list link)  payload  |...
 *                       ^         ^                  ^
 *                   alignment     |                  +- next block
 *                                 |
 *   Payload of freed block contains explicit link
 */

// TODO: place frags in power of 2 bins
// TODO: Deterministic allocations using power of 2 bins.

enum {
    HEAP_CANARY_VALUE = 0xe711dead,
    MIN_PAYLOAD_SIZE  = sizeof(MosList)
};

typedef struct {
    u32 canary;
    u32 size_p;
    u32 size;
} Link;

typedef struct {
    Link link;
    union { // <--  Alignment guaranteed here
        MosLink fl_link;
        u8 payload[0];
    };
} Block;

MOS_STATIC_ASSERT(Link, sizeof(Link) == 12);
MOS_STATIC_ASSERT(Block, sizeof(Block) == 20);

void MosInitHeap(MosHeap * heap, u8 * _bot, u32 heap_size, u32 alignment) {
    // Alignment must be a power of 2, and at a minimum should be
    //   the pointer size. Smallest block must fit a Link, the free-list
    //   link and satisfy alignment requirements of payload.
    alignment = (alignment > sizeof(void *)) ? alignment : sizeof(void *);
    heap->align_mask = alignment - 1;
    MosAssert((alignment & heap->align_mask) == 0);
    heap->min_block_size = MOS_ALIGN32(MIN_PAYLOAD_SIZE + sizeof(Link), heap->align_mask);
    // Initialize implicit links
    Link * ptr  = (Link *) MOS_ALIGN_PTR(_bot + sizeof(Link), heap->align_mask);
    Block * bot = (Block *) (ptr - 1);
    ptr         = (Link *) MOS_ALIGN_PTR_DOWN(_bot + heap_size, heap->align_mask);
    Block * top = (Block *) (ptr - 1);
    bot->link.canary = HEAP_CANARY_VALUE;
    bot->link.size_p = 0x1;
    bot->link.size   = (u8 *) top - (u8 *) bot;
    top->link.canary = HEAP_CANARY_VALUE;
    top->link.size_p = bot->link.size;
    top->link.size   = 0x1;
    // Free counters
    heap->bytes_free     = bot->link.size;
    heap->min_bytes_free = bot->link.size;
    // Initialize free-list (explicit links)
    MosInitList(&heap->fl);
    MosAddToList(&heap->fl, &bot->fl_link);
    heap->fl_block_cnt = 1;
    MosInitMutex(&heap->mtx);
}

void * MosAlloc(MosHeap * heap, u32 size) {
    if (size < MIN_PAYLOAD_SIZE) size = MIN_PAYLOAD_SIZE;
    size = MOS_ALIGN32(size + sizeof(Link), heap->align_mask);
    MosLockMutex(&heap->mtx);
    Block * block;
    {
        // First-fit search: max(min_size, size) + link_size needs to fit
        MosList * elm;
        for (elm = heap->fl.pNext; elm != &heap->fl; elm = elm->pNext) {
            block = container_of(elm, Block, fl_link);
            if (block->link.size >= size) {
                MosAssert(block->link.canary == HEAP_CANARY_VALUE);
                break;
            }
        }
        if (elm == &heap->fl) {
            MosUnlockMutex(&heap->mtx);
            return NULL;
        }
        // Remove chosen block from free-list
        MosRemoveFromList(elm);
    }
    Block * next_block = (Block *) ((u8 *) block + block->link.size);
    // Split the block if there is enough room left for a block of minimum size
    if (block->link.size >= size + heap->min_block_size) {
        u32 next_block_size = block->link.size - size;
        next_block->link.size_p = next_block_size;
        // The new next block
        next_block = (Block *) ((u8 *) block + size);
        next_block->link.canary = HEAP_CANARY_VALUE;
        next_block->link.size = next_block_size;
        // Set size and mark allocation bit
        next_block->link.size_p = size + 1;
        block->link.size = next_block->link.size_p;
        // Add new block to free-list
        MosAddToList(&heap->fl, &next_block->fl_link);
        heap->bytes_free -= size;
    } else {
        // Use existing block
        heap->fl_block_cnt -= 1;
        heap->bytes_free -= next_block->link.size_p;
        // Mark allocation bit
        next_block->link.size_p += 1;
        block->link.size = next_block->link.size_p;
    }
    if (heap->bytes_free < heap->min_bytes_free)
        heap->min_bytes_free = heap->bytes_free;
    MosUnlockMutex(&heap->mtx);
    return (void *) ((u8 *) block + sizeof(Link));
}

void * MosReAlloc(MosHeap * heap, void * _block, u32 _new_size) {
    if (!_block) return MosAlloc(heap, _new_size);
    MosLockMutex(&heap->mtx);
    Block * block = (Block *) ((u8 *) _block - sizeof(Link));
    // Check for canary value and double-free
    MosAssert(block->link.canary == HEAP_CANARY_VALUE);
    MosAssert(block->link.size & 0x1);
    u32 new_size = _new_size;
    if (new_size < MIN_PAYLOAD_SIZE) new_size = MIN_PAYLOAD_SIZE;
    new_size = MOS_ALIGN32(new_size + sizeof(Link), heap->align_mask);
    u32 avail = block->link.size - 1;
    Block * next_block = (Block *) ((u8 *) block + avail);
    if (!(next_block->link.size & 0x1)) {
        if (avail + next_block->link.size >= new_size) {
            avail += next_block->link.size;
            // Merge with next
            MosRemoveFromList(&next_block->fl_link);
            heap->fl_block_cnt -= 1;
            heap->bytes_free -= next_block->link.size;
            block->link.size += next_block->link.size;
            // The new next block
            next_block = (Block *) ((u8 *) block + block->link.size - 1);
            next_block->link.size_p = block->link.size;
        }
    }
    if (avail >= new_size + heap->min_block_size) {
        // Split
        u32 next_block_size = block->link.size - new_size - 1;
        heap->bytes_free += next_block_size;
        next_block->link.size_p = next_block_size;
        // The new next block
        next_block = (Block *) ((u8 *) block + new_size);
        next_block->link.canary = HEAP_CANARY_VALUE;
        next_block->link.size = next_block_size;
        // Set size and mark allocation bit
        next_block->link.size_p = new_size + 1;
        block->link.size = next_block->link.size_p;
        // Add new block to free-list
        MosAddToList(&heap->fl, &next_block->fl_link);
        heap->fl_block_cnt += 1;
    } else if (avail < new_size) {
        // Move
        u8 * new_block = MosAlloc(heap, _new_size);
        if (new_block) {
            u32 old_size = block->link.size - sizeof(Link) - 1;
            u32 copy_size = _new_size < old_size ? _new_size : old_size;
            for (u32 ix = 0; ix < copy_size; ix++)
                new_block[ix] = ((u8 *) _block)[ix];
            // Only free block if successful
            MosFree(heap, _block);
        }
        _block = new_block;
    }
    MosUnlockMutex(&heap->mtx);
    return _block;
}

void MosFree(MosHeap * heap, void * _block) {
    if (!_block) return;
    Block * block = (Block *) ((u8 *) _block - sizeof(Link));
    MosLockMutex(&heap->mtx);
    // Check for canary value and double-free
    MosAssert(block->link.canary == HEAP_CANARY_VALUE);
    MosAssert(block->link.size & 0x1);
    // Unmark allocation bit
    block->link.size--;
    // Check next canary value
    Block * next = (Block *) ((u8 *) block + block->link.size);
    MosAssert(next->link.canary == HEAP_CANARY_VALUE);
    heap->bytes_free += block->link.size;
    // Find next and previous blocks and determine if allocated
    Block * prev = NULL;
    if (!(block->link.size_p & 0x1))
        prev = (Block *) ((u8 *) block - block->link.size_p);
    s32 size_increase = 0;
    if (!(next->link.size & 0x1)) {
        if (prev) {
            // Combine with previous and next
            size_increase += block->link.size + next->link.size;
            MosRemoveFromList(&prev->fl_link);
            MosRemoveFromList(&next->fl_link);
            block = prev;
            heap->fl_block_cnt -= 1;
        } else {
            // Combine with next
            size_increase += next->link.size;
            MosRemoveFromList(&next->fl_link);
        }
    } else if (prev) {
        // Combine with previous
        size_increase += block->link.size;
        MosRemoveFromList(&prev->fl_link);
        block = prev;
    } else {
        // No combination possible
        heap->fl_block_cnt += 1;
    }
    // Adjust implicit links
    block->link.size += size_increase;
    next = (Block *) ((u8 *) block + block->link.size);
    next->link.size_p = block->link.size;
    // Add block to free-list
    MosAddToFrontOfList(&heap->fl, &block->fl_link);
    MosUnlockMutex(&heap->mtx);
}
