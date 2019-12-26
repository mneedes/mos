
//  Copyright 2019 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#include "mos.h"
#include "mosh.h"

#define MOSH_BLOCK_ID          0xe711dead
#define MOSH_ODD_BLOCK_ID      0xba5eba11
#define MOSH_SL_ID             0x7331e711

// MoshLink size should ensure MOSH_HEAP_ALIGNED alignment
typedef struct {
    u32 id_canary;
    union {
        MoshBlockSize * bss;
        u32 bs;
    };
    MosList fl;
} MoshLink;

void MoshInitHeap(MoshHeap * heap, u8 * pit, u32 len) {
    heap->pit = pit;
    heap->bot = pit + len - sizeof(MoshLink);
    MosInitList(&heap->bsl);
    MosInitList(&heap->bsl_free);
    MosInitList(&heap->osl);
    MosInitList(&heap->sl);
    for (u32 ix = 0; ix < MOSH_MAX_HEAP_BLOCK_SIZES; ix++) {
        heap->bs[ix].bs = 0;
        MosInitList(&heap->bs[ix].fl);
        MosAddToList(&heap->bsl_free, &heap->bs[ix].bsl);
    }
    heap->max_bs = 0;
    MosInitMutex(&heap->mtx);
}

bool MoshReserveBlockSize(MoshHeap * heap, u32 bs) {
    bool success = false;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    if (!MosIsListEmpty(&heap->bsl_free)) {
        MoshBlockSize * new_bss = container_of(heap->bsl_free.next,
                                               MoshBlockSize, bsl);
        MosRemoveFromList(&new_bss->bsl);
        new_bss->bs = bs;
        MosList * elm;
        // Insertion sort into reserved block sizes
        for (elm = heap->bsl.next; elm != &heap->bsl; elm = elm->next) {
            if (container_of(elm, MoshBlockSize, bsl)->bs >= bs) break;
        }
        MosAddToListBefore(elm, &new_bss->bsl);
        if (bs > heap->max_bs) heap->max_bs = bs;
        success = true;
    }
    MosGiveMutex(&heap->mtx);
    return success;
}

void * MoshAlloc(MoshHeap * heap, u32 bs) {
    if (bs > heap->max_bs) return MoshAllocOddBlock(heap, bs);
    return MoshAllocBlock(heap, bs);
}

void MoshFree(MoshHeap * heap, void * block) {
    MoshLink * link = (MoshLink *)((u8 *)block - sizeof(MoshLink));
    MosTakeMutex(&heap->mtx);
    switch (link->id_canary) {
    case MOSH_BLOCK_ID:
        MosAddToList(&link->bss->fl, &link->fl);
        break;
    case MOSH_ODD_BLOCK_ID:
        {
            MosList * elm;
            // Insertion sort into odd size free-list
            for (elm = heap->osl.next; elm != &heap->osl; elm = elm->next) {
                if (container_of(elm, MoshLink, fl)->bs >= link->bs) break;
            }
            MosAddToListBefore(elm, &link->fl);
        }
        break;
    case MOSH_SL_ID:
        if (heap->sl.next == &link->fl) {
            heap->bot += (link->bs + sizeof(MoshLink));
        } else {
            MoshLink * prev = container_of(link->fl.prev, MoshLink, fl);
            prev->bs += (link->bs + sizeof(MoshLink));
        }
        MosRemoveFromList(&link->fl);
        break;
    default:
        // Error condition
        break;
    }
    MosGiveMutex(&heap->mtx);
}

void * MoshAllocBlock(MoshHeap * heap, u32 size) {
    u8 * block = NULL;
    MosTakeMutex(&heap->mtx);
    if (!MosIsListEmpty(&heap->bsl)) {
        // Find the smallest block size to accommodate request
        MosList * elm;
        for (elm = heap->bsl.next; elm != &heap->bsl; elm = elm->next) {
            if (container_of(elm, MoshBlockSize, bsl)->bs >= size) {
                // Allocate free block if available...
                MoshBlockSize * bss = container_of(elm, MoshBlockSize, bsl);
                if (!MosIsListEmpty(&bss->fl)) {
                    MoshLink * link = container_of(bss->fl.next, MoshLink, fl);
                    MosRemoveFromList(bss->fl.next);
                    block = (u8 *)link + sizeof(MoshLink);
                    break;
                } else if (heap->pit + bss->bs < heap->bot) {
                    // ...allocate new block from pit since there is room
                    MoshLink * link = (MoshLink *)heap->pit;
                    link->bss = bss;
                    heap->pit += (bss->bs + sizeof(MoshLink));
                    link->id_canary = MOSH_BLOCK_ID;
                    block = (u8 *)link + sizeof(MoshLink);
                    break;
                }
                // continue to see if next higher block size is available
            }
        }
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}

void * MoshAllocOddBlock(MoshHeap * heap, u32 bs) {
    MoshLink * link = NULL;
    MoshLink * link_found = NULL;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    // Find smallest available block in odd block list that accommodates size
    for (MosList * elm = heap->osl.next; elm != &heap->osl; elm = elm->next) {
        if (container_of(elm, MoshLink, fl)->bs >= bs) {
            link_found = container_of(elm, MoshLink, fl);
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
            link = (MoshLink *)heap->pit;
            link->bs = bs;
            heap->pit += (bs + sizeof(MoshLink));
            link->id_canary = MOSH_ODD_BLOCK_ID;
        } else if (link_found) {
            // If cannot allocate from pit use original block (if found)
            MosRemoveFromList(&link_found->fl);
            link = link_found;
        }
    }
    MosGiveMutex(&heap->mtx);
    if (!link) return NULL;
    return (void *)((u8 *)link + sizeof(MoshLink));
}

void * MoshAllocShortLived(MoshHeap * heap, u32 bs) {
    u8 * block = NULL;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    if (heap->pit + bs < heap->bot) {
        heap->bot -= (bs + sizeof(MoshLink));
        MoshLink * link = (MoshLink *)heap->bot;
        link->bs = bs;
        MosAddToListAfter(&heap->sl, &link->fl);
        link->id_canary = MOSH_SL_ID;
        block = (u8 *)link + sizeof(MoshLink);
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}
