
//  Copyright 2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// MOS Thread Library
//   Dynamic thread interface
//

#ifndef _MOS_THREAD_HEAP_H_
#define _MOS_THREAD_HEAP_H_

#include "mos/kernel.h"
#include "mos/heap.h"

// auto_free means thread resources are freed in the idle task after threads stop
void MosSetThreadHeap(MosHeap * heap, bool auto_free);
MosThread * MosAllocThread(u32 stack_size);
MosThread * MosAllocAndRunThread(MosThreadPriority pri, MosThreadEntry * entry,
                                 s32 arg, u32 stack_size);
// NOTE: Cannot free running thread
void MosFreeThread(MosThread * thd);

#endif
