
// Copyright 2020-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/list.h>

void MosInitList(MosList * list) {
    list->prev = list;
    list->next = list;
}

void MosInitLinkHet(MosLinkHet * link, u32 type) {
    link->link.prev = &link->link;
    link->link.next = &link->link;
    link->type = type;
}

void MosAddToList(MosList * list, MosList * elm_add) {
    elm_add->prev = list->prev;
    elm_add->next = list;
    list->prev->next = elm_add;
    list->prev = elm_add;
}

void MosAddToListAfter(MosList * list, MosList * elm_add) {
    elm_add->prev = list;
    elm_add->next = list->next;
    list->next->prev = elm_add;
    list->next = elm_add;
}

void MosRemoveFromList(MosList * elm_rem) {
    elm_rem->next->prev = elm_rem->prev;
    elm_rem->prev->next = elm_rem->next;
    // For MosIsElementOnList() and safety
    elm_rem->prev = elm_rem;
    elm_rem->next = elm_rem;
}

void MosMoveToEndOfList(MosList * elm_exist, MosList * elm_move) {
    // Remove element
    elm_move->next->prev = elm_move->prev;
    elm_move->prev->next = elm_move->next;
    // Add to end of list
    elm_move->prev = elm_exist->prev;
    elm_move->next = elm_exist;
    elm_exist->prev->next = elm_move;
    elm_exist->prev = elm_move;
}
