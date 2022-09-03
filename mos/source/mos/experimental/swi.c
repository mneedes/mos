
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#include <mos/kernel.h>

void MOS_NAKED SVC_Handler(void) {
    asm volatile (
        ""
    );
}

MOS_ISR_SAFE void MosKickOffBackEnd(MosThread * thread) {
    // Put thread on queue
    asm volatile ( "svc 1" );
}
