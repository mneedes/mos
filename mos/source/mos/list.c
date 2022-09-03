
// Copyright 2020-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/list.h>

MOS_ISR_SAFE void MosInitList(MosList * pList) {
    pList->pPrev = pList;
    pList->pNext = pList;
}

MOS_ISR_SAFE void MosInitLinkHet(MosLinkHet * pLink, u32 type) {
    pLink->link.pPrev = &pLink->link;
    pLink->link.pNext = &pLink->link;
    pLink->type = type;
}

MOS_ISR_SAFE void MosAddToList(MosList * pList, MosList * pElmAdd) {
    pElmAdd->pPrev = pList->pPrev;
    pElmAdd->pNext = pList;
    pList->pPrev->pNext = pElmAdd;
    pList->pPrev = pElmAdd;
}

MOS_ISR_SAFE void MosAddToListAfter(MosList * pList, MosList * pElmAdd) {
    pElmAdd->pPrev = pList;
    pElmAdd->pNext = pList->pNext;
    pList->pNext->pPrev = pElmAdd;
    pList->pNext = pElmAdd;
}

MOS_ISR_SAFE void MosRemoveFromList(MosList * pElmRem) {
    pElmRem->pNext->pPrev = pElmRem->pPrev;
    pElmRem->pPrev->pNext = pElmRem->pNext;
    // For MosIsElementOnList() and safety
    pElmRem->pPrev = pElmRem;
    pElmRem->pNext = pElmRem;
}

MOS_ISR_SAFE void MosMoveToEndOfList(MosList * pElmExist, MosList * pElmMove) {
    // Remove element
    pElmMove->pNext->pPrev = pElmMove->pPrev;
    pElmMove->pPrev->pNext = pElmMove->pNext;
    // Add to end of pList
    pElmMove->pPrev = pElmExist->pPrev;
    pElmMove->pNext = pElmExist;
    pElmExist->pPrev->pNext = pElmMove;
    pElmExist->pPrev = pElmMove;
}
