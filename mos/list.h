
//  Copyright 2020-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#ifndef _MOS_LIST_H_
#define _MOS_LIST_H_

#include <mos/defs.h>

// Doubly-Linked Lists (idea borrowed from famous OS)

// List descriptor and link*
typedef struct MosList {
    struct MosList * prev;
    struct MosList * next;
} MosList;

typedef MosList MosLink;

MOS_ISR_SAFE void MosInitList(MosList * list);
MOS_ISR_SAFE void MosAddToList(MosList * list, MosList * elm_add);
static MOS_INLINE MOS_ISR_SAFE void
MosAddToListBefore(MosList * elm_exist, MosList * elm_add) {
    // AddToList <=> AddToListBefore if used on element rather than list
    MosAddToList(elm_exist, elm_add);
}
void MOS_ISR_SAFE MosAddToListAfter(MosList * elm_exist, MosList * elm_add);
static MOS_INLINE MOS_ISR_SAFE void
MosAddToFrontOfList(MosList * list, MosList * elm_add) {
    // AddToListAfter <=> AddToFrontOfList if used on list rather than element
    MosAddToListAfter(list, elm_add);
}
void MOS_ISR_SAFE MosRemoveFromList(MosList * elm_rem);
void MOS_ISR_SAFE MosMoveToEndOfList(MosList * elm_exist, MosList * elm_move);
static MOS_INLINE MOS_ISR_SAFE bool MosIsLastElement(MosList * list, MosList * elm) {
    return (list->prev == elm);
}
static MOS_INLINE MOS_ISR_SAFE bool MosIsListEmpty(MosList * list) {
    return (list->prev == list);
}
static MOS_INLINE MOS_ISR_SAFE bool MosIsOnList(MosList * elm) {
    return (elm->prev != elm);
}

#endif
