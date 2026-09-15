/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * ZArchiveFile.hpp: IRpFile implementation for a file inside ZArchive.   *
 *                                                                         *
 * Copyright (c) 2026 by David Korth.                                      *
 * SPDX-License-Identifier: GPL-2.0-or-later                               *
 ***************************************************************************/

#pragma once

#include "librpfile/IRpFile.hpp"
#include "ZArchiveReader.hpp"

namespace LibRomData {

class ZArchiveFile final : public LibRpFile::IRpFile
{
public:
	/**
	 * Construct a ZArchiveFile.
	 * @param archive ZArchiveReader reference.
	 * @param node File node handle.
	 */
	ZArchiveFile(const std::shared_ptr<ZArchiveReader> &archive, uint32_t node);
	~ZArchiveFile() final = default;

private:
	typedef LibRpFile::IRpFile super;

public:
	RP_DISABLE_COPY(ZArchiveFile)

public:
	/**
	 * Is the file open?
	 * @return True if open; false if not.
	 */
	bool isOpen(void) const final;

	/**
	 * Close the file.
	 */
	void close(void) final;

	/**
	 * Read data from the file.
	 * @param ptr Output data buffer.
	 * @param size Amount of data to read, in bytes.
	 * @return Number of bytes read.
	 */
	ATTR_ACCESS_SIZE(write_only, 2, 3)
	size_t read(void *ptr, size_t size) final;

	/**
	 * Write data to the file.
	 * (NOTE: Not supported for ZArchiveFile; this will always return 0.)
	 * @param ptr Input data buffer.
	 * @param size Amount of data to write, in bytes.
	 * @return Number of bytes written.
	 */
	ATTR_ACCESS_SIZE(read_only, 2, 3)
	size_t write(const void *ptr, size_t size) final;

	/**
	 * Set the file position.
	 * @param pos File position.
	 * @param whence Where to seek from.
	 * @return 0 on success; -1 on error.
	 */
	int seek(off64_t pos, SeekWhence whence) final;

	/**
	 * Get the file position.
	 * @return File position, or -1 on error.
	 */
	off64_t tell(void) final;

	/**
	 * Get the data size.
	 * @return Data size, or -1 on error.
	 */
	off64_t size(void) final;

private:
	std::shared_ptr<ZArchiveReader> m_archive;
	uint32_t m_node;
	off64_t m_pos;
	off64_t m_size;
};

} // namespace LibRomData
