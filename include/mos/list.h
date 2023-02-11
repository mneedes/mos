
// Copyright 2020-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/list.h
/// \brief Doubly-Linked Circular Lists

#ifndef _MOS_LIST_H_
#define _MOS_LIST_H_

#include <mos/defs.h>

// Link for homogeneous lists
typedef struct MosLink {
    struct MosLink * pPrev;
    struct MosLink * pNext;
} MosLink;

// List descriptor is a link
typedef MosLink MosList;

// Link for polymorphic lists
typedef struct {
    MosLink link;
    u32     type;
} MosPmLink;

MOS_ISR_SAFE void mosInitList(MosList * pList);
MOS_ISR_SAFE void mosInitPmLink(MosPmLink * pElm, u32 type);
MOS_ISR_SAFE void mosAddToEndOfList(MosList * pList, MosLink * pElmAdd);
MOS_ISR_SAFE static MOS_INLINE void
mosAddToListBefore(MosLink * pElmExist, MosLink * pElmAdd) {
    // AddToList <=> AddToListBefore if used on element rather than pList
    mosAddToEndOfList(pElmExist, pElmAdd);
}
MOS_ISR_SAFE void mosAddToListAfter(MosLink * pElmExist, MosLink * pElmAdd);
MOS_ISR_SAFE static MOS_INLINE void mosAddToFrontOfList(MosList * pList, MosLink * pElmAdd) {
    // AddToListAfter <=> AddToFrontOfList if used on pList rather than element
    mosAddToListAfter(pList, pElmAdd);
}
MOS_ISR_SAFE void mosRemoveFromList(MosLink * pElmRem);
MOS_ISR_SAFE void mosMoveToEndOfList(MosList * pList, MosLink * pElmMove);
MOS_ISR_SAFE static MOS_INLINE bool mosIsAtEndOfList(MosList * pList, MosLink * pElm) {
    return (pList->pPrev == pElm);
}
MOS_ISR_SAFE static MOS_INLINE bool mosIsListEmpty(MosList * pList) {
    return (pList->pPrev == pList);
}
MOS_ISR_SAFE static MOS_INLINE bool mosIsOnList(MosLink * pElm) {
    return (pElm->pPrev != pElm);
}

#endif
