
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel Secure-side Implementation
//

#ifndef _MOS_KERNEL_S_H_
#define _MOS_KERNEL_S_H_

/// Determine if address is from a non-secure area.
///   All pointers passed in from the Non-Secure side should be validated as Non-Secure before use.
MOS_ISR_SAFE MOS_INLINE bool S_MosIsAddressNonSecure(void * address) {
    u32 perms;
    asm volatile (
        "tt %0, %1"
            : "=r" (perms) : "r" (address) :
    );
    return ((perms & (0x1 << 22)) == 0x0);
}

/// Determine if address range is non-secure by examining end-points.
///   All pointers passed in from the Non-Secure side should be validated as Non-Secure before use.
///   e.g.:  S_MosIsAddressRangeNonSecure(pStruct, sizeof(Struct))
MOS_ISR_SAFE MOS_INLINE bool S_MosIsAddressRangeNonSecure(void * address, mos_size size) {
    return (S_MosIsAddressNonSecure(address) && S_MosIsAddressNonSecure((u8 *)address + size - 1));
}

#endif
