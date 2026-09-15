/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * ZArchiveFile.cpp: IRpFile implementation for a file inside ZArchive.   *
 *                                                                         *
 * Copyright (c) 2026 by David Korth.                                      *
 * SPDX-License-Identifier: GPL-2.0-or-later                               *
 ***************************************************************************/

#include "config.librpbase.h"
#include "ZArchiveFile.hpp"

#include <algorithm>
#include <cerrno>

namespace LibRomData {

ZArchiveFile::ZArchiveFile(const std::shared_ptr<ZArchiveReader> &archive, uint32_t node)
	: m_archive(archive)
	, m_node(node)
	, m_pos(0)
	, m_size(0)
{
	if (m_archive && m_archive->isOpen() && m_node != ZARCHIVE_INVALID_NODE && m_archive->isFile(m_node)) {
		m_size = static_cast<off64_t>(m_archive->getFileSize(m_node));
	} else {
		m_archive.reset();
		m_node = ZARCHIVE_INVALID_NODE;
		m_lastError = ENOENT;
	}
}

bool ZArchiveFile::isOpen(void) const
{
	return (m_archive && m_archive->isOpen() && m_node != ZARCHIVE_INVALID_NODE);
}

void ZArchiveFile::close(void)
{
	m_archive.reset();
	m_node = ZARCHIVE_INVALID_NODE;
	m_pos = 0;
	m_size = 0;
}

size_t ZArchiveFile::read(void *ptr, size_t size)
{
	if (!isOpen()) {
		m_lastError = EBADF;
		return 0;
	}

	if (m_pos >= m_size || size == 0) {
		return 0;
	}

	const size_t toRead = static_cast<size_t>(std::min<off64_t>(size, m_size - m_pos));
	const size_t bytesRead = m_archive->readFromFile(m_node, m_pos, toRead, ptr);
	m_pos += bytesRead;
	return bytesRead;
}

size_t ZArchiveFile::write(const void *ptr, size_t size)
{
	RP_UNUSED(ptr);
	RP_UNUSED(size);
	m_lastError = EBADF;
	return 0;
}

int ZArchiveFile::seek(off64_t pos, SeekWhence whence)
{
	if (!isOpen()) {
		m_lastError = EBADF;
		return -1;
	}

	off64_t newPos;
	switch (whence) {
		case SeekWhence::Set:
			newPos = pos;
			break;
		case SeekWhence::Cur:
			newPos = m_pos + pos;
			break;
		case SeekWhence::End:
			newPos = m_size + pos;
			break;
		default:
			m_lastError = EINVAL;
			return -1;
	}

	if (newPos < 0) {
		m_lastError = EINVAL;
		return -1;
	}

	m_pos = newPos;
	return 0;
}

off64_t ZArchiveFile::tell(void)
{
	if (!isOpen()) {
		m_lastError = EBADF;
		return -1;
	}
	return m_pos;
}

off64_t ZArchiveFile::size(void)
{
	if (!isOpen()) {
		m_lastError = EBADF;
		return -1;
	}
	return m_size;
}

} // namespace LibRomData
