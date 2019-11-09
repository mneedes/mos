
//  Copyright 2019 Matthew Christopher Needes
//  Licensed under the terms and conditions contained within the LICENSE
//  file (the "License") included under this distribution.  You may not use
//  this file except in compliance with the License.

//
// MOSH - MOS Heap
//   Provides 3 types of allocation:
//     1. Dynamic allocation for a set of reserved block sizes from the pit,
//         Once a fixed block has been allocated it can be returned and
//         reallocated but its size cannot change.
//     2. Static allocation--these blocks cannot be returned.  These blocks can
//         be of any size--i.e.: don't have to match a reserved block size.
//     3. Allocation of short-lived data (any size).  These blocks should
//         be returned "within a tick or two" to prevent fragmentation. These
//         blocks can be of any size--i.e.: don't have to match a reserved
//         block size.
//

#include "mos.h"
#include "mosh.h"

#define MOSH_CANARY_VALUE     0xe711dead

// MoshLink / MoshSLink sizes should ensure MOSH_HEAP_ALIGNED alignment
typedef struct {
    u32 canary_pad;
    MoshBlockSize * bs;
    MosList fl;
} MoshLink;

// Short-lived data link
typedef struct {
    u32 canary_pad;
    u32 bs;
    MosList sl;
} MoshSLink;

void MoshInitHeap(MoshHeap * heap, u8 * pit, u32 len) {
    heap->pit = pit;
    heap->bot = pit + len - sizeof(MoshLink);
    MosInitList(&heap->bsl);
    MosInitList(&heap->bsl_free);
    MosInitList(&heap->sl);
    for (u32 ix = 0; ix < MOSH_MAX_HEAP_BLOCK_SIZES; ix++) {
        heap->bs[ix].bs = 0;
        MosInitList(&heap->bs[ix].fl);
        MosAddToList(&heap->bsl_free, &heap->bs[ix].bsl);
    }
    MosInitMutex(&heap->mtx);
}

bool MoshReserveBlockSize(MoshHeap * heap, u32 bs) {
    bool success = false;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    if (!MosIsListEmpty(&heap->bsl_free)) {
        MoshBlockSize *new_bs = container_of(heap->bsl_free.next, MoshBlockSize, bsl);
        MosRemoveFromList(&new_bs->bsl);
        new_bs->bs = bs;
        MosList * elm;
        // Insertion sort
        for (elm = heap->bsl.next; elm != &heap->bsl; elm = elm->next) {
            if (container_of(elm, MoshBlockSize, bsl)->bs >= bs) break;
        }
        MosAddToListBefore(elm, &new_bs->bsl);
        success = true;
    }
    MosGiveMutex(&heap->mtx);
    return success;
}

void *MoshAllocBlock(MoshHeap * heap, u32 size) {
    u8 * block = NULL;
    MosTakeMutex(&heap->mtx);
    if (!MosIsListEmpty(&heap->bsl)) {
        // Find the smallest block size to accommodate request
        MosList * elm;
        for (elm = heap->bsl.next; elm != &heap->bsl; elm = elm->next) {
            if (container_of(elm, MoshBlockSize, bsl)->bs >= size) {
                // Allocate free block if available...
                MoshBlockSize *bs = container_of(elm, MoshBlockSize, bsl);
                if (!MosIsListEmpty(&bs->fl)) {
                    MoshLink *link = container_of(bs->fl.next, MoshLink, fl);
                    MosRemoveFromList(bs->fl.next);
                    link->canary_pad = MOSH_CANARY_VALUE;
                    block = (u8 *)link + sizeof(MoshLink);
                    break;
                } else if (heap->pit + bs->bs < heap->bot) {
                    // ...allocate new block from pit if there is room
                    MoshLink *link = (MoshLink *)heap->pit;
                    link->bs = bs;
                    heap->pit += (bs->bs + sizeof(MoshLink));
                    link->canary_pad = MOSH_CANARY_VALUE;
                    block = (u8 *)link + sizeof(MoshLink);
                    break;
                }
            }
        }
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}

void MoshFreeBlock(MoshHeap * heap, void * block) {
    MosTakeMutex(&heap->mtx);
    MoshLink * link = (MoshLink *)((u8 *)block - sizeof(MoshLink));
    if (link->bs != NULL) MosAddToList(&link->bs->fl, &link->fl);
    MosGiveMutex(&heap->mtx);
}

void *MoshAllocForever(MoshHeap * heap, u32 bs) {
    u8 * block = NULL;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    if (heap->pit + bs < heap->bot) {
        MoshLink * link = (MoshLink *)heap->pit;
        link->bs = NULL;
        heap->pit += (bs + sizeof(MoshLink));
        link->canary_pad = MOSH_CANARY_VALUE;
        block = (u8 *)link + sizeof(MoshLink);
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}

void *MoshAllocShortLived(MoshHeap * heap, u32 bs) {
    u8 * block = NULL;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
    MosTakeMutex(&heap->mtx);
    if (heap->pit + bs < heap->bot) {
        heap->bot -= (bs + sizeof(MoshSLink));
        MoshSLink * link = (MoshSLink *)heap->bot;
        link->bs = bs;
        MosAddToListAfter(&heap->sl, &link->sl);
        block = (u8 *)link + sizeof(MoshLink);
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}

void MoshFreeShortLived(MoshHeap * heap, void * block) {
    MosTakeMutex(&heap->mtx);
    MoshSLink * link = (MoshSLink *)((u8 *)block - sizeof(MoshSLink));
    if (heap->sl.next == &link->sl)
        heap->bot += (link->bs + sizeof(MoshSLink));
    else {
        MoshSLink * prev = container_of(link->sl.prev, MoshSLink, sl);
        prev->bs += (link->bs + sizeof(MoshSLink));
    }
    MosRemoveFromList(&link->sl);
    MosGiveMutex(&heap->mtx);
}
