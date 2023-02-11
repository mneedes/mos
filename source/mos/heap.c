
// Copyright 2019-2023 Matthew C Needes
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
    u32  canary;
    u32  size_p;
    u32  size;
} Link;

typedef struct {
    Link link;
    union { /* <--  Alignment guaranteed here */
        MosLink flLink;
        u8      payload[0];
    };
} Block;

MOS_STATIC_ASSERT(Link, sizeof(Link) == 12);
MOS_STATIC_ASSERT(Block, sizeof(Block) == 20);

void mosInitHeap(MosHeap * pHeap, u16 alignment, u8 * pBot_, u32 poolSize) {
    /* Alignment must be a power of 2, and at a minimum should be
     *   the pointer size. Smallest block must fit a Link, the free-list
     *   link and satisfy alignment requirements of payload. */
    alignment = (alignment > sizeof(void *)) ? alignment : sizeof(void *);
    pHeap->alignMask = alignment - 1;
    mosAssert((alignment & pHeap->alignMask) == 0);
    pHeap->minBlockSize = MOS_ALIGN32(MIN_PAYLOAD_SIZE + sizeof(Link), pHeap->alignMask);
    /* Free counters */
    pHeap->bytesFree    = 0;
    pHeap->minBytesFree = 0;
    /* Initialize free-list (explicit links) */ 
    pHeap->flBlockCount = 0;
    mosInitList(&pHeap->fl);
    mosInitMutex(&pHeap->mtx);
    mosAddHeapPool(pHeap, pBot_, poolSize);
}

void mosAddHeapPool(MosHeap * pHeap, u8 * pBot_, u32 poolSize) {
    if (!pBot_) return;
    Link * pLink = (Link *)MOS_ALIGN_PTR(pBot_ + sizeof(Link), pHeap->alignMask);
    Block * pBot = (Block *)(pLink - 1);
    pLink        = (Link *)MOS_ALIGN_PTR_DOWN(pBot_ + poolSize, pHeap->alignMask);
    Block * pTop = (Block *)(pLink - 1);
    /* Initialize implicit links */
    pBot->link.canary = HEAP_CANARY_VALUE;
    pBot->link.size_p = 0x1;
    pBot->link.size   = (u8 *)pTop - (u8 *)pBot;
    pTop->link.canary = HEAP_CANARY_VALUE;
    pTop->link.size_p = pBot->link.size;
    pTop->link.size   = 0x1;
    mosLockMutex(&pHeap->mtx);
    /* Add to size */
    pHeap->bytesFree    += pBot->link.size;
    pHeap->minBytesFree += pBot->link.size;
    /* Initialize free-list (explicit links) */ 
    pHeap->flBlockCount++;
    mosAddToEndOfList(&pHeap->fl, &pBot->flLink);
    mosUnlockMutex(&pHeap->mtx);
}

void * mosAlloc(MosHeap * pHeap, u32 size) {
    if (size < MIN_PAYLOAD_SIZE) size = MIN_PAYLOAD_SIZE;
    size = MOS_ALIGN32(size + sizeof(Link), pHeap->alignMask);
    mosLockMutex(&pHeap->mtx);
    Block * pBlock;
    {
        /* First-fit search: max(min_size, size) + link_size needs to fit */
        MosList * pElm;
        for (pElm = pHeap->fl.pNext; pElm != &pHeap->fl; pElm = pElm->pNext) {
            pBlock = container_of(pElm, Block, flLink);
            if (pBlock->link.size >= size) {
                mosAssert(pBlock->link.canary == HEAP_CANARY_VALUE);
                break;
            }
        }
        if (pElm == &pHeap->fl) {
            mosUnlockMutex(&pHeap->mtx);
            return NULL;
        }
        /* Remove chosen block from free-list */
        mosRemoveFromList(pElm);
    }
    Block * pNextBlock = (Block *)((u8 *)pBlock + pBlock->link.size);
    /* Split the block if there is enough room left for a block of minimum size */
    if (pBlock->link.size >= size + pHeap->minBlockSize) {
        u32 nextBlockSize = pBlock->link.size - size;
        pNextBlock->link.size_p = nextBlockSize;
        /* The new next block */
        pNextBlock = (Block *)((u8 *)pBlock + size);
        pNextBlock->link.canary = HEAP_CANARY_VALUE;
        pNextBlock->link.size = nextBlockSize;
        /* Set size and mark allocation bit */
        pNextBlock->link.size_p = size + 1;
        pBlock->link.size = pNextBlock->link.size_p;
        /* Add new block to free-list */
        mosAddToEndOfList(&pHeap->fl, &pNextBlock->flLink);
        pHeap->bytesFree -= size;
    } else {
        /* Use existing block */
        pHeap->flBlockCount -= 1;
        pHeap->bytesFree -= pNextBlock->link.size_p;
        /* Mark allocation bit */
        pNextBlock->link.size_p += 1;
        pBlock->link.size = pNextBlock->link.size_p;
    }
    if (pHeap->bytesFree < pHeap->minBytesFree)
        pHeap->minBytesFree = pHeap->bytesFree;
    mosUnlockMutex(&pHeap->mtx);
    return (void *)((u8 *)pBlock + sizeof(Link));
}

void * mosRealloc(MosHeap * pHeap, void * pBlock_, u32 newSize_) {
    if (!pBlock_) return mosAlloc(pHeap, newSize_);
    mosLockMutex(&pHeap->mtx);
    Block * pBlock = (Block *)((u8 *)pBlock_ - sizeof(Link));
    /* Check for canary value and double-free */
    mosAssert(pBlock->link.canary == HEAP_CANARY_VALUE);
    mosAssert(pBlock->link.size & 0x1);
    u32 newSize = newSize_;
    if (newSize < MIN_PAYLOAD_SIZE) newSize = MIN_PAYLOAD_SIZE;
    newSize = MOS_ALIGN32(newSize + sizeof(Link), pHeap->alignMask);
    u32 avail = pBlock->link.size - 1;
    Block * pNextBlock = (Block *)((u8 *)pBlock + avail);
    if (!(pNextBlock->link.size & 0x1)) {
        if (avail + pNextBlock->link.size >= newSize) {
            avail += pNextBlock->link.size;
            /* Merge with next */
            mosRemoveFromList(&pNextBlock->flLink);
            pHeap->flBlockCount -= 1;
            pHeap->bytesFree -= pNextBlock->link.size;
            pBlock->link.size += pNextBlock->link.size;
            /* The new next block */
            pNextBlock = (Block *)((u8 *)pBlock + pBlock->link.size - 1);
            pNextBlock->link.size_p = pBlock->link.size;
        }
    }
    if (avail >= newSize + pHeap->minBlockSize) {
        /* Split */
        u32 nextBlockSize = pBlock->link.size - newSize - 1;
        pHeap->bytesFree += nextBlockSize;
        pNextBlock->link.size_p = nextBlockSize;
        /* The new next block */
        pNextBlock = (Block *)((u8 *)pBlock + newSize);
        pNextBlock->link.canary = HEAP_CANARY_VALUE;
        pNextBlock->link.size = nextBlockSize;
        /* Set size and mark allocation bit */
        pNextBlock->link.size_p = newSize + 1;
        pBlock->link.size = pNextBlock->link.size_p;
        /* Add new block to free-list */
        mosAddToEndOfList(&pHeap->fl, &pNextBlock->flLink);
        pHeap->flBlockCount += 1;
    } else if (avail < newSize) {
        /* Move */
        u8 * pNewBlock = mosAlloc(pHeap, newSize_);
        if (pNewBlock) {
            u32 oldSize = pBlock->link.size - sizeof(Link) - 1;
            u32 copySize = newSize_ < oldSize ? newSize_ : oldSize;
            for (u32 ix = 0; ix < copySize; ix++)
                pNewBlock[ix] = ((u8 *)pBlock_)[ix];
            /* Only free block if successful */
            mosFree(pHeap, pBlock_);
        }
        pBlock_ = pNewBlock;
    }
    mosUnlockMutex(&pHeap->mtx);
    return pBlock_;
}

void mosFree(MosHeap * pHeap, void * pBlock_) {
    if (!pBlock_) return;
    Block * pBlock = (Block *)((u8 *)pBlock_ - sizeof(Link));
    mosLockMutex(&pHeap->mtx);
    /* Check for canary value and double-free */
    mosAssert(pBlock->link.canary == HEAP_CANARY_VALUE);
    mosAssert(pBlock->link.size & 0x1);
    /* Clear allocation bit */
    pBlock->link.size--;
    /* Check next canary value */
    Block * pNext = (Block *)((u8 *)pBlock + pBlock->link.size);
    mosAssert(pNext->link.canary == HEAP_CANARY_VALUE);
    pHeap->bytesFree += pBlock->link.size;
    /* Find next and previous blocks and determine if allocated */
    Block * pPrev = NULL;
    if (!(pBlock->link.size_p & 0x1))
        pPrev = (Block *)((u8 *)pBlock - pBlock->link.size_p);
    s32 sizeIncrease = 0;
    if (!(pNext->link.size & 0x1)) {
        if (pPrev) {
            /* Combine with previous and next */
            sizeIncrease += pBlock->link.size + pNext->link.size;
            mosRemoveFromList(&pPrev->flLink);
            mosRemoveFromList(&pNext->flLink);
            pBlock = pPrev;
            pHeap->flBlockCount -= 1;
        } else {
            /* Combine with next */
            sizeIncrease += pNext->link.size;
            mosRemoveFromList(&pNext->flLink);
        }
    } else if (pPrev) {
        /* Combine with previous */
        sizeIncrease += pBlock->link.size;
        mosRemoveFromList(&pPrev->flLink);
        pBlock = pPrev;
    } else {
        /* No combination possible */
        pHeap->flBlockCount += 1;
    }
    /* Adjust implicit links */
    pBlock->link.size += sizeIncrease;
    pNext = (Block *)((u8 *)pBlock + pBlock->link.size);
    pNext->link.size_p = pBlock->link.size;
    /* Add block to free-list */
    mosAddToFrontOfList(&pHeap->fl, &pBlock->flLink);
    mosUnlockMutex(&pHeap->mtx);
}
