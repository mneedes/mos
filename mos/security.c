
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Security
//

#include <mos/security.h>

static MosSem SecureContextCounter;

void MosSecurityInit(u32 num_contexts) {
    MosInitSem(&SecureContextCounter, num_contexts);
}

void MosReserveSecureContext(void) {
    MosWaitForSem(&SecureContextCounter);
}

bool MosTryReserveSecureContext(void) {
    if (MosTrySem(&SecureContextCounter)) {
        return true;
    }
    return false;
}

void MosReleaseSecureContext(void) {
    MosIncrementSem(&SecureContextCounter);
}
