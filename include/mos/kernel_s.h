
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Microkernel Secure-side Implementation
//

#ifndef _MOS_KERNEL_S_H_
#define _MOS_KERNEL_S_H_

MOS_ISR_SAFE MOS_INLINE bool S_mosTT(const void * pAddress) {
    u32 tt;
    asm volatile (
        "tt %0, %1"
            : "=r" (tt) : "r" (pAddress) :
    );
    return tt;
}

/// Determine if address is from a non-secure area.
///   All pointers passed in from the Non-Secure side should be validated as Non-Secure before use.
MOS_ISR_SAFE MOS_INLINE bool S_mosIsAddressNonSecure(const void * pAddress) {
    u32 perms = S_mosTT(pAddress);
    return ((perms & (0x1 << 22)) == 0x0);
}

/// Determine if address range is non-secure by examining end-points.
///   All pointers passed in from the Non-Secure side should be validated as Non-Secure before use.
///   e.g.:  S_MosIsAddressRangeNonSecure(pStruct, sizeof(Struct))
MOS_ISR_SAFE MOS_INLINE bool S_mosIsAddressRangeNonSecure(const void * pAddress, mos_size size) {
    u32 perms_b = S_mosTT(pAddress);
    u32 perms_e = S_mosTT((u8 *)pAddress + size - 1);
    return (perms_b == perms_e) && ((perms_b & (0x1 << 22)) == 0x0);
}

#endif
