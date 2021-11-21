
// Copyright 2020-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/list.h
/// \brief Doubly-Linked Circular Lists

#ifndef _MOS_LIST_H_
#define _MOS_LIST_H_

#include <mos/defs.h>

// List descriptor and link*
//   (*for homogeneous lists)

typedef struct MosList {
    struct MosList * prev;
    struct MosList * next;
} MosList;

typedef MosList MosLink;

// List link for heterogeneous lists
typedef struct {
    MosLink link;
    u32 type;
} MosLinkHet;

MOS_ISR_SAFE void MosInitList(MosList * list);
MOS_ISR_SAFE void MosInitLinkHet(MosLinkHet * elm, u32 type);
MOS_ISR_SAFE void MosAddToList(MosList * list, MosList * elm_add);
MOS_ISR_SAFE static MOS_INLINE void
MosAddToListBefore(MosList * elm_exist, MosList * elm_add) {
    // AddToList <=> AddToListBefore if used on element rather than list
    MosAddToList(elm_exist, elm_add);
}
MOS_ISR_SAFE void MosAddToListAfter(MosList * elm_exist, MosList * elm_add);
MOS_ISR_SAFE static MOS_INLINE void
MosAddToFrontOfList(MosList * list, MosList * elm_add) {
    // AddToListAfter <=> AddToFrontOfList if used on list rather than element
    MosAddToListAfter(list, elm_add);
}
MOS_ISR_SAFE void MosRemoveFromList(MosList * elm_rem);
MOS_ISR_SAFE void MosMoveToEndOfList(MosList * elm_exist, MosList * elm_move);
MOS_ISR_SAFE static MOS_INLINE bool MosIsLastElement(MosList * list, MosList * elm) {
    return (list->prev == elm);
}
MOS_ISR_SAFE static MOS_INLINE bool MosIsListEmpty(MosList * list) {
    return (list->prev == list);
}
MOS_ISR_SAFE static MOS_INLINE bool MosIsOnList(MosList * elm) {
    return (elm->prev != elm);
}

#endif
