
// Copyright 2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

// TBD: Work in progress, an ambitious (misguided?) attempt to unify raw flash and filesystems in one API.

// Context Name:  "<:deviceNum:>partition<:file>"
//  If <:DeviceNum:> is omitted, device zero is assumed.
//  If <:file> is omitted, refers to entire partition.

#ifndef _MOS_FLASH_H
#define _MOS_FLASH_H

typedef struct {
    char name[16];
    char type[16];
} MosPartitionTableEntry;

typedef struct {
    MosPartitionTableEntry * pEntry;
} MosPartition;

typedef struct {
    MosPartition  * pPartition;
    u8            * pPrivate;
    u32             sizeInBytes;             //< Total size in bytes
    u32             startByteOffset;         //< Starting byte offset in flash
    u32             sectorSize;              //< Size of flash sector
    u32             numSectors;              //< Number of sectors
    u32             currentReadByteOffest;   //< Read offset in context
    u32             currentWriteByteOffest;  //< Write offset in context
    u16             writeAlignment;          //< Required write alignment
    u8              deviceNum;               //< Device number */
} MosFlashContext;

typedef enum {
    MosFlashStatus_Ok,             //< Success
    MosFlashStatus_NoSuchContext,  //< Cannot resolve flash context
    MosFlashStatus_EraseError,     //< Error erasing flash
    MosFlashStatus_ReadError,      //< Error reading flash
    MosFlashStatus_WriteError,     //< Error writing flash
    MosFlashStatus_ReadOverflow,   //< Read overflow
    MosFlashStatus_WriteOverflow,  //< Write overflow
    MosFlashStatus_OutOfMemory     //< Memory allocation error
} MosFlashStatus;

/**************************** DEVICE INTERFACE **********************************/

/// Initialize the flash subsystem
///
void mosInitFlash(void);

/// Create a flash context for accessing a flash partition or file.
/// Find file or partition on device number, use -1 to search all devices.
MosFlashContext * mosFlashCreateContext(const char * pContextName, s32 deviceNum);

/// Destroy a flash context.
///
void mosFlashDestroyContext(MosFlashContext * pContext);

/// Stream read from flash context.
///
MosFlashStatus mosFlashRead(MosFlashContext * pContext, u8 * pData, u32 numBytes, bool decrypt);

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

/**************************** DRIVER INTERFACE **********************************/

MosFlashStatus mosFlashDriverGetParams(u32 deviceNum, u32 * nWriteAlignment, u32 * nSectorSize);

/// Flash read
///
MosFlashStatus mosFlashDriverRead(u32 deviceNum, u8 * pData, u32 numBytes, bool decrypt);

/// Flash write
///
MosFlashStatus mosFlashDriverWrite(u32 deviceNum, const u8 * pData, u32 numBytes, bool encrypt);

#endif

