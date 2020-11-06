
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

void MosInitList(MosList * list); // IS
void MosInitListElm(MosListElm * elm, u32 type); // IS
void MosAddToList(MosList * list, MosList * elm_add); // IS
static void MOS_INLINE
MosAddToListBefore(MosList * elm_exist, MosList * elm_add) { // IS
    // AddToList <=> AddToListBefore if used on element rather than list
    MosAddToList(elm_exist, elm_add);
}
void MosAddToListAfter(MosList * elm_exist, MosList * elm_add); // IS
static void MOS_INLINE
MosAddToFrontOfList(MosList * list, MosList * elm_add) { // IS
    // AddToListAfter <=> AddToFrontOfList if used on list rather than element
    MosAddToListAfter(list, elm_add);
}
void MosRemoveFromList(MosList * elm_rem); // IS
void MosMoveToEndOfList(MosList * elm_exist, MosList * elm_move); // IS
static bool MOS_INLINE MosIsLastElement(MosList * list, MosList * elm) { // IS
    return (list->prev == elm);
}
static bool MOS_INLINE MosIsListEmpty(MosList * list) { // IS
    return (list->prev == list);
}
static bool MOS_INLINE MosIsOnList(MosList * elm) { // IS
    return (elm->prev != elm);
}

#endif
