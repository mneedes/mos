
// Copyright 2020-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/list.h
/// \brief Doubly-Linked Circular Lists

#ifndef _MOS_LIST_H_
#define _MOS_LIST_H_

#include <mos/defs.h>

// List descriptor and link*
//   (*for homogeneous pLists)

typedef struct MosList {
    struct MosList * pPrev;
    struct MosList * pNext;
} MosList;

typedef MosList MosLink;

// List link for heterogeneous pLists
typedef struct {
    MosLink link;
    u32     type;
} MosLinkHet;

MOS_ISR_SAFE void MosInitList(MosList * pList);
MOS_ISR_SAFE void MosInitLinkHet(MosLinkHet * pElm, u32 type);
MOS_ISR_SAFE void MosAddToList(MosList * pList, MosList * pElmAdd);
MOS_ISR_SAFE static MOS_INLINE void
MosAddToListBefore(MosList * pElmExist, MosList * pElmAdd) {
    // AddToList <=> AddToListBefore if used on element rather than pList
    MosAddToList(pElmExist, pElmAdd);
}
MOS_ISR_SAFE void MosAddToListAfter(MosList * pElmExist, MosList * pElmAdd);
MOS_ISR_SAFE static MOS_INLINE void
MosAddToFrontOfList(MosList * pList, MosList * pElmAdd) {
    // AddToListAfter <=> AddToFrontOfList if used on pList rather than element
    MosAddToListAfter(pList, pElmAdd);
}
MOS_ISR_SAFE void MosRemoveFromList(MosList * pElmRem);
MOS_ISR_SAFE void MosMoveToEndOfList(MosList * pElmExist, MosList * pElmMove);
MOS_ISR_SAFE static MOS_INLINE bool MosIsLastElement(MosList * pList, MosList * pElm) {
    return (pList->pPrev == pElm);
}
MOS_ISR_SAFE static MOS_INLINE bool MosIsListEmpty(MosList * pList) {
    return (pList->pPrev == pList);
}
MOS_ISR_SAFE static MOS_INLINE bool MosIsOnList(MosList * pElm) {
    return (pElm->pPrev != pElm);
}

#endif
