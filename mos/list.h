
//  Copyright 2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#ifndef _MOS_LIST_H_
#define _MOS_LIST_H_

#include <mos/defs.h>

// Doubly-Linked Lists (idea borrowed from famous OS)

// List descriptor and link*
//   (*for homogeneous lists)
typedef struct MosList {
    struct MosList * prev;
    struct MosList * next;
} MosList;

// List link for heterogeneous lists
typedef struct {
    MosList link;
    u32 type;
} MosListElm;

void MOS_ISR_SAFE MosInitList(MosList * list);
void MOS_ISR_SAFE MosInitListElm(MosListElm * elm, u32 type);
void MOS_ISR_SAFE MosAddToList(MosList * list, MosList * elm_add);
static void MOS_INLINE MOS_ISR_SAFE
MosAddToListBefore(MosList * elm_exist, MosList * elm_add) {
    // AddToList <=> AddToListBefore if used on element rather than list
    MosAddToList(elm_exist, elm_add);
}
void MOS_ISR_SAFE MosAddToListAfter(MosList * elm_exist, MosList * elm_add);
static void MOS_INLINE MOS_ISR_SAFE
MosAddToFrontOfList(MosList * list, MosList * elm_add) {
    // AddToListAfter <=> AddToFrontOfList if used on list rather than element
    MosAddToListAfter(list, elm_add);
}
void MOS_ISR_SAFE MosRemoveFromList(MosList * elm_rem);
void MOS_ISR_SAFE MosMoveToEndOfList(MosList * elm_exist, MosList * elm_move);
static bool MOS_INLINE MOS_ISR_SAFE MosIsLastElement(MosList * list, MosList * elm) {
    return (list->prev == elm);
}
static bool MOS_INLINE MOS_ISR_SAFE MosIsListEmpty(MosList * list) {
    return (list->prev == list);
}
static bool MOS_INLINE MOS_ISR_SAFE MosIsOnList(MosList * elm) {
    return (elm->prev != elm);
}

#endif
