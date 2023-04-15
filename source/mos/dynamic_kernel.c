
// Copyright 2020-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/kernel.h>

static MosHeap * pSystemHeap = NULL;
static MosMutex  ThreadMutex;

// TODO:
// 1. Polymorphic thread extension byte in MosThread?

// Local storage data
typedef struct {
    MosLink                       link;
    u32                           id;
    void                        * pData;
    MosThreadStorageReleaseFunc * pReleaseFunc;
} LocalStorage;

typedef struct {
    MosThread   thd;
    MosList     sl;
    u32         refCnt;
} DynamicThread;

void mosInitDynamicKernel(MosHeap * pHeap) {
    if (!pSystemHeap) {
        pSystemHeap = pHeap;
        mosInitMutex(&ThreadMutex);
    }
}

static void FreeDynamicThread(DynamicThread * pThd) {
    for (MosLink * pLink = pThd->sl.pNext; pLink != &pThd->sl;) {
        MosLink * pNext = pLink->pNext;
        LocalStorage * pStorage = container_of(pLink, LocalStorage, link);
        if (pStorage->pReleaseFunc) (*pStorage->pReleaseFunc)(pStorage->pData);
        pLink = pNext;
        mosFree(pSystemHeap, pStorage);
    }
    /* Free stack and thread */
    mosFree(pSystemHeap, mosGetStackBottom(&pThd->thd));
    mosFree(pSystemHeap, pThd);
}

bool mosAllocThread(MosThread ** ppThd, u32 stackSize) {
    if (!pSystemHeap) return false;
    bool rtn = false;
    u8 * pStackBottom = (u8 *)mosAlloc(pSystemHeap, stackSize);
    if (pStackBottom == NULL) return false;
    DynamicThread * pThd = (DynamicThread *)mosAlloc(pSystemHeap, sizeof(DynamicThread));
    if (pThd == NULL) {
        mosFree(pSystemHeap, pStackBottom);
        return false;
    }
    mosInitList(&pThd->sl);
    mosSetStack(&pThd->thd, pStackBottom, stackSize);
    mosLockMutex(&ThreadMutex);
    if (*ppThd == NULL) {
        pThd->refCnt = 1;
        *ppThd = (MosThread *)pThd;
        rtn = true;
    } else FreeDynamicThread(pThd);
    mosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool mosAllocAndRunThread(MosThread ** ppThd, MosThreadPriority pri,
                              MosThreadEntry * pEntry, s32 arg, u32 stackSize) {
    if (!mosAllocThread(ppThd, stackSize)) return false;
    if (!mosInitAndRunThread(*ppThd, pri, pEntry, arg, mosGetStackBottom(*ppThd),
                             stackSize)) {
        mosDecThreadRefCount(ppThd);
        return false;
    }
    return true;
}

bool mosIncThreadRefCount(MosThread ** ppThd) {
    bool rtn = false;
    mosLockMutex(&ThreadMutex);
    DynamicThread * pThd = (DynamicThread *)*ppThd;
    if (pThd != NULL) {
        pThd->refCnt++;
        rtn = true;
    }
    mosUnlockMutex(&ThreadMutex);
    return rtn;
}

bool mosDecThreadRefCount(MosThread ** ppThd) {
    bool rtn = false;
    mosLockMutex(&ThreadMutex);
    DynamicThread * pThd = (DynamicThread *)*ppThd;
    if (pThd != NULL && --pThd->refCnt <= 0) {
        *ppThd = NULL;
        FreeDynamicThread(pThd);
        rtn = true;
    }
    mosUnlockMutex(&ThreadMutex);
    return rtn;
}

u32 mosGetUniqueID(void) {
    static s32 s_nextID = 1;
    return (u32)mosAtomicFetchAndAdd32(&s_nextID, 1);
}

bool mosSetThreadStorage(MosThread * pThread, u32 uniqueID, void * pData, MosThreadStorageReleaseFunc * pReleaseFunc) {
    DynamicThread * pThd = (DynamicThread *)pThread;
    mosLockMutex(&ThreadMutex);
    MosLink * pLink;
    for (pLink = pThd->sl.pNext; pLink != &pThd->sl; pLink = pLink->pNext) {
        LocalStorage * pStorage = container_of(pLink, LocalStorage, link);
        if (pStorage->id == uniqueID) {
            pStorage->pData = pData;
            pStorage->pReleaseFunc = pReleaseFunc;
            mosUnlockMutex(&ThreadMutex);
            return true;
        }
    }
    bool bSuccess = false;
    if (pSystemHeap) {
        LocalStorage * pStorage = (LocalStorage *)mosAlloc(pSystemHeap, sizeof(LocalStorage));
        if (pStorage) {
            pStorage->id = uniqueID;
            pStorage->pData = pData;
            pStorage->pReleaseFunc = pReleaseFunc;
            mosAddToEndOfList(&pThd->sl, &pStorage->link);
            bSuccess = true;
        }
    }
    mosUnlockMutex(&ThreadMutex);
    return bSuccess;
}

void * mosGetThreadStorage(MosThread * pThread, u32 uniqueID) {
    DynamicThread * pThd = (DynamicThread *)pThread;
    void * pData = NULL;
    mosLockMutex(&ThreadMutex);
    for (MosLink * pLink = pThd->sl.pNext; pLink != &pThd->sl; pLink = pLink->pNext) {
        LocalStorage * pStorage = container_of(pLink, LocalStorage, link);
        if (pStorage->id == uniqueID) {
            pData = pStorage->pData;
            break;
        }
    }
    mosUnlockMutex(&ThreadMutex);
    return pData;
}
