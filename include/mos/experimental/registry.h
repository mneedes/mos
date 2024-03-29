// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/registry.h
/// \brief Registry (Prefix Tree)

#ifndef _MOS_REGISTRY_H_
#define _MOS_REGISTRY_H_

#include <mos/kernel.h>
#include <mos/allocator.h>

typedef u8 MosEntryType;
enum MosEntryType {
    MosEntryTypeEmpty      = 'm',
    MosEntryTypeInternal   = 'n',
    MosEntryTypeString     = 's',
    MosEntryTypeBinary     = 'b',
    MosEntryTypeInteger    = 'i',
    MosEntryTypeExternalIF = 'e'
};

// Handle to a registry entry (i.e.: tree node)
//  NULL refers to the root entry.
typedef void * MosEntry;

typedef bool (MosRegistrySetFunc)(char * val, u32 n);
typedef bool (MosRegistryGetFunc)(const char * val);

typedef struct {
    MosRegistrySetFunc  * SetFunc;
    MosRegistryGetFunc  * GetFunc;
} MosRegistryExternalInterface;

/// Initialize registry, returns root entry
///
MosEntry mosRegistryInit(MosHeap * heap, char delimiter);

/// Get handle to tree entry given path
/// \note The entry is handle to the root of the sub-tree and the path is a relative path from there.
/// \note MosEntry of NULL refers to the root entry.
MosEntry mosFindEntry(MosEntry root, const char * path);

/// Obtain type of entry
///
MosEntryType mosGetEntryType(MosEntry entry);

/// Set string value
///
bool mosSetStringEntry(MosEntry root, const char * path, const char * data);

/// Get string value
///
bool mosGetStringEntry(MosEntry root, const char * path, char * data, u32 * size);

/// Set binary value
///
bool mosSetBinaryEntry(MosEntry root, const char * path, const u8 * data, u32 size);

/// Get string value
///
bool mosGetBinaryEntry(MosEntry root, const char * path, u8 * data, u32 * size);

/// Set integer value
///
bool mosSetIntegerEntry(MosEntry root, const char * path, const s64 data);

/// Get integer value
///
bool mosGetIntegerEntry(MosEntry root, const char * path, s64 * data);

/// Set an external interface on a registry leaf entry
///
bool mosSetExternalInterface(MosEntry root, const char * path);

/// Print a setting value (TODO: maybe use snprintf() equivalent)
///
void mosPrintEntryAsString(MosEntry entry, void (*PrintfFunc)(const char *, ...));

/// Set a value using a string
///
void mosSetEntryWithString(MosEntry entry, const char * value);

#endif
