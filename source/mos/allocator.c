
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/allocator.h>

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
 *
 * Deterministic best-effort allocation using bins:
 *
 *   Free blocks are stored in power-of-2 bins. N attempts are made to
 *   find a block that can accommodate the allocation from its bin size,
 *   otherwise the next block size up is split.
 *
 *   Allocation sizes are a minimum of 2^4 = 16.  Therefore we omit the
 *   first 4 bins.  A bin is for block sizes in the interval:
 *        2^(bin + 4) <= size < 2^(bin + 5)
 *
 *     Bin  size
 *      0 - 16-31
 *      1 - 32-63
 *      2 - 64-127
 *         ...
 *     13 - 131072-262143
 */

enum {
    HEAP_CANARY_VALUE = 0xe711dead,
    MIN_PAYLOAD_SIZE  = sizeof(MosList),
    MIN_BIN_SHIFT     = 4,
    NUM_BINS          = 14,
    MAX_ALLOC_TRIES   = 8,
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

#define BIN_NUM_FOR_SIZE(size)  (31 - __builtin_clz(size) - MIN_BIN_SHIFT)
#define BIN_MASK_FOR_SIZE(size) (1 << BIN_NUM_FOR_SIZE(size))

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
    /* Init free-list bins */
    pHeap->pBins = (MosList *)MOS_ALIGN_PTR(pBot_, sizeof(void *) - 1);
    mosAssert((u8 *)(&pHeap->pBins[NUM_BINS]) - pBot_ < poolSize);
    for (u32 ix = 0; ix < NUM_BINS; ix++) {
        mosInitList(&pHeap->pBins[ix]);
    }
    poolSize -= ((u8 *)&pHeap->pBins[NUM_BINS] - pBot_);
    pBot_ = (u8 *)&pHeap->pBins[NUM_BINS];
    pHeap->binMask = 0;
    /* Free counters */
    pHeap->bytesFree    = 0;
    pHeap->minBytesFree = 0;
    mosInitMutex(&pHeap->mtx);
    mosAddHeapPool(pHeap, pBot_, poolSize);
}

static void RemoveFromFreeList(MosHeap * pHeap, Block * pBlock) {
    /* Clear mask bit if removing last item on list */
    if (pBlock->flLink.pNext == pBlock->flLink.pPrev)
        pHeap->binMask -= BIN_MASK_FOR_SIZE(pBlock->link.size);
    mosRemoveFromList(&pBlock->flLink);
}

static void AddToFreeList(MosHeap * pHeap, Block * pBlock) {
    u32 bin = BIN_NUM_FOR_SIZE(pBlock->link.size);
    mosAddToFrontOfList(&pHeap->pBins[bin], &pBlock->flLink);
    pHeap->binMask |= (1 << bin);
}

void mosAddHeapPool(MosHeap * pHeap, u8 * pBot_, u32 poolSize) {
    mosAssert(poolSize >= 128);
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
    mosAssert(BIN_NUM_FOR_SIZE(pBot->link.size) < NUM_BINS);
    mosLockMutex(&pHeap->mtx);
    /* Add to size */
    pHeap->bytesFree    += pBot->link.size;
    pHeap->minBytesFree += pBot->link.size;
    /* Add to free list bin */
    AddToFreeList(pHeap, pBot);
    mosUnlockMutex(&pHeap->mtx);
}

void * mosAlloc(MosHeap * pHeap, u32 size) {
    if (size < MIN_PAYLOAD_SIZE) size = MIN_PAYLOAD_SIZE;
    size = MOS_ALIGN32(size + sizeof(Link), pHeap->alignMask);
    mosLockMutex(&pHeap->mtx);
    Block * pBlock;
    {
        /* Deterministic best-effort search */
        u32 searchMask = pHeap->binMask & ~(BIN_MASK_FOR_SIZE(size) - 1);
        if (searchMask) {
            u32 bin = __builtin_ctz(searchMask);
            MosList * pLink = pHeap->pBins[bin].pNext;
            /* First try best-fit list */
            for (u32 try = 0; try < MAX_ALLOC_TRIES; try++, pLink = pLink->pNext) {
                if (pLink == &pHeap->pBins[bin]) break;
                pBlock = container_of(pLink, Block, flLink);
                mosAssert(pBlock->link.canary == HEAP_CANARY_VALUE);
                if (pBlock->link.size >= size) goto FOUND;
            }
            /* Otherwise split next available bin */
            searchMask -= (1 << bin);
            if (searchMask) {
                bin = __builtin_ctz(searchMask);
                pBlock = container_of(pHeap->pBins[bin].pNext, Block, flLink);
                mosAssert(pBlock->link.canary == HEAP_CANARY_VALUE);
                goto FOUND;
            }
        }
        mosUnlockMutex(&pHeap->mtx);
        return NULL;
FOUND:
        RemoveFromFreeList(pHeap, pBlock);
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
        AddToFreeList(pHeap, pNextBlock);
        pHeap->bytesFree -= size;
    } else {
        /* Use existing block */
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
    if (!newSize_) {
        mosFree(pHeap, pBlock_);
        return NULL;
    }
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
            RemoveFromFreeList(pHeap, pNextBlock);
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
        AddToFreeList(pHeap, pNextBlock);
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
            RemoveFromFreeList(pHeap, pPrev);
            RemoveFromFreeList(pHeap, pNext);
            pBlock = pPrev;
        } else {
            /* Combine with next */
            sizeIncrease += pNext->link.size;
            RemoveFromFreeList(pHeap, pNext);
        }
    } else if (pPrev) {
        /* Combine with previous */
        sizeIncrease += pBlock->link.size;
        RemoveFromFreeList(pHeap, pPrev);
        pBlock = pPrev;
    }
    /* Adjust implicit links */
    pBlock->link.size += sizeIncrease;
    pNext = (Block *)((u8 *)pBlock + pBlock->link.size);
    pNext->link.size_p = pBlock->link.size;
    /* Add block to free-list */
    AddToFreeList(pHeap, pBlock);
    mosUnlockMutex(&pHeap->mtx);
}

u32 mosGetBiggestAvailableChunk(MosHeap * pHeap) {
    u32 maxChunk = 0;
    mosLockMutex(&pHeap->mtx);
    u32 bin = 31 - __builtin_clz(pHeap->binMask);
    MosLink * pLink = pHeap->pBins[bin].pNext;
    for (; pLink != &pHeap->pBins[bin]; pLink = pLink->pNext) {
        Block * pBlock = container_of(pLink, Block, flLink);
        if (pBlock->link.size > maxChunk) maxChunk = pBlock->link.size;
    }
    mosUnlockMutex(&pHeap->mtx);
    return maxChunk;
}

