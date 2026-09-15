/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * ZArchiveReader.hpp: ZArchive reader.                                    *
 *                                                                         *
 * Copyright (c) 2022 by Exzap.                                            *
 * Copyright (c) 2026 by David Korth.                                      *
 * SPDX-License-Identifier: MIT-0                                          *
 ***************************************************************************/

#pragma once

#include "librpfile/IRpFile.hpp"
#include "zarchive_structs.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LibRomData {

class ZArchiveReaderPrivate;

class ZArchiveReader final : public std::enable_shared_from_this<ZArchiveReader>
{
public:
	/**
	 * Construct a ZArchiveReader from an IRpFile.
	 * @param file Underlying file.
	 */
	explicit ZArchiveReader(const LibRpFile::IRpFilePtr &file);
	~ZArchiveReader();

public:
	RP_DISABLE_COPY(ZArchiveReader)

public:
	/**
	 * Is the given file a valid ZArchive?
	 * @param file Underlying file.
	 * @return True if valid ZArchive; false if not.
	 */
	static bool isZArchive(const LibRpFile::IRpFilePtr &file);

	/**
	 * Is the given footer buffer a valid ZArchive footer?
	 * @param pFooter Footer buffer (at least 144 bytes).
	 * @param szFooter Size of pFooter.
	 * @param szFile Total file size.
	 * @return True if valid ZArchive footer; false if not.
	 */
	static bool isZArchive_static(const uint8_t *pFooter, size_t szFooter, off64_t szFile);

public:
	/**
	 * Is the archive open and valid?
	 * @return True if open; false if not.
	 */
	bool isOpen(void) const;

	/**
	 * Get the last error.
	 * @return Last error code.
	 */
	int lastError(void) const;

	/**
	 * Look up a node handle by path.
	 * @param path Path inside archive (e.g. "code/app.xml").
	 * @return Node handle, or ZARCHIVE_INVALID_NODE on failure.
	 */
	uint32_t lookUp(const char *path) const;

	/**
	 * Check if a node is a file.
	 * @param node Node handle.
	 * @return True if file; false if not.
	 */
	bool isFile(uint32_t node) const;

	/**
	 * Check if a node is a directory.
	 * @param node Node handle.
	 * @return True if directory; false if not.
	 */
	bool isDirectory(uint32_t node) const;

	/**
	 * Get directory entry count.
	 * @param node Directory node handle.
	 * @return Entry count.
	 */
	uint32_t getDirEntryCount(uint32_t node) const;

	/**
	 * Get a directory entry by index.
	 * @param node Directory node handle.
	 * @param index Child entry index.
	 * @param name Output entry name.
	 * @param isFile Output whether it is a file.
	 * @param size Output file size (0 for directory).
	 * @return True on success; false on failure.
	 */
	bool getDirEntry(uint32_t node, uint32_t index, std::string &name, bool &isFile, uint64_t &size) const;

	/**
	 * Get the size of a file node.
	 * @param node File node handle.
	 * @return File size in bytes, or 0 on error.
	 */
	uint64_t getFileSize(uint32_t node) const;

	/**
	 * Read decompressed data from a file node.
	 * @param node File node handle.
	 * @param offset Byte offset within the file.
	 * @param length Number of bytes to read.
	 * @param buffer Output buffer.
	 * @return Number of bytes read.
	 */
	size_t readFromFile(uint32_t node, off64_t offset, size_t length, void *buffer);

	/**
	 * Open a virtual IRpFile for a node handle.
	 * @param node File node handle.
	 * @return IRpFilePtr, or nullptr on failure.
	 */
	LibRpFile::IRpFilePtr openFile(uint32_t node);

	/**
	 * Open a virtual IRpFile for a path.
	 * @param path Path inside archive.
	 * @return IRpFilePtr, or nullptr on failure.
	 */
	LibRpFile::IRpFilePtr openFile(const char *path);

protected:
	friend class ZArchiveReaderPrivate;
	std::unique_ptr<ZArchiveReaderPrivate> const d_ptr;
};

typedef std::shared_ptr<ZArchiveReader> ZArchiveReaderPtr;

} // namespace LibRomData
