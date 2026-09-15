/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * zarchive_structs.h: ZArchive data structures.                           *
 *                                                                         *
 * Copyright (c) 2022 by Exzap.                                            *
 * Copyright (c) 2026 by David Korth.                                      *
 * SPDX-License-Identifier: MIT-0                                          *
 ***************************************************************************/

#pragma once

#include <stdint.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define ZARCHIVE_MAGIC   0x169F52D6
#define ZARCHIVE_VERSION 0x61BF3A01

#define ZARCHIVE_COMPRESSED_BLOCK_SIZE (64 * 1024)
#define ZARCHIVE_ENTRIES_PER_OFFSETRECORD 16
#define ZARCHIVE_INVALID_NODE 0xFFFFFFFFU

#pragma pack(1)

/**
 * ZArchive section offset and size info.
 * All fields are big-endian.
 */
typedef struct RP_PACKED _ZArchive_OffsetInfo {
	uint64_t offset;
	uint64_t size;
} ZArchive_OffsetInfo;
ASSERT_STRUCT(ZArchive_OffsetInfo, 16);

/**
 * ZArchive footer (144 bytes located at the end of the file).
 * All fields are big-endian.
 */
typedef struct RP_PACKED _ZArchive_Footer {
	ZArchive_OffsetInfo sectionCompressedData;
	ZArchive_OffsetInfo sectionOffsetRecords;
	ZArchive_OffsetInfo sectionNames;
	ZArchive_OffsetInfo sectionFileTree;
	ZArchive_OffsetInfo sectionMetaDirectory;
	ZArchive_OffsetInfo sectionMetaData;
	uint8_t integrityHash[32];
	uint64_t totalSize;
	uint32_t version;
	uint32_t magic;
} ZArchive_Footer;
ASSERT_STRUCT(ZArchive_Footer, 144);

/**
 * ZArchive compression offset record.
 * All fields are big-endian.
 */
typedef struct RP_PACKED _ZArchive_CompressionOffsetRecord {
	uint64_t baseOffset;
	uint16_t size[ZARCHIVE_ENTRIES_PER_OFFSETRECORD];
} ZArchive_CompressionOffsetRecord;
ASSERT_STRUCT(ZArchive_CompressionOffsetRecord, 40);

/**
 * ZArchive file/directory tree entry.
 * All fields are big-endian.
 */
typedef struct RP_PACKED _ZArchive_FileDirectoryEntry {
	uint32_t nameOffsetAndTypeFlag; // MSB is type: 0 -> dir, 1 -> file. Lower 31 bits are name offset.
	union {
		struct {
			uint32_t fileOffsetLow;
			uint32_t fileSizeLow;
			uint32_t fileOffsetAndSizeHigh; // High 16 bits -> fileSize extension, Low 16 bits -> fileOffset extension
		} fileRecord;
		struct {
			uint32_t nodeStartIndex;
			uint32_t count;
			uint32_t _reserved;
		} directoryRecord;
	};
} ZArchive_FileDirectoryEntry;
ASSERT_STRUCT(ZArchive_FileDirectoryEntry, 16);

#pragma pack()

#ifdef __cplusplus
}
#endif /* __cplusplus */
