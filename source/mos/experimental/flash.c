
// Copyright 2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

// TODO: Work in progress

#include <mos/allocator.h>
#include <mos/experimental/flash.h>

void mosInitFlash(void) {
    /* Enumerate driver contexts */
}

MosFlashStatus
mosInitFlashContext(MosFlashContext * pContext, const char * pContextName, s32 deviceNumber) {
    return 0;
}

/// Destroy a flash context.
///
void mosFlashClose(MosFlashContext * pContext) {
}

#if 0
/// Stream read from flash context.
///
MosFlashStatus mosFlashRead(MosFlashContext * pContext, u8 * pData, u32 numBytes, bool decrypt)

/// Stream write to flash context.
///
MosFlashStatus mosFlashWrite(MosFlashContext * pContext, const u8 * pData, u32 numBytes, bool encrypt);

/// Flush stream writes.
///
MosFlashStatus mosFlashWriteFlush(MosFlashContext * pContext);

/// Adjust flash read context.
///
MosFlashStatus mosAdjustReadContext(MosFlashContext * pContext, s32 delta, u32 absolute);

/// Adjust flash write context.
///
MosFlashStatus mosAdjustWriteContext(MosFlashContext * pContext, s32 delta, u32 absolute);

/// Erase the flash corresponding to the flash context.
///
MosFlashStatus mosEraseContext(MosFlashContext * pContext);

/// Erase a single sector within the flash context.
///
MosFlashStatus mosEraseSector(MosFlashContext * pContext, u32 sectorOffset);

#endif

