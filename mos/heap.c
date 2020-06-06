
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#include "mos/heap.h"

enum {
    STD_BLOCK_ID = 0xe711dead,
    ODD_BLOCK_ID = 0xba5eba11,
    SL_BLOCK_ID  = 0x7331e711
};

// MosBlockDesc and MosLink shall be sized to guarantee
//   MOSH_HEAP_ALIGNED alignment.

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
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
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

void MosFree(MosHeap * heap, void * block) {
    MosLink * link = (MosLink *)((u8 *)block - sizeof(MosLink));
    MosTakeMutex(&heap->mtx);
    switch (link->id_canary) {
    case STD_BLOCK_ID:
        MosAddToList(&link->bd->fl, &link->fl);
        break;
    case ODD_BLOCK_ID:
        {
            MosList * elm;
            // Insertion sort into odd size free-list
            for (elm = heap->osl.next; elm != &heap->osl; elm = elm->next) {
                if (container_of(elm, MosLink, fl)->bs >= link->bs) break;
            }
            MosAddToListBefore(elm, &link->fl);
        }
        break;
    case SL_BLOCK_ID:
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
    u8 * block = NULL;
    MosTakeMutex(&heap->mtx);
    if (!MosIsListEmpty(&heap->bsl)) {
        // Find the smallest block size to accommodate request
        MosList * elm;
        for (elm = heap->bsl.next; elm != &heap->bsl; elm = elm->next) {
            if (container_of(elm, MosBlockDesc, bsl)->bs >= bs) {
                // Allocate free block if available...
                MosBlockDesc * bd = container_of(elm, MosBlockDesc, bsl);
                if (!MosIsListEmpty(&bd->fl)) {
                    MosLink * link = container_of(bd->fl.next, MosLink, fl);
                    MosRemoveFromList(bd->fl.next);
                    block = (u8 *) link + sizeof(MosLink);
                    break;
                } else if (heap->pit + bd->bs < heap->bot) {
                    // ...allocate new block from pit since there is room
                    MosLink * link = (MosLink *)heap->pit;
                    link->bd = bd;
                    heap->pit += (bd->bs + sizeof(MosLink));
                    link->id_canary = STD_BLOCK_ID;
                    block = (u8 *) link + sizeof(MosLink);
                    break;
                }
                // continue to see if next higher block size is available
            }
        }
    }
    MosGiveMutex(&heap->mtx);
    return (void *)block;
}

void * MosAllocOddBlock(MosHeap * heap, u32 bs) {
    MosLink * link = NULL;
    MosLink * link_found = NULL;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
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
    u8 * block = NULL;
    // Round bs to next aligned value
    if (bs % MOSH_HEAP_ALIGNMENT)
        bs += (MOSH_HEAP_ALIGNMENT - (bs % MOSH_HEAP_ALIGNMENT));
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
