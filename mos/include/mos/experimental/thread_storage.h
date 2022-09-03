
// Copyright 2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#ifndef _MOS_THREAD_STORAGE_H
#define _MOS_THREAD_STORAGE_H

/// Get a unique ID
///
u32 MosGetUniqueID(void);

typedef void (MosThreadStorageReleaseFunc)(void * pStorage);

/// Obtain local storage for the current thread with the given size
///   (Re)allocating if the size is different from before.
///   Calls the optional callback function in idle context if thread
///   dies before MosThreadStorageReturn is called.
void * MosThreadStorageObtain(u32 unique_id, mos_size size, MosThreadStorageReleaseFunc * pRelease);

/// Return thread local storage
///   This should be called before the library exits
void * MosThreadStorageReturn(u32 unique_id, void * pStorage);

#endif

