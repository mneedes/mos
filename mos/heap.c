
//  Copyright 2019-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#include <mos/heap.h>

/*
 * MOS General Purpose Allocator
 *
 * Allocated blocks:
 *
 *  |canary|size_p| size |      payload       |canary|size_p| size...
 *                       ^                    ^
 *                   alignment                +-  NEXT BLOCK
 *
 *    Actual alignment = max(requested alignment, pointer size)
 *    Implicit link sizes = sizeof(payload) + sizeof(Link)
 *    LSB of size/size_p gets set to indicate block is allocated
 *
 * Freed block:
 *
 *  |canary|size_p| size | (free-list link)  payload  |...
 *                       ^         ^                  ^
 *                   alignment     |                  +- NEXT BLOCK
 *                                 |
 *   Payload of freed block contains explicit link
 *
 */

enum {
    HEAP_CANARY_VALUE = 0xE711DEAD,
    MIN_PAYLOAD_SIZE  = sizeof(MosList)
};

typedef struct __attribute__((packed)) {
    u32 canary;
    u32 size_p;
    u32 size;
} Link;

typedef struct __attribute__((packed)) {
    Link link;
    union { // <--  Alignment guaranteed here
        MosList fl_e;
        u8 payload[0];
    };
} Block;

void MosInitHeap(MosHeap * heap, u8 * _bot, u32 heap_size, u32 alignment) {
    // Alignment must be a power of 2, and at a minimum should be
    //   the pointer size. Smallest block must fit a Link, the free-list
    //   link and satisfy alignment requirements of payload.
    alignment = (alignment > sizeof(void *)) ? alignment : sizeof(void *);
    heap->align_mask = alignment - 1;
    MosAssert((alignment & heap->align_mask) == 0);
    heap->min_block_size = MOS_ALIGN32(MIN_PAYLOAD_SIZE + sizeof(Link), alignment - 1);
    // Initialize implicit links
    Link * ptr  = (Link *) MOS_ALIGN_PTR(_bot + sizeof(Link), alignment - 1);
    Block * bot = (Block *) (ptr - 1);
    ptr         = (Link *) MOS_ALIGN_PTR_DOWN(_bot + heap_size, alignment - 1);
    Block * top = (Block *) (ptr - 1);
    bot->link.canary = HEAP_CANARY_VALUE;
    bot->link.size_p = 0x1;
    bot->link.size   = (u8 *) top - (u8 *) bot;
    top->link.canary = HEAP_CANARY_VALUE;
    top->link.size_p = bot->link.size;
    top->link.size   = 0x1;
    heap->bytes_free = bot->link.size;
    // Initialize free-list (explicit links)
    MosInitList(&bot->fl_e);
    MosInitList(&heap->fl);
    MosAddToList(&heap->fl, &bot->fl_e);
    heap->fl_block_cnt = 1;
    // Initialize error counters
    heap->double_free_cnt = 0;
    heap->dead_canary_cnt = 0;
    MosInitMutex(&heap->mtx);
}

void * MosAlloc(MosHeap * heap, u32 size) {
    if (size < MIN_PAYLOAD_SIZE) size = MIN_PAYLOAD_SIZE;
    size = MOS_ALIGN32(size + sizeof(Link), heap->align_mask);
    MosTakeMutex(&heap->mtx);
    Block * block;
    {
        // First-fit search: (max(min_size, size) + link_size) needs to fit
        MosList * elm;
        for (elm = heap->fl.next; elm != &heap->fl; elm = elm->next) {
            block = container_of(elm, Block, fl_e);
            if (block->link.size >= size) {
                if (block->link.canary != HEAP_CANARY_VALUE)
                    heap->dead_canary_cnt++;
                else break;
            }
        }
        if (elm == &heap->fl) {
            MosGiveMutex(&heap->mtx);
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
        heap->bytes_free -= size;
        // Set size and mark allocation bit
        next_block->link.size_p = size + 1;
        block->link.size = next_block->link.size_p;
        // Add new block to free-list
        MosInitList(&next_block->fl_e);
        MosAddToList(&heap->fl, &next_block->fl_e);
    } else {
        // Use existing block
        heap->fl_block_cnt -= 1;
        heap->bytes_free -= next_block->link.size_p;
        // Mark allocation bit
        next_block->link.size_p += 1;
        block->link.size = next_block->link.size_p;
    }
    MosGiveMutex(&heap->mtx);
    return (void *) ((u8 *) block + sizeof(Link));
}

void * MosReAlloc(MosHeap * heap, void * _block, u32 _new_size) {
    if (!_block) return MosAlloc(heap, _new_size);
    MosTakeMutex(&heap->mtx);
    Block * block = (Block *) ((u8 *) _block - sizeof(Link));
    // Check for canary value and double-free
    if (block->link.canary != HEAP_CANARY_VALUE) {
        heap->dead_canary_cnt++;
        MosGiveMutex(&heap->mtx);
        return NULL;
    }
    if (!(block->link.size & 0x1)) {
        heap->double_free_cnt++;
        MosGiveMutex(&heap->mtx);
        return NULL;
    }
    u32 old_copy_size = block->link.size - sizeof(Link);
    u8 * new_block = MosAlloc(heap, _new_size);
    if (!new_block) {
        MosFree(heap, _block);
        MosGiveMutex(&heap->mtx);
        return NULL;
    }
    u32 copy_size = _new_size < old_copy_size ? _new_size : old_copy_size;
    for (u32 ix = 0; ix < copy_size; ix++)
        new_block[ix] = ((u8 *) _block)[ix];
    MosFree(heap, _block);
    MosGiveMutex(&heap->mtx);
    return new_block;
}

#if 0

// TODO: Work on higher performance ReAlloc ...
//   The following is WIP

void * MosReAlloc(MosHeap * heap, void * _block, u32 _new_size) {
    if (!_block) return MosAlloc(heap, _new_size);
    u32 new_size = align_size(_new_size + sizeof(Link), heap->alignment);
    if (new_size < MIN_PAYLOAD_SIZE) new_size = MIN_PAYLOAD_SIZE;
    Block * block = (Block *) ((u8 *) _block - sizeof(Link));
    // Check magic number and detect double-free
    if (block->link.canary != HEAP_CANARY_VALUE) {
        heap->dead_canary_cnt++;
        return NULL;
    }
    if (!(block->link.size & 0x1)) {
        heap->double_free_cnt++;
        return NULL;
    }
    // TODO: Better realloc
    Block * next_block = (Block *) ((u8 *) block + block->link.size);
    u32 old_size = block->link.size - 1;
    if (old_size < new_size) {
        printf("RE NEW\n");
        MosFree(heap, _block);
        void * _block_new = MosAlloc(heap, _new_size);
        if (!_block_new) return NULL;
        if (_block_new != _block) {
            // TODO: Copy value from old to new
        }
    } else if (old_size >= new_size + heap->min_block_size) {
        printf("RE SPLIT\n");
        // NEXT BLOCK MIGHT BE FREE , join?
        // Split block
        u32 next_block_size = old_size - new_size;
        next_block->link.size_p = next_block_size;
        // The new next block
        next_block = (Block *) ((u8 *) block + new_size);
        next_block->link.size = next_block_size;
        heap->bytes_free -= new_size;
        // Set size and mark allocation bit
        next_block->link.size_p = new_size + 1;
        block->link.size = next_block->link.size_p;
        // Add new block to free-list
        MosInitList(&next_block->fl_e);
        MosAddToList(&heap->fl, &next_block->fl_e);
    } else {
        // Use current block
        printf("RE UC\n");
        MosDumpFreeList(heap);
        return _block;
    }
    MosDumpFreeList(heap);
}
#endif

void MosFree(MosHeap * heap, void * _block) {
    if (!_block) return;
    Block * block = (Block *) ((u8 *) _block - sizeof(Link));
    MosTakeMutex(&heap->mtx);
    // Check for canary value and double-free
    if (block->link.canary != HEAP_CANARY_VALUE) {
        heap->dead_canary_cnt++;
        MosGiveMutex(&heap->mtx);
        return;
    }
    if (!(block->link.size & 0x1)) {
        heap->double_free_cnt++;
        MosGiveMutex(&heap->mtx);
        return;
    }
    // Unmark allocation bit
    block->link.size--;
    // Check next canary value
    Block * next = (Block *) ((u8 *) block + block->link.size);
    if (next->link.canary != HEAP_CANARY_VALUE) {
        heap->dead_canary_cnt++;
        MosGiveMutex(&heap->mtx);
        return;
    }
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
            MosRemoveFromList(&prev->fl_e);
            MosRemoveFromList(&next->fl_e);
            block = prev;
            heap->fl_block_cnt -= 1;
        } else {
            // Combine with next
            size_increase += next->link.size;
            MosRemoveFromList(&next->fl_e);
            MosInitList(&block->fl_e);
        }
    } else if (prev) {
        // Combine with previous
        size_increase += block->link.size;
        MosRemoveFromList(&prev->fl_e);
        block = prev;
    } else {
        // No combination possible
        MosInitList(&block->fl_e);
        heap->fl_block_cnt += 1;
    }
    // Adjust Links
    block->link.size += size_increase;
    next = (Block *) ((u8 *) block + block->link.size);
    next->link.size_p = block->link.size;
    // Add block to free-list
    MosAddToFrontOfList(&heap->fl, &block->fl_e);
    MosGiveMutex(&heap->mtx);
}

#if 0

#include <string.h>

enum {
    STD_BLOCK_ID = 0xe711dead,
    ODD_BLOCK_ID = 0xba5eba11,
    SL_BLOCK_ID  = 0x7331e711
};

// MosBlockDesc and MosLink shall be sized to guarantee
//   MOS_HEAP_ALIGNED alignment.

typedef struct {
    u32 bs;
    u32 pad;       // TODO: Remove pad and then align
    MosList fl;
    MosList bsl;
} MosBlockDesc;

typedef struct {
    u32 id_canary;
    union {
        MosBlockDesc * bd;
        u32 bs;
    };
    MosList fl;
} MosLink;

void MosInitHeap(MosHeap * heap, u8 * pit, u32 size, u8 nbs) {
    MosInitList(&heap->bsl);
    MosInitList(&heap->bsl_free);
    MosInitList(&heap->osl);
    MosInitList(&heap->sl);
    // Allocate block descriptors from the pit
    MosBlockDesc * bd = (MosBlockDesc *) pit;
    for (u32 ix = 0; ix < nbs; ix++) {
        bd->bs = 0;
        MosInitList(&bd->fl);
        MosAddToList(&heap->bsl_free, &bd->bsl);
        bd++;
    }
    heap->pit = (u8 *) bd;
    heap->bot = heap->pit + size;
    heap->bot -= (nbs * sizeof(MosBlockDesc) + sizeof(MosLink));
    heap->max_bs = 0;
    MosInitMutex(&heap->mtx);
}

bool MosReserveBlockSize(MosHeap * heap, u32 bs) {
    bool success = false;
    // Round bs to next aligned value
    if (bs % MOS_HEAP_ALIGNMENT)
        bs += (MOS_HEAP_ALIGNMENT - (bs % MOS_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    if (!MosIsListEmpty(&heap->bsl_free)) {
        MosBlockDesc * new_bd = container_of(heap->bsl_free.next,
                                             MosBlockDesc, bsl);
        MosRemoveFromList(&new_bd->bsl);
        new_bd->bs = bs;
        MosList * elm;
        // Insertion sort into reserved block sizes
        for (elm = heap->bsl.next; elm != &heap->bsl; elm = elm->next) {
            if (container_of(elm, MosBlockDesc, bsl)->bs >= bs) break;
        }
        MosAddToListBefore(elm, &new_bd->bsl);
        if (bs > heap->max_bs) heap->max_bs = bs;
        success = true;
    }
    MosGiveMutex(&heap->mtx);
    return success;
}

void * MosAlloc(MosHeap * heap, u32 bs) {
    void * block;
    MosTakeMutex(&heap->mtx);
    if (bs > heap->max_bs) block = MosAllocOddBlock(heap, bs);
    else block = MosAllocBlock(heap, bs);
    MosGiveMutex(&heap->mtx);
    return block;
}

void * MosReAlloc(MosHeap * heap, void * cur_block, u32 bs) {
    if (cur_block == NULL) return MosAlloc(heap, bs);
    MosLink * cur_link = (MosLink *)((u8 *)cur_block - sizeof(MosLink));
    MosTakeMutex(&heap->mtx);
    u32 cur_bs = 0;
    switch (cur_link->id_canary) {
        case STD_BLOCK_ID:
            cur_bs = cur_link->bd->bs;
            break;
        case ODD_BLOCK_ID:
            cur_bs = cur_link->bs;
            break;
        default:
            // MosReAlloc does not support SL blocks
            MosGiveMutex(&heap->mtx);
            return NULL;
    }
    void * block = NULL;
    if (bs > heap->max_bs) {
        // Move to new odd block if larger than current block,
        //   or smaller than half of current block
        if (bs > cur_bs || 2*bs < cur_bs) {
            block = MosAllocOddBlock(heap, bs);
            MosFree(heap, cur_block);
        } else {
            block = cur_block;
        }
    } else {
        // If current block best accommodates new size, it will
        //   be retrieved again on new allocation.  If bs is zero,
        //   cur_block is freed only.
        MosFree(heap, cur_block);
        block = MosAllocBlock(heap, bs);
    }
    // Copy contents and free old block if block changed
    if (block && block != cur_block) {
        memcpy(block, cur_block, (cur_bs < bs) ? cur_bs : bs);
    }
    MosGiveMutex(&heap->mtx);
    return block;
}

void MosFree(MosHeap * heap, void * block) {
    MosLink * link = (MosLink *)((u8 *)block - sizeof(MosLink));
    MosTakeMutex(&heap->mtx);
    switch (link->id_canary) {
    case STD_BLOCK_ID:
        // Double-free protection, plus put on front of list so realloc
        //  will grab it first if it is still the best fit.
        if (MosIsListEmpty(&link->fl))
            MosAddToFrontOfList(&link->bd->fl, &link->fl);
        break;
    case ODD_BLOCK_ID:
        // Double-free protection
        if (MosIsListEmpty(&link->fl)) {
            MosList * elm;
            // Insertion sort into odd size free-list
            for (elm = heap->osl.next; elm != &heap->osl; elm = elm->next) {
                if (container_of(elm, MosLink, fl)->bs >= link->bs) break;
            }
            MosAddToListBefore(elm, &link->fl);
        }
        break;
    case SL_BLOCK_ID:
        // TODO: Short-lived blocks need double-free protection
        if (heap->sl.next == &link->fl) {
            heap->bot += (link->bs + sizeof(MosLink));
        } else {
            MosLink * prev = container_of(link->fl.prev, MosLink, fl);
            prev->bs += (link->bs + sizeof(MosLink));
        }
        MosRemoveFromList(&link->fl);
        break;
    default:
        // Error condition
        break;
    }
    MosGiveMutex(&heap->mtx);
}

void * MosAllocBlock(MosHeap * heap, u32 bs) {
    if (!bs) return NULL;
    u8 * block = NULL;
    MosTakeMutex(&heap->mtx);
    // Find the smallest block size to accommodate request
    MosList * elm;
    for (elm = heap->bsl.next; elm != &heap->bsl; elm = elm->next) {
        MosBlockDesc * bd = container_of(elm, MosBlockDesc, bsl);
        if (bd->bs >= bs) {
            // Allocate free block if available...
            if (!MosIsListEmpty(&bd->fl)) {
                MosLink * link = container_of(bd->fl.next, MosLink, fl);
                MosRemoveFromList(bd->fl.next);
                block = (u8 *) link + sizeof(MosLink);
                break;
            } else if (heap->pit + bd->bs < heap->bot) {
                // ...allocate new block from pit since there is room
                MosLink * link = (MosLink *)heap->pit;
                link->bd = bd;
                link->fl.prev = &link->fl;
                heap->pit += (bd->bs + sizeof(MosLink));
                link->id_canary = STD_BLOCK_ID;
                block = (u8 *) link + sizeof(MosLink);
                break;
            }
            // continue to see if next higher block size is available
        }
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}

void * MosAllocOddBlock(MosHeap * heap, u32 bs) {
    if (bs == 0) return NULL;
    MosLink * link = NULL;
    MosLink * link_found = NULL;
    // Round bs to next aligned value
    if (bs % MOS_HEAP_ALIGNMENT)
        bs += (MOS_HEAP_ALIGNMENT - (bs % MOS_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    // Find smallest available block in odd block list that accommodates size
    for (MosList * elm = heap->osl.next; elm != &heap->osl; elm = elm->next) {
        if (container_of(elm, MosLink, fl)->bs >= bs) {
            link_found = container_of(elm, MosLink, fl);
            break;
        }
    }
    // Use block if its size is less than twice requested block size.
    if (link_found && link_found->bs <= 2*bs) {
        MosRemoveFromList(&link_found->fl);
        link = link_found;
    } else {
        // Allocate new block from pit if there is room
        if (heap->pit + bs < heap->bot) {
            link = (MosLink *)heap->pit;
            link->bs = bs;
            link->fl.prev = &link->fl;
            heap->pit += (bs + sizeof(MosLink));
            link->id_canary = ODD_BLOCK_ID;
        } else if (link_found) {
            // If cannot allocate from pit use original block (if found)
            MosRemoveFromList(&link_found->fl);
            link = link_found;
        }
    }
    MosGiveMutex(&heap->mtx);
    if (!link) return NULL;
    return (void *)((u8 *)link + sizeof(MosLink));
}

void * MosAllocShortLived(MosHeap * heap, u32 bs) {
    if (bs == 0) return NULL;
    u8 * block = NULL;
    // Round bs to next aligned value
    if (bs % MOS_HEAP_ALIGNMENT)
        bs += (MOS_HEAP_ALIGNMENT - (bs % MOS_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    if (heap->pit + bs < heap->bot) {
        heap->bot -= (bs + sizeof(MosLink));
        MosLink * link = (MosLink *)heap->bot;
        link->bs = bs;
        MosAddToListAfter(&heap->sl, &link->fl);
        link->id_canary = SL_BLOCK_ID;
        block = (u8 *)link + sizeof(MosLink);
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}
#endif
