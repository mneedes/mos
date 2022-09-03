
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/experimental/slab.h>

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
    MosList  blkQ;
    MosLink  slabLink;
    u32      availBlocks;
    u8       alignPadAndBlocks[0];
} Slab;

typedef struct {
    Slab * pSlab;
    union { // <--  Alignment guaranteed here
        MosLink flLink;
        u8 payload[0];
    };
} Block;

MOS_STATIC_ASSERT(Block, sizeof(Block) == 12);

void MosInitPool(MosPool * pPool, MosHeap * pHeap, u32 blocksPerSlab, u32 blockSize, u16 alignment) {
    // Alignment must be a power of 2, and at a minimum should be the pointer size.
    alignment = (alignment > sizeof(void *)) ? alignment : sizeof(void *);
    pPool->alignMask = alignment - 1;
    MosAssert((alignment & pPool->alignMask) == 0);
    // Actual block size has overhead bytes and minimum size
    if (blockSize < MIN_PAYLOAD_SIZE) blockSize = MIN_PAYLOAD_SIZE;
    blockSize = MOS_ALIGN32(blockSize + sizeof(Slab *), pPool->alignMask);
    pPool->blockSize = blockSize;
    u32 heapAlignment = pHeap->alignMask + 1;
    u32 maxAlignOffset = alignment;
    if (heapAlignment < alignment) {
        maxAlignOffset -= heapAlignment;
    } else maxAlignOffset -= MIN_ALIGNMENT;
    // Allocate over-sized slab to provide enough adjustment room to guarantee block alignment.
    pPool->slabSize = sizeof(Slab) + maxAlignOffset + blockSize * blocksPerSlab;
    MosAssert(blocksPerSlab > 0);
    pPool->blocksPerSlab = blocksPerSlab;
    pPool->availBlocks = 0;
    pPool->pHeap = pHeap;
    MosInitList(&pPool->partQ);
    MosInitList(&pPool->freeQ);
    MosInitList(&pPool->fullQ);
}

u32 MosAddSlabsToPool(MosPool * pPool, u32 maxToAdd) {
    u32 slabsAddedCount = 0;
    for (; slabsAddedCount < maxToAdd; slabsAddedCount++) {
        // Allocate pSlab
        Slab * pSlab = MosAlloc(pPool->pHeap, pPool->slabSize);
        if (!pSlab) break;
        MosInitList(&pSlab->blkQ);
        MosInitList(&pSlab->slabLink);
        // Align here
        u8 * pBuf = (u8 *)pSlab + sizeof(Slab) + sizeof(Slab *);
        pBuf = (u8 *)MOS_ALIGN_PTR(pBuf, pPool->alignMask) - sizeof(Slab *);
        // Link up blocks
        for (u32 ix = 0; ix < pPool->blocksPerSlab; ix++) {
            Block * pBlock = (Block *)pBuf;
            pBlock->pSlab = pSlab;
            MosAddToList(&pSlab->blkQ, &pBlock->flLink);
            pBuf += pPool->blockSize;
        }
        pSlab->availBlocks = pPool->blocksPerSlab;
        _MosDisableInterrupts();
        MosAddToList(&pPool->freeQ, &pSlab->slabLink);
        pPool->availBlocks += pPool->blocksPerSlab;
        _MosEnableInterrupts();
    }
    return slabsAddedCount;
}

u32 MosFreeUnallocatedSlabs(MosPool * pPool, u32 maxToRemove) {
    // Remove free slabs
    u32 slabsRemovedCount = 0;
    while (1) {
        if (slabsRemovedCount == maxToRemove) break;
        _MosDisableInterrupts();
        if (MosIsListEmpty(&pPool->freeQ)) {
            _MosEnableInterrupts();
            break;
        }
        pPool->availBlocks -= pPool->blocksPerSlab;
        MosList * pElm = pPool->freeQ.pNext;
        MosRemoveFromList(pElm);
        _MosEnableInterrupts();
        MosFree(pPool->pHeap, container_of(pElm, Slab, slabLink));
        slabsRemovedCount++;
    }
    return slabsRemovedCount;
}

MOS_ISR_SAFE void * MosAllocFromSlab(MosPool * pPool) {
    u32 mask = MosDisableInterrupts();
    if (pPool->availBlocks) {
        pPool->availBlocks--;
        Slab * pSlab;
        if (!MosIsListEmpty(&pPool->partQ)) {
            pSlab = container_of(pPool->partQ.pNext, Slab, slabLink);
            if (--pSlab->availBlocks == 0) {
                MosRemoveFromList(&pSlab->slabLink);
                MosAddToList(&pPool->fullQ, &pSlab->slabLink);
            }
        } else {
            pSlab = container_of(pPool->freeQ.pNext, Slab, slabLink);
            pSlab->availBlocks--;
            MosRemoveFromList(&pSlab->slabLink);
            MosAddToList(&pPool->partQ, &pSlab->slabLink);
        }
        MosList * pElm = pSlab->blkQ.pNext;
        MosRemoveFromList(pElm);
        Block * pBlock = container_of(pElm, Block, flLink);
        MosEnableInterrupts(mask);
        return &pBlock->payload;
    }
    MosEnableInterrupts(mask);
    return NULL;
}

MOS_ISR_SAFE void MosFreeToSlab(MosPool * pPool, void * pBlock_) {
    Block * pBlock = (Block *)((u32 *)pBlock_ - 1);
    u32 mask = MosDisableInterrupts();
    pPool->availBlocks++;
    MosAddToList(&pBlock->pSlab->blkQ, &pBlock->flLink);
    pBlock->pSlab->availBlocks++;
    if (pBlock->pSlab->availBlocks == 1) {
        MosRemoveFromList(&pBlock->pSlab->slabLink);
        MosAddToList(&pPool->partQ, &pBlock->pSlab->slabLink);
    } else if (pBlock->pSlab->availBlocks == pPool->blocksPerSlab) {
        MosRemoveFromList(&pBlock->pSlab->slabLink);
        MosAddToList(&pPool->freeQ, &pBlock->pSlab->slabLink);
    }
    MosEnableInterrupts(mask);
}
