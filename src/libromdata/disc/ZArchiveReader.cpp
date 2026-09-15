/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * ZArchiveReader.cpp: ZArchive reader.                                    *
 *                                                                         *
 * Copyright (c) 2022 by Exzap.                                            *
 * Copyright (c) 2026 by David Korth.                                      *
 * SPDX-License-Identifier: MIT-0                                          *
 ***************************************************************************/

#include "config.librpbase.h"
#include "ZArchiveReader.hpp"
#include "ZArchiveFile.hpp"
#include "zarchive_structs.h"

// Byte swapping
#include "librpbyteswap/byteswap_rp.h"

// C++ STL classes
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// zstd
#include <zstd.h>
#ifdef _MSC_VER
#  include "libwin32common/DelayLoadHelper.h"
#endif /* _MSC_VER */

namespace LibRomData {

#if defined(_MSC_VER) && defined(ZSTD_IS_DLL)
// DelayLoad test implementation.
DELAYLOAD_TEST_FUNCTION_IMPL1(ZSTD_freeDCtx, nullptr);
#endif /* _MSC_VER && ZSTD_IS_DLL */

static constexpr size_t ZARCHIVE_CACHE_BLOCK_COUNT = 16;

class ZArchiveReaderPrivate
{
public:
	explicit ZArchiveReaderPrivate(const LibRpFile::IRpFilePtr &file);
	~ZArchiveReaderPrivate() = default;

public:
	RP_DISABLE_COPY(ZArchiveReaderPrivate)

public:
	struct CacheBlock {
		std::vector<uint8_t> data;
		uint64_t blockIndex;
		CacheBlock *prev;
		CacheBlock *next;

		CacheBlock()
			: data(ZARCHIVE_COMPRESSED_BLOCK_SIZE)
			, blockIndex(0xFFFFFFFFFFFFFFFFULL)
			, prev(nullptr)
			, next(nullptr)
		{}
	};

	LibRpFile::IRpFilePtr file;
	bool isOpen;
	int lastError;

	ZArchive_Footer footer;
	std::vector<ZArchive_CompressionOffsetRecord> offsetRecords;
	std::vector<uint8_t> nameTable;
	std::vector<ZArchive_FileDirectoryEntry> fileTree;

	uint64_t compressedDataOffset;
	uint64_t compressedDataSize;
	uint64_t blockCount;

	std::mutex accessMutex;
	std::vector<CacheBlock> cacheBlocks;
	CacheBlock *lruChainFirst;
	CacheBlock *lruChainLast;
	std::unordered_map<uint64_t, CacheBlock*> blockLookup;
	std::vector<uint8_t> decompressionBuffer;

public:
	CacheBlock *getCachedBlock(uint64_t blockIndex);
	CacheBlock *recycleLRUBlock(uint64_t newBlockIndex);
	void markBlockAsMRU(CacheBlock *block);
	void registerBlock(CacheBlock *block, uint64_t blockIndex);
	void unregisterBlock(CacheBlock *block);
	bool loadBlock(CacheBlock *block);

	static std::string_view getName(const std::vector<uint8_t> &nameTable, uint32_t nameOffset);
	static bool getNextPathNode(std::string_view &pathParser, std::string_view &node);
	static bool compareNodeName(std::string_view n1, std::string_view n2);
};

ZArchiveReaderPrivate::ZArchiveReaderPrivate(const LibRpFile::IRpFilePtr &file)
	: file(file)
	, isOpen(false)
	, lastError(0)
	, compressedDataOffset(0)
	, compressedDataSize(0)
	, blockCount(0)
	, cacheBlocks(ZARCHIVE_CACHE_BLOCK_COUNT)
	, lruChainFirst(nullptr)
	, lruChainLast(nullptr)
	, decompressionBuffer(ZSTD_compressBound(ZARCHIVE_COMPRESSED_BLOCK_SIZE))
{
#if defined(_MSC_VER) && defined(ZSTD_IS_DLL)
	int err = DelayLoad_test_ZSTD_freeDCtx();
	if (err != 0) {
		lastError = -err;
		return;
	}
#endif /* _MSC_VER && ZSTD_IS_DLL */

	if (!file || !file->isOpen()) {
		lastError = EBADF;
		return;
	}

	const off64_t szFile = file->size();
	if (szFile < static_cast<off64_t>(sizeof(ZArchive_Footer))) {
		lastError = EIO;
		return;
	}

	// Read footer
	ZArchive_Footer rawFooter;
	size_t readBytes = file->seekAndRead(szFile - sizeof(ZArchive_Footer), &rawFooter, sizeof(rawFooter));
	if (readBytes != sizeof(rawFooter)) {
		lastError = file->lastError() != 0 ? file->lastError() : EIO;
		return;
	}

	if (!ZArchiveReader::isZArchive_static(reinterpret_cast<const uint8_t*>(&rawFooter), sizeof(rawFooter), szFile)) {
		lastError = EIO;
		return;
	}

	// Deserialize footer
	footer.sectionCompressedData.offset = be64_to_cpu(rawFooter.sectionCompressedData.offset);
	footer.sectionCompressedData.size   = be64_to_cpu(rawFooter.sectionCompressedData.size);
	footer.sectionOffsetRecords.offset  = be64_to_cpu(rawFooter.sectionOffsetRecords.offset);
	footer.sectionOffsetRecords.size    = be64_to_cpu(rawFooter.sectionOffsetRecords.size);
	footer.sectionNames.offset          = be64_to_cpu(rawFooter.sectionNames.offset);
	footer.sectionNames.size            = be64_to_cpu(rawFooter.sectionNames.size);
	footer.sectionFileTree.offset       = be64_to_cpu(rawFooter.sectionFileTree.offset);
	footer.sectionFileTree.size         = be64_to_cpu(rawFooter.sectionFileTree.size);
	footer.sectionMetaDirectory.offset  = be64_to_cpu(rawFooter.sectionMetaDirectory.offset);
	footer.sectionMetaDirectory.size    = be64_to_cpu(rawFooter.sectionMetaDirectory.size);
	footer.sectionMetaData.offset       = be64_to_cpu(rawFooter.sectionMetaData.offset);
	footer.sectionMetaData.size         = be64_to_cpu(rawFooter.sectionMetaData.size);
	memcpy(footer.integrityHash, rawFooter.integrityHash, sizeof(footer.integrityHash));
	footer.totalSize = be64_to_cpu(rawFooter.totalSize);
	footer.version   = be32_to_cpu(rawFooter.version);
	footer.magic     = be32_to_cpu(rawFooter.magic);

	compressedDataOffset = footer.sectionCompressedData.offset;
	compressedDataSize   = footer.sectionCompressedData.size;

	// Read Offset Records
	if (footer.sectionOffsetRecords.size > 0) {
		if ((footer.sectionOffsetRecords.size % sizeof(ZArchive_CompressionOffsetRecord)) != 0) {
			lastError = EIO;
			return;
		}
		const size_t recordCount = static_cast<size_t>(footer.sectionOffsetRecords.size / sizeof(ZArchive_CompressionOffsetRecord));
		offsetRecords.resize(recordCount);
		readBytes = file->seekAndRead(footer.sectionOffsetRecords.offset, offsetRecords.data(), footer.sectionOffsetRecords.size);
		if (readBytes != footer.sectionOffsetRecords.size) {
			lastError = file->lastError() != 0 ? file->lastError() : EIO;
			return;
		}

		for (auto &rec : offsetRecords) {
			rec.baseOffset = be64_to_cpu(rec.baseOffset);
			for (size_t i = 0; i < ZARCHIVE_ENTRIES_PER_OFFSETRECORD; i++) {
				rec.size[i] = be16_to_cpu(rec.size[i]);
			}
		}
		blockCount = recordCount * ZARCHIVE_ENTRIES_PER_OFFSETRECORD;
	}

	// Read Name Table
	if (footer.sectionNames.size > 0) {
		nameTable.resize(static_cast<size_t>(footer.sectionNames.size));
		readBytes = file->seekAndRead(footer.sectionNames.offset, nameTable.data(), footer.sectionNames.size);
		if (readBytes != footer.sectionNames.size) {
			lastError = file->lastError() != 0 ? file->lastError() : EIO;
			return;
		}
	}

	// Read File Tree
	if (footer.sectionFileTree.size > 0) {
		if ((footer.sectionFileTree.size % sizeof(ZArchive_FileDirectoryEntry)) != 0) {
			lastError = EIO;
			return;
		}
		const size_t entryCount = static_cast<size_t>(footer.sectionFileTree.size / sizeof(ZArchive_FileDirectoryEntry));
		fileTree.resize(entryCount);
		readBytes = file->seekAndRead(footer.sectionFileTree.offset, fileTree.data(), footer.sectionFileTree.size);
		if (readBytes != footer.sectionFileTree.size) {
			lastError = file->lastError() != 0 ? file->lastError() : EIO;
			return;
		}

		for (auto &entry : fileTree) {
			entry.nameOffsetAndTypeFlag = be32_to_cpu(entry.nameOffsetAndTypeFlag);
			// File and directory records have identical field widths (3 x uint32)
			entry.fileRecord.fileOffsetLow = be32_to_cpu(entry.fileRecord.fileOffsetLow);
			entry.fileRecord.fileSizeLow = be32_to_cpu(entry.fileRecord.fileSizeLow);
			entry.fileRecord.fileOffsetAndSizeHigh = be32_to_cpu(entry.fileRecord.fileOffsetAndSizeHigh);
		}
	}

	// Initialize cache LRU chain
	for (size_t i = 0; i < cacheBlocks.size(); i++) {
		cacheBlocks[i].prev = (i > 0) ? &cacheBlocks[i - 1] : nullptr;
		cacheBlocks[i].next = (i + 1 < cacheBlocks.size()) ? &cacheBlocks[i + 1] : nullptr;
	}
	lruChainFirst = &cacheBlocks.front();
	lruChainLast = &cacheBlocks.back();

	isOpen = true;
}

std::string_view ZArchiveReaderPrivate::getName(const std::vector<uint8_t> &nameTable, uint32_t nameOffset)
{
	if (nameOffset == 0x7FFFFFFF || nameOffset >= nameTable.size()) {
		return {};
	}

	uint16_t nameLength = nameTable[nameOffset] & 0x7F;
	if (nameTable[nameOffset] & 0x80) {
		if (nameOffset + 1 >= nameTable.size()) {
			return {};
		}
		nameLength |= (static_cast<uint16_t>(nameTable[nameOffset + 1]) << 7);
		nameOffset += 2;
	} else {
		nameOffset++;
	}

	if (nameOffset + static_cast<size_t>(nameLength) > nameTable.size()) {
		return {};
	}

	return std::string_view(reinterpret_cast<const char*>(nameTable.data() + nameOffset), nameLength);
}

bool ZArchiveReaderPrivate::getNextPathNode(std::string_view &pathParser, std::string_view &node)
{
	while (!pathParser.empty() && (pathParser.front() == '/' || pathParser.front() == '\\')) {
		pathParser.remove_prefix(1);
	}
	if (pathParser.empty()) {
		return false;
	}

	size_t index = 0;
	while (index < pathParser.size() && pathParser[index] != '/' && pathParser[index] != '\\') {
		index++;
	}

	node = pathParser.substr(0, index);
	pathParser.remove_prefix(index);
	return true;
}

bool ZArchiveReaderPrivate::compareNodeName(std::string_view n1, std::string_view n2)
{
	if (n1.size() != n2.size()) {
		return false;
	}
	for (size_t i = 0; i < n1.size(); i++) {
		char c1 = n1[i];
		char c2 = n2[i];
		if (c1 >= 'A' && c1 <= 'Z') c1 += ('a' - 'A');
		if (c2 >= 'A' && c2 <= 'Z') c2 += ('a' - 'A');
		if (c1 != c2) return false;
	}
	return true;
}

ZArchiveReaderPrivate::CacheBlock *ZArchiveReaderPrivate::getCachedBlock(uint64_t blockIndex)
{
	auto it = blockLookup.find(blockIndex);
	if (it != blockLookup.end()) {
		markBlockAsMRU(it->second);
		return it->second;
	}
	if (blockIndex >= blockCount) {
		return nullptr;
	}

	CacheBlock *newBlock = recycleLRUBlock(blockIndex);
	if (!loadBlock(newBlock)) {
		unregisterBlock(newBlock);
		return nullptr;
	}
	return newBlock;
}

ZArchiveReaderPrivate::CacheBlock *ZArchiveReaderPrivate::recycleLRUBlock(uint64_t newBlockIndex)
{
	CacheBlock *recycledBlock = lruChainFirst;
	unregisterBlock(recycledBlock);
	registerBlock(recycledBlock, newBlockIndex);
	markBlockAsMRU(recycledBlock);
	return recycledBlock;
}

void ZArchiveReaderPrivate::markBlockAsMRU(CacheBlock *block)
{
	if (!block->next) {
		return;
	}

	if (!block->prev) {
		lruChainFirst = block->next;
		block->next->prev = nullptr;
	} else {
		block->prev->next = block->next;
		block->next->prev = block->prev;
	}

	block->prev = lruChainLast;
	block->next = nullptr;
	lruChainLast->next = block;
	lruChainLast = block;
}

void ZArchiveReaderPrivate::registerBlock(CacheBlock *block, uint64_t blockIndex)
{
	block->blockIndex = blockIndex;
	blockLookup.emplace(blockIndex, block);
}

void ZArchiveReaderPrivate::unregisterBlock(CacheBlock *block)
{
	if (block->blockIndex != 0xFFFFFFFFFFFFFFFFULL) {
		blockLookup.erase(block->blockIndex);
	}
	block->blockIndex = 0xFFFFFFFFFFFFFFFFULL;
}

bool ZArchiveReaderPrivate::loadBlock(CacheBlock *block)
{
	const uint32_t recordIndex = static_cast<uint32_t>(block->blockIndex / ZARCHIVE_ENTRIES_PER_OFFSETRECORD);
	const uint32_t recordSubIndex = static_cast<uint32_t>(block->blockIndex % ZARCHIVE_ENTRIES_PER_OFFSETRECORD);
	if (recordIndex >= offsetRecords.size()) {
		return false;
	}

	const auto &record = offsetRecords[recordIndex];
	uint64_t offset = record.baseOffset;
	for (uint32_t i = 0; i < recordSubIndex; i++) {
		offset += static_cast<uint64_t>(record.size[i]) + 1;
	}
	const uint32_t compressedSize = static_cast<uint32_t>(record.size[recordSubIndex]) + 1;

	if (offset + compressedSize > compressedDataSize) {
		return false;
	}

	const off64_t physicalOffset = static_cast<off64_t>(compressedDataOffset + offset);
	if (compressedSize == ZARCHIVE_COMPRESSED_BLOCK_SIZE) {
		// Uncompressed block
		size_t read = file->seekAndRead(physicalOffset, block->data.data(), compressedSize);
		return (read == compressedSize);
	}

	if (compressedSize > decompressionBuffer.size()) {
		decompressionBuffer.resize(compressedSize);
	}

	size_t read = file->seekAndRead(physicalOffset, decompressionBuffer.data(), compressedSize);
	if (read != compressedSize) {
		return false;
	}

	size_t decompressedSize = ZSTD_decompress(block->data.data(), ZARCHIVE_COMPRESSED_BLOCK_SIZE,
	                                          decompressionBuffer.data(), compressedSize);
	return (decompressedSize == ZARCHIVE_COMPRESSED_BLOCK_SIZE);
}

/** ZArchiveReader **/

ZArchiveReader::ZArchiveReader(const LibRpFile::IRpFilePtr &file)
	: d_ptr(std::make_unique<ZArchiveReaderPrivate>(file))
{}

ZArchiveReader::~ZArchiveReader() = default;

bool ZArchiveReader::isZArchive_static(const uint8_t *pFooter, size_t szFooter, off64_t szFile)
{
	if (!pFooter || szFooter < sizeof(ZArchive_Footer) || szFile < static_cast<off64_t>(sizeof(ZArchive_Footer))) {
		return false;
	}

	const ZArchive_Footer *const footer = reinterpret_cast<const ZArchive_Footer*>(pFooter);
	if (be32_to_cpu(footer->magic) != ZARCHIVE_MAGIC ||
	    be32_to_cpu(footer->version) != ZARCHIVE_VERSION)
	{
		return false;
	}

	const uint64_t totalSize = be64_to_cpu(footer->totalSize);
	if (totalSize != static_cast<uint64_t>(szFile)) {
		return false;
	}

	const uint64_t maxOffset = totalSize;
	if (be64_to_cpu(footer->sectionCompressedData.offset) + be64_to_cpu(footer->sectionCompressedData.size) > maxOffset ||
	    be64_to_cpu(footer->sectionOffsetRecords.offset)  + be64_to_cpu(footer->sectionOffsetRecords.size)  > maxOffset ||
	    be64_to_cpu(footer->sectionNames.offset)          + be64_to_cpu(footer->sectionNames.size)          > maxOffset ||
	    be64_to_cpu(footer->sectionFileTree.offset)       + be64_to_cpu(footer->sectionFileTree.size)       > maxOffset ||
	    be64_to_cpu(footer->sectionMetaDirectory.offset)  + be64_to_cpu(footer->sectionMetaDirectory.size)  > maxOffset ||
	    be64_to_cpu(footer->sectionMetaData.offset)       + be64_to_cpu(footer->sectionMetaData.size)       > maxOffset)
	{
		return false;
	}

	return true;
}

bool ZArchiveReader::isZArchive(const LibRpFile::IRpFilePtr &file)
{
	if (!file || !file->isOpen()) {
		return false;
	}
	const off64_t szFile = file->size();
	if (szFile < static_cast<off64_t>(sizeof(ZArchive_Footer))) {
		return false;
	}

	ZArchive_Footer footer;
	size_t read = file->seekAndRead(szFile - sizeof(ZArchive_Footer), &footer, sizeof(footer));
	if (read != sizeof(footer)) {
		return false;
	}

	return isZArchive_static(reinterpret_cast<const uint8_t*>(&footer), sizeof(footer), szFile);
}

bool ZArchiveReader::isOpen(void) const
{
	return d_ptr->isOpen;
}

int ZArchiveReader::lastError(void) const
{
	return d_ptr->lastError;
}

uint32_t ZArchiveReader::lookUp(const char *path) const
{
	if (!isOpen() || !path || d_ptr->fileTree.empty()) {
		return ZARCHIVE_INVALID_NODE;
	}

	std::string_view pathParser(path);
	uint32_t currentNode = 0; // Root node is index 0

	while (true) {
		std::string_view nodeName;
		if (!ZArchiveReaderPrivate::getNextPathNode(pathParser, nodeName)) {
			return currentNode;
		}

		if (currentNode >= d_ptr->fileTree.size()) {
			return ZARCHIVE_INVALID_NODE;
		}

		const auto &entry = d_ptr->fileTree[currentNode];
		if ((entry.nameOffsetAndTypeFlag & 0x80000000) != 0) {
			// Current node is a file, cannot traverse into it
			return ZARCHIVE_INVALID_NODE;
		}

		const uint32_t startIndex = entry.directoryRecord.nodeStartIndex;
		const uint32_t endIndex = startIndex + entry.directoryRecord.count;
		if (endIndex > d_ptr->fileTree.size()) {
			return ZARCHIVE_INVALID_NODE;
		}

		uint32_t matchNode = ZARCHIVE_INVALID_NODE;
		for (uint32_t idx = startIndex; idx < endIndex; idx++) {
			const auto &child = d_ptr->fileTree[idx];
			std::string_view childName = ZArchiveReaderPrivate::getName(d_ptr->nameTable, child.nameOffsetAndTypeFlag & 0x7FFFFFFF);
			if (ZArchiveReaderPrivate::compareNodeName(nodeName, childName)) {
				matchNode = idx;
				break;
			}
		}

		if (matchNode == ZARCHIVE_INVALID_NODE) {
			return ZARCHIVE_INVALID_NODE;
		}
		currentNode = matchNode;
	}
}

bool ZArchiveReader::isFile(uint32_t node) const
{
	if (!isOpen() || node >= d_ptr->fileTree.size()) {
		return false;
	}
	return (d_ptr->fileTree[node].nameOffsetAndTypeFlag & 0x80000000) != 0;
}

bool ZArchiveReader::isDirectory(uint32_t node) const
{
	if (!isOpen() || node >= d_ptr->fileTree.size()) {
		return false;
	}
	return (d_ptr->fileTree[node].nameOffsetAndTypeFlag & 0x80000000) == 0;
}

uint32_t ZArchiveReader::getDirEntryCount(uint32_t node) const
{
	if (!isOpen() || node >= d_ptr->fileTree.size() || isFile(node)) {
		return 0;
	}
	return d_ptr->fileTree[node].directoryRecord.count;
}

bool ZArchiveReader::getDirEntry(uint32_t node, uint32_t index, std::string &name, bool &isFileOut, uint64_t &sizeOut) const
{
	if (!isOpen() || node >= d_ptr->fileTree.size() || isFile(node)) {
		return false;
	}

	const auto &dir = d_ptr->fileTree[node];
	if (index >= dir.directoryRecord.count) {
		return false;
	}

	const uint32_t childIndex = dir.directoryRecord.nodeStartIndex + index;
	if (childIndex >= d_ptr->fileTree.size()) {
		return false;
	}

	const auto &child = d_ptr->fileTree[childIndex];
	isFileOut = (child.nameOffsetAndTypeFlag & 0x80000000) != 0;
	if (isFileOut) {
		uint64_t fileSize = child.fileRecord.fileSizeLow;
		fileSize |= (static_cast<uint64_t>(child.fileRecord.fileOffsetAndSizeHigh & 0xFFFF0000) << 16);
		sizeOut = fileSize;
	} else {
		sizeOut = 0;
	}

	std::string_view nameView = ZArchiveReaderPrivate::getName(d_ptr->nameTable, child.nameOffsetAndTypeFlag & 0x7FFFFFFF);
	if (nameView.empty()) {
		return false;
	}
	name.assign(nameView.data(), nameView.size());
	return true;
}

uint64_t ZArchiveReader::getFileSize(uint32_t node) const
{
	if (!isOpen() || node >= d_ptr->fileTree.size() || !isFile(node)) {
		return 0;
	}

	const auto &entry = d_ptr->fileTree[node];
	uint64_t fileSize = entry.fileRecord.fileSizeLow;
	fileSize |= (static_cast<uint64_t>(entry.fileRecord.fileOffsetAndSizeHigh & 0xFFFF0000) << 16);
	return fileSize;
}

size_t ZArchiveReader::readFromFile(uint32_t node, off64_t offset, size_t length, void *buffer)
{
	if (!isOpen() || node >= d_ptr->fileTree.size() || !isFile(node) || !buffer || length == 0 || offset < 0) {
		return 0;
	}

	const auto &entry = d_ptr->fileTree[node];
	uint64_t fileOffset = entry.fileRecord.fileOffsetLow;
	fileOffset |= (static_cast<uint64_t>(entry.fileRecord.fileOffsetAndSizeHigh & 0x0000FFFF) << 32);

	uint64_t fileSize = entry.fileRecord.fileSizeLow;
	fileSize |= (static_cast<uint64_t>(entry.fileRecord.fileOffsetAndSizeHigh & 0xFFFF0000) << 16);

	if (static_cast<uint64_t>(offset) >= fileSize) {
		return 0;
	}

	const size_t bytesToRead = std::min<size_t>(length, static_cast<size_t>(fileSize - offset));
	uint64_t rawReadOffset = fileOffset + offset;
	size_t remainingBytes = bytesToRead;
	uint8_t *outBuf = reinterpret_cast<uint8_t*>(buffer);

	std::unique_lock<std::mutex> lock(d_ptr->accessMutex);

	while (remainingBytes > 0) {
		const uint64_t blockIdx = rawReadOffset / ZARCHIVE_COMPRESSED_BLOCK_SIZE;
		const uint32_t blockOffset = static_cast<uint32_t>(rawReadOffset % ZARCHIVE_COMPRESSED_BLOCK_SIZE);
		const size_t step = std::min<size_t>(remainingBytes, ZARCHIVE_COMPRESSED_BLOCK_SIZE - blockOffset);

		ZArchiveReaderPrivate::CacheBlock *block = d_ptr->getCachedBlock(blockIdx);
		if (!block) {
			break;
		}

		memcpy(outBuf, block->data.data() + blockOffset, step);
		rawReadOffset += step;
		remainingBytes -= step;
		outBuf += step;
	}

	return (bytesToRead - remainingBytes);
}

LibRpFile::IRpFilePtr ZArchiveReader::openFile(uint32_t node)
{
	if (!isOpen() || !isFile(node)) {
		return {};
	}
	return std::make_shared<ZArchiveFile>(shared_from_this(), node);
}

LibRpFile::IRpFilePtr ZArchiveReader::openFile(const char *path)
{
	if (!isOpen() || !path) {
		return {};
	}
	uint32_t node = lookUp(path);
	if (node == ZARCHIVE_INVALID_NODE || !isFile(node)) {
		return {};
	}
	return openFile(node);
}

} // namespace LibRomData
