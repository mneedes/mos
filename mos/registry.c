
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Registry Tree
//

#include <string.h>

#include <mos/registry.h>
#include <mos/list.h>

#include <mos/trace.h>

typedef struct Entry {
    MosLink  link;
    union {
        s64     int_value;     // integer value
        MosList entries;       // child list
        struct {
            u8 * data;
            u32  size;         // blob size (includes '\0' for string)
        } blob;                // blob value (e.g: binary/string)
        struct {
            void * interface;
            void * priv_data;
        } external;            // external interface
    };
    MosEntryType type;
    u8 rsvd[3];
    // Payload {
    //   char name[];          // Entry name
    //   u8 data[];            // Blob data
    // }
} Entry;

typedef struct Entry2 {
    MosLink  link;             // tree link
    MosLink  op_link;          // operation link
    union {
        s64     int_value;     // integer value
        MosList entries;       // child list
        struct {
            u8 * data;         // pointer to blob data
            u32  size;         // blob size (includes '\0' for string)
        } blob;                // blob value (e.g: binary/string)
        struct {
            void * interface;
            void * priv_data;
        } external;            // external interface
    };
    MosEntryType type;
    u8 rsvd[3];
    u32 offset;                // offset in external storage
    // Payload {
    //   char name[];          // Entry name
    // }
} Entry2;

typedef struct Registry {
    MosMutex  mutex;
    Entry   * root;
    MosHeap * heap;
    char      delimiter;
} Registry;

static Registry reg;

static const char * MatchEntryName(char * entry_name, const char * path_name) {
    u32 idx = 0;
    while (1) {
        /* Path name ends at \0 or delimiter, entry ends at \0 */
        if (path_name[idx] == '\0' || path_name[idx] == reg.delimiter) {
            if (entry_name[idx] == '\0') return (path_name + idx);
            else break;
        }
        if (entry_name[idx] != path_name[idx]) break;
        idx++;
    }
    return NULL;
}

static Entry * FindEntry2(Entry * entry, const char ** path, bool * leaf_found) {
    if (!entry) entry = reg.root;
    if (**path == '\0') {
        *leaf_found = false;
        return entry;
    }
FindNext:
    if (entry->type != MosEntryTypeInternal) return NULL;
    MosLink * elm = entry->entries.next;
    for (; elm != &entry->entries; elm = elm->next) {
        Entry * check_entry = container_of(elm, Entry, link);
        const char * matched_path = MatchEntryName((char *)(check_entry + 1), *path);
        if (matched_path) {
            if (*matched_path == '\0') {
                *leaf_found = true;
                return check_entry;
            }
            else {
                *path = matched_path + 1;
                entry = check_entry;
                goto FindNext;
            }
        }
    }
    *leaf_found = false;
    return entry;
}

static Entry * AllocAndFillEntry(const char ** _path, const u8 * data, u32 blob_size) {
    const char * path = *_path;
    u32 name_size = 0;
    while (path[name_size] != '\0' && path[name_size] != reg.delimiter) name_size++;
    *_path = path + name_size;
    u32 entry_size = sizeof(Entry) + name_size + 1;
    if (data && **_path == '\0') entry_size += blob_size;
    Entry * entry = (Entry *)MosAlloc(reg.heap, entry_size);
    if (entry) {
        u8 * buf = (u8 *)(entry + 1);
        memcpy(buf, (u8 *)path, name_size);
        buf[name_size] = '\0';
        if (data && **_path == '\0') {
            /* Node is for data blobs AND this is the end of path */
            buf += name_size + 1;
            entry->blob.data = buf;
            entry->blob.size = blob_size;
            memcpy(buf, data, blob_size);
        }
    }
    return entry;
}

static Entry * FindEntry(Entry * entry, const char * path) {
    if (!entry) entry = reg.root;
    if (*path == '\0') return entry;
FindNext:
    if (entry->type != MosEntryTypeInternal) return NULL;
    MosLink * elm = entry->entries.next;
    for (; elm != &entry->entries; elm = elm->next) {
        Entry * check_entry = container_of(elm, Entry, link);
        const char * matched_path = MatchEntryName((char *)(check_entry + 1), path);
        if (matched_path) {
            if (*matched_path == '\0') return check_entry;
            else {
                path = matched_path + 1;
                entry = check_entry;
                goto FindNext;
            }
        }
    }
    return NULL;
}

static Entry * CreateEntry(Entry * entry, const char * path, const u8 * data, u32 blob_size) {
#if 0
    if (!entry) entry = reg.root;
    if (*path == '\0') return entry;
FindNext: {
        if (entry->type != MosEntryTypeInternal) return NULL;
        MosLink * elm = entry->entries.next;
        for (; elm != &entry->entries; elm = elm->next) {
            Entry * check_entry = container_of(elm, Entry, link);
            const char * matched_path = MatchEntryName((char *)(check_entry + 1), path);
            if (matched_path) {
                if (*matched_path == '\0') return check_entry;  // If Entry already exists...
                else {
                    path = matched_path + 1;
                    entry = check_entry;
                    goto FindNext;
                }
            }
        }
    }
#endif
    bool leaf_found;
    entry = FindEntry2(entry, &path, &leaf_found);
    if (!entry) return NULL;
    if (leaf_found) return entry;
    Entry * new_entry;
    while (1) {
        new_entry = AllocAndFillEntry(&path, data, blob_size);
        if (new_entry) {
            MosAddToList(&entry->entries, &new_entry->link);
            if (*path++ == '\0') break;
            new_entry->type = MosEntryTypeInternal;
            MosInitList(&new_entry->entries);
            entry = new_entry;
        } else break;
    }
    return new_entry;
}

MosEntry MosRegistryInit(MosHeap * heap, char delimiter) {
    MosInitMutex(&reg.mutex);
    reg.heap      = heap;
    reg.delimiter = delimiter;
    reg.root      = (Entry *)MosAlloc(reg.heap, sizeof(Entry));
    if (reg.root) {
        reg.root->type = MosEntryTypeInternal;
        MosInitList(&reg.root->entries);
    }
    MosPrintf("SiZE: %u\n", sizeof(Entry));
    return (MosEntry)reg.root;
}

MosEntry MosFindEntry(MosEntry root, const char * path) {
    MosLockMutex(&reg.mutex);
    MosEntry entry = FindEntry((Entry *)root, path);
    MosUnlockMutex(&reg.mutex);
    return entry;
}

// TODO: replace a value at a entry...

bool MosSetStringEntry(MosEntry root, const char * path, const char * str) {
    bool success = false;
    MosLockMutex(&reg.mutex);
    Entry * entry = (Entry *)CreateEntry((Entry *)root, path, (const u8 *)str, strlen(str) + 1);
    if (entry) {
        entry->type = MosEntryTypeString;
        success = true;
    }
    MosUnlockMutex(&reg.mutex);
    return success;
}

bool MosGetStringEntry(MosEntry root, const char * path, char * data, u32 * size) {
    bool success = false;
    MosLockMutex(&reg.mutex);
    Entry * entry = FindEntry((Entry *)root, path);
    if (entry && entry->type == MosEntryTypeString) {
        if (*size >= entry->blob.size) {
            memcpy(data, entry->blob.data, entry->blob.size);
            success = true;
        }
        *size = entry->blob.size;
    }
    MosUnlockMutex(&reg.mutex);
    return success;
}

#if 0
bool MosSetIntegerEntry(MosEntry root, const char * path, const s64 data) {
    bool success = false;
    MosLockMutex(&reg.mutex);
    Entry * entry = FindEntry((Entry *)root, path);
    if (entry && entry->type == MosEntryTypeInteger) {
        *data = entry->int_value;
        success = true;
    }
    MosUnlockMutex(&reg.mutex);
    return success;
}
#endif

bool MosGetIntegerEntry(MosEntry root, const char * path, s64 * data) {
    bool success = false;
    MosLockMutex(&reg.mutex);
    Entry * entry = FindEntry((Entry *)root, path);
    if (entry && entry->type == MosEntryTypeInteger) {
        *data = entry->int_value;
        success = true;
    }
    MosUnlockMutex(&reg.mutex);
    return success;
}

#if 0

bool MosPrintEntryAsString(MosEntry entry, (*PrintfFunc)(const char *, ...)) {
    MosLockMutex(&reg.mutex);
    MosUnlockMutex(&reg.mutex);
}

bool MosSetEntryWithString(MosEntry entry, const char * value) {
    MosLockMutex(&reg.mutex);
    MosUnlockMutex(&reg.mutex);
}

#endif
