
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// FIFO
//

#include <mos/fifo.h>

void mosInitFIFO32(MosFIFO32 * fifo, u32 * buf, u32 len) {
    fifo->buf = buf;
    fifo->len = len;
    fifo->tail = 0;
    fifo->head = 0;
}

bool mosWriteToFIFO32(MosFIFO32 * fifo, u32 data) {
    u32 new_tail = fifo->tail + 1;
    if (new_tail >= fifo->len) new_tail = 0;
    if (fifo->head == new_tail) return false;
    fifo->buf[fifo->tail] = data;
    asm volatile ( "dmb" );
    fifo->tail = new_tail;
    return true;
}

bool mosReadFromFIFO32(MosFIFO32 * fifo, u32 * data) {
    if (fifo->head == fifo->tail) return false;
    *data = fifo->buf[fifo->head];
    asm volatile ( "dmb" );
    if (fifo->head + 1 >= fifo->len) fifo->head = 0;
    else fifo->head++;
    return true;
}

bool mosSnoopFIFO32(MosFIFO32 * fifo, u32 * data) {
    if (fifo->head == fifo->tail) return false;
    *data = fifo->buf[fifo->head];
    return true;
}
