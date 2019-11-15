
//  Copyright 2019 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS Low-Overhead Deterministic Heaps
//

#ifndef _MOSH_H_
#define _MOSH_H_

#define MOSH_HEAP_ALIGNED      MOS_STACK_ALIGNED
#define MOSH_HEAP_ALIGNMENT    MOS_STACK_ALIGNMENT

typedef struct {
    u32 bs;
    MosList fl;
    MosList bsl;
} MoshBlockSize;

// Low-overhead heap
typedef struct {
    MosMutex mtx;
    u8 *pit;
    u8 *bot;
    MosList bsl;
    MosList bsl_free;
    MosList sl;
    MoshBlockSize bs[MOSH_MAX_HEAP_BLOCK_SIZES];
} MoshHeap;

void MoshInitHeap(MoshHeap * heap, u8 * pit, u32 len);
bool MoshReserveBlockSize(MoshHeap * heap, u32 bs);
// Allocate block of a reserved block size
void *MoshAllocBlock(MoshHeap * heap, u32 size);
void MoshFreeBlock(MoshHeap * heap, void *block);
// Allocate block that will never be returned to pit, size does not have to
//   conform to reserved block sizes.
void *MoshAllocForever(MoshHeap * heap, u32 bs);
// Allocate short-lived block that should be returned within a tick or so,
//   size does not have to conform to reserved block sizes.  Can result
//   in fragmentation.
void *MoshAllocShortLived(MoshHeap * heap, u32 bs);
void MoshFreeShortLived(MoshHeap * heap, void * block);

#endif
