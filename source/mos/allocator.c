
// Copyright 2019-2024 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/allocator.h>

/*
 * MOS General Purpose Allocator
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
 *     12 - 65536 - 131071
 *     13 - 131072 (and above)
 *
 * Heap may be composed of multiple non-contiguous pools:
 *
 *   Pool format at initialization:
 *     PAD
 *       pointer to next pool
 *       MosList bins[14];   // NOTE: bins are present only in first pool
 *     PAD to guarantee block alignment after First Link
 *       First Link
 *       ...Free Space...
 *       Last Link
 *
 * Allocated blocks:
 *
 *  |canary_tag|size_p| size |      payload       |canary_tag|size_p| size...
 *                           ^                    ^
 *                       alignment                +- next block
 *
 *    Actual alignment = max(requested alignment, pointer size)
 *    Implicit link sizes = sizeof(payload) + sizeof(Link)
 *    LSB of size/size_p gets set to indicate block is allocated
 *
 * Freed block:
 *
 *  |canary_tag|size_p| size | (free-list link)  payload  |...
 *                           ^         ^                  ^
 *                       alignment     |                  +- next block
 *                                     |
 *   Payload of freed block contains explicit link
 */

#if defined(__SIZEOF_SIZE_T__)
#if (__SIZEOF_SIZE_T__ == 8)
  #define MOS_ARCHITECTURE_BITS 64
#elif (__SIZEOF_SIZE_T__ == 4)
  #define MOS_ARCHITECTURE_BITS 32
#endif
#endif

enum {
    CANARY_CHECKSUM   = 0xe711,
    CANARY_DEFAULT    = CANARY_CHECKSUM << 16,
    MIN_PAYLOAD_SIZE  = sizeof(MosList),
    MIN_BIN_SHIFT     = 4,
    NUM_BINS          = 14,
    MAX_ALLOC_TRIES   = 8,
};

typedef struct {
    u32  canary_tag;   /* combination canary/tag field */
#if MOS_ARCHITECTURE_BITS == 64
    u32  pad;
#endif
    u32  size_p;       /* size of previous block */
    u32  size;         /* size of this block */
} Link;

typedef struct {
    Link link;
    union {            /* <--  Alignment guaranteed here */
        MosLink flLink;
        u8      payload[0];
    };
} Block;

#if MOS_ARCHITECTURE_BITS == 32
MOS_STATIC_ASSERT(Link, sizeof(Link) == 12);
MOS_STATIC_ASSERT(Block, sizeof(Block) == 20);
#elif MOS_ARCHITECTURE_BITS == 64
MOS_STATIC_ASSERT(Link, sizeof(Link) == 16);
MOS_STATIC_ASSERT(Block, sizeof(Block) == 32);
#endif

#define BIN_MASK_FOR_SIZE(size) (1 << BinNumForSize(size))
#define CANARY_CHECK(c)         (((c) & 0xffff) ^ ((c) >> 16) == CANARY_CHECKSUM)

void mosInitHeap(MosHeap * pHeap, u16 alignment, u8 * pBot, u32 poolSize) {
    /* Alignment must be a power of 2, and at a minimum should be
     *   the pointer size. Smallest block must fit a Link, the free-list
     *   link and satisfy alignment requirements of payload. */
    alignment = (alignment > sizeof(void *)) ? alignment : sizeof(void *);
    pHeap->alignMask = alignment - 1;
    mosAssert((alignment & pHeap->alignMask) == 0);
    pHeap->minBlockSize = MOS_ALIGN32(MIN_PAYLOAD_SIZE + sizeof(Link), pHeap->alignMask);
    /* Align bins to MosList alignment, leaving room for next pool pointer */
    pHeap->pBins = (MosList *)MOS_ALIGN_PTR(pBot + sizeof(void *), sizeof(void *) - 1);
    mosAssert((u8 *)(&pHeap->pBins[NUM_BINS]) - pBot < poolSize);
    /* Init free-list bins */
    for (u32 ix = 0; ix < NUM_BINS; ix++) {
        mosInitList(&pHeap->pBins[ix]);
    }
    /* Init next pool pointer */
    pBot = (u8 *)pHeap->pBins - sizeof(void *);
    *((void **)pBot) = NULL;
    pHeap->binMask = 0;
    /* Free counters */
    pHeap->bytesFree    = 0;
    pHeap->minBytesFree = 0;
    mosInitMutex(&pHeap->mtx);
    mosAddHeapPool(pHeap, pBot, poolSize);
}

MOS_INLINE u32 BinNumForSize(u32 size) {
    u32 bin = 31 - __builtin_clz(size) - MIN_BIN_SHIFT;
    return bin > (NUM_BINS - 1) ? NUM_BINS - 1 : bin;
}

static void RemoveFromFreeList(MosHeap * pHeap, Block * pBlock) {
    /* Clear mask bit if removing last item on list */
    if (pBlock->flLink.pNext == pBlock->flLink.pPrev)
        pHeap->binMask -= BIN_MASK_FOR_SIZE(pBlock->link.size);
    mosRemoveFromList(&pBlock->flLink);
}

static void AddToFreeList(MosHeap * pHeap, Block * pBlock) {
    u32 bin = BinNumForSize(pBlock->link.size);
    mosAddToFrontOfList(&pHeap->pBins[bin], &pBlock->flLink);
    pHeap->binMask |= (1 << bin);
}

void mosAddHeapPool(MosHeap * pHeap, u8 * pBot__, u32 poolSize) {
    mosAssert(poolSize >= 256);
    mosLockMutex(&pHeap->mtx);
    u8 * pBot_ = (void *)MOS_ALIGN_PTR(pBot__, sizeof(void *) - 1);
    if (pBot_ == ((u8 *)pHeap->pBins) - sizeof(void *)) {
        /* First pool, skip bin array */
        pBot_ = (u8 *)&pHeap->pBins[NUM_BINS];
    } else {
        /* Subsequent pool, add pool to end of list */
        void ** ppNext = (void **)(((u8 *)pHeap->pBins) - sizeof(void *));
        while (*ppNext != NULL) ppNext = *ppNext;
        *ppNext = pBot_;
        *((void **)pBot_) = NULL;
        pBot_ += sizeof(void *);
    }
    poolSize -= (pBot_ - pBot__);
    Link * pLink = (Link *)MOS_ALIGN_PTR(pBot_ + sizeof(Link), pHeap->alignMask);
    Block * pBot = (Block *)(pLink - 1);
    pLink        = (Link *)MOS_ALIGN_PTR_DOWN(pBot_ + poolSize, pHeap->alignMask);
    Block * pTop = (Block *)(pLink - 1);
    /* Initialize implicit links */
    pBot->link.canary_tag = CANARY_DEFAULT;
    pBot->link.size_p     = 0x1;
    pBot->link.size       = (u8 *)pTop - (u8 *)pBot;
    pTop->link.canary_tag = CANARY_DEFAULT;
    pTop->link.size_p     = pBot->link.size;
    pTop->link.size       = 0x1;
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
                mosAssert(CANARY_CHECK(pBlock->link.canary_tag));
                if (pBlock->link.size >= size) goto FOUND;
            }
            /* Otherwise split next available bin */
            searchMask -= (1 << bin);
            if (searchMask) {
                bin = __builtin_ctz(searchMask);
                pBlock = container_of(pHeap->pBins[bin].pNext, Block, flLink);
                mosAssert(CANARY_CHECK(pBlock->link.canary_tag));
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
        pNextBlock->link.canary_tag = CANARY_DEFAULT;
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
    /* Check for canary_tag value and double-free */
    mosAssert(CANARY_CHECK(pBlock->link.canary_tag));
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
        pNextBlock->link.canary_tag = CANARY_DEFAULT;
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
    /* Check for canary_tag value and double-free */
    mosAssert(CANARY_CHECK(pBlock->link.canary_tag));
    mosAssert(pBlock->link.size & 0x1);
    /* Clear allocation bit */
    pBlock->link.size--;
    /* Check next canary_tag value */
    Block * pNext = (Block *)((u8 *)pBlock + pBlock->link.size);
    mosAssert(CANARY_CHECK(pNext->link.canary_tag));
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

void mosTagAllocatedBlock(void * pBlock_, u16 tag) {
    Block * pBlock = (Block *)((u8 *)pBlock_ - sizeof(Link));
    mosLockMutex(&pHeap->mtx);
    mosAssert(CANARY_CHECK(pBlock->link.canary_tag));
    mosAssert(pBlock->link.size & 0x1);
    pBlock->link.canary_tag = tag | (CANARY_CHECKSUM ^ tag) << 16;
    mosUnlockMutex(&pHeap->mtx);
}

void mosWalkHeap(MosHeap * pHeap, mosHeapCallbackFunc * pFunc) {
    mosLockMutex(&pHeap->mtx);
    void ** ppNext = (void **)(((u8 *)pHeap->pBins) - sizeof(void *));
    u8 * pBot = (u8 *)&pHeap->pBins[NUM_BINS];
    do {
        Link * pLink  = (Link *)MOS_ALIGN_PTR(pBot + sizeof(Link), pHeap->alignMask);
        pLink--;
        do {
           Block * pBlock = container_of(pLink, Block, flLink);
           u32 nSize = pLink->size;
           if (nSize & 0x1) {
               nSize--;
               (*pFunc)((void *)(pLink + 1), pLink->canary_tag & 0xffff, nSize);
           } else {
               (*pFunc)(NULL, 0, pLink->size);
           }
           pLink = (Link *)((u8 *)pLink + nSize);
        } while (pLink->size != 1);
        ppNext = *ppNext;
        pBot = ((u8 *)ppNext) + sizeof(void *);
    } while (ppNext != NULL);
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

