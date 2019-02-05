/***************************************************************************
 * RVT-H Tool (librvth)                                                    *
 * reader_plain.cpp: Plain disc image reader class.                        *
 * Used for plain binary disc images, e.g. .gcm and RVT-H images.          *
 *                                                                         *
 * Copyright (c) 2018-2019 by David Korth.                                 *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 ***************************************************************************/

#include "reader_plain.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#ifndef LBA_TO_BYTES
# define LBA_TO_BYTES(x) ((int64_t)(x) * LBA_SIZE)
#endif

// Functions.
static uint32_t reader_plain_read(Reader *reader, void *ptr, uint32_t lba_start, uint32_t lba_len);
static uint32_t reader_plain_write(Reader *reader, const void *ptr, uint32_t lba_start, uint32_t lba_len);
static void reader_plain_flush(Reader *reader);
static void reader_plain_close(Reader *reader);

// vtable
static const Reader_Vtbl reader_plain_vtable = {
	reader_plain_read,
	reader_plain_write,
	reader_plain_flush,
	reader_plain_close,
};

// We're not using an internal struct for reader_plain,
// since we don't need to maintain any internal state
// other than what's provided by Reader*.

/**
 * Create a plain reader for a disc image.
 *
 * NOTE: If lba_start == 0 and lba_len == 0, the entire file
 * will be used.
 *
 * @param file		RefFile*.
 * @param lba_start	[in] Starting LBA,
 * @param lba_len	[in] Length, in LBAs.
 * @return Reader*, or NULL on error.
 */
Reader *reader_plain_open(RefFile *file, uint32_t lba_start, uint32_t lba_len)
{
	Reader *reader;
	int64_t filesize;
	int err = 0;

	// Validate parameters.
	if (!file || (lba_start > 0 && lba_len == 0)) {
		// Invalid parameters.
		errno = EINVAL;
		return NULL;
	}

	// Allocate memory for the Reader object.
	reader = (Reader*)malloc(sizeof(*reader));
	if (!reader) {
		// Error allocating memory.
		if (errno == 0) {
			errno = ENOMEM;
		}
		return NULL;
	}

	// ref() the file.
	reader->file = file->ref();

	// Set the vtable.
	reader->vtbl = &reader_plain_vtable;

	// Get the file size.
	errno = 0;
	filesize = reader->file->size();
	if (filesize < 0) {
		// Seek error.
		// NOTE: Not failing on empty file, since that happens
		// when creating a new file to extract an image.
		err = errno;
		if (err == 0) {
			err = EIO;
		}
		goto fail;
	}

	// Set the LBAs.
	if (lba_start == 0 && lba_len == 0) {
		// NOTE: If not a multiple of the LBA size,
		// the partial LBA will be ignored.
		lba_len = (uint32_t)(filesize / LBA_SIZE);
	}
	reader->lba_start = lba_start;
	reader->lba_len = lba_len;

	// Set the reader type.
	if (reader->file->isDevice()) {
		// This is an RVT-H Reader.
		reader->type = RVTH_ImageType_HDD_Reader;
	} else {
		// If the file is larger than 10 GB, assume it's an RVT-H Reader disk image.
		// Otherwise, it's a standalone disc image.
		if (filesize > 10LL*1024LL*1024LL*1024LL) {
			// RVT-H Reader disk image.
			reader->type = RVTH_ImageType_HDD_Image;
		} else {
			// If the starting LBA is 0, it's a standard GCM.
			// Otherwise, it has an SDK header.
			reader->type = (lba_start == 0 ? RVTH_ImageType_GCM : RVTH_ImageType_GCM_SDK);
		}
	}

	// Reader initialized.
	return reader;

fail:
	// Failed to initialize the reader.
	if (reader->file) {
		reader->file->unref();
	}
	free(reader);
	errno = err;
	return NULL;
}

/**
 * Read data from a disc image.
 * @param reader	[in] Reader*
 * @param ptr		[out] Read buffer.
 * @param lba_start	[in] Starting LBA.
 * @param lba_len	[in] Length, in LBAs.
 * @return Number of LBAs read, or 0 on error.
 */
static uint32_t reader_plain_read(Reader *reader, void *ptr, uint32_t lba_start, uint32_t lba_len)
{
	int ret;

	// LBA bounds checking.
	// TODO: Check for overflow?
	lba_start += reader->lba_start;
	assert(lba_start + lba_len <= reader->lba_start + reader->lba_len);
	if (lba_start + lba_len > reader->lba_start + reader->lba_len) {
		// Out of range.
		errno = EIO;
		return 0;
	}

	// Seek to lba_start.
	ret = reader->file->seeko(LBA_TO_BYTES(lba_start), SEEK_SET);
	if (ret != 0) {
		// Seek error.
		if (errno == 0) {
			errno = EIO;
		}
		return 0;
	}

	// Read the data.
	return (uint32_t)reader->file->read(ptr, LBA_SIZE, lba_len);
}

/**
 * Write data to a disc image.
 * @param reader	[in] Reader*
 * @param ptr		[in] Write buffer.
 * @param lba_start	[in] Starting LBA.
 * @param lba_len	[in] Length, in LBAs.
 * @return Number of LBAs read, or 0 on error.
 */
static uint32_t reader_plain_write(Reader *reader, const void *ptr, uint32_t lba_start, uint32_t lba_len)
{
	int ret;

	// LBA bounds checking.
	// TODO: Check for overflow?
	lba_start += reader->lba_start;
	assert(lba_start + lba_len <= reader->lba_start + reader->lba_len);
	if (lba_start + lba_len > reader->lba_start + reader->lba_len) {
		// Out of range.
		errno = EIO;
		return 0;
	}

	// Seek to lba_start.
	ret = reader->file->seeko(LBA_TO_BYTES(lba_start), SEEK_SET);
	if (ret != 0) {
		// Seek error.
		if (errno == 0) {
			errno = EIO;
		}
		return 0;
	}

	// Write the data.
	return (uint32_t)reader->file->write(ptr, LBA_SIZE, lba_len);
}

/**
 * Flush the file buffers.
 * @param reader	[in] Reader*
 * */
static void reader_plain_flush(Reader *reader)
{
	reader->file->flush();
}

/**
 * Close a disc image.
 * @param reader	[in] Reader*
 */
static void reader_plain_close(Reader *reader)
{
	reader->file->unref();
	free(reader);
}