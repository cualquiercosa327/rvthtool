/***************************************************************************
 * RVT-H Tool (librvth)                                                    *
 * rvth.c: RVT-H image handler.                                            *
 *                                                                         *
 * Copyright (c) 2018 by David Korth.                                      *
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

#include "rvth.h"
#include "rvth_p.h"

#include "byteswap.h"
#include "nhcd_structs.h"
#include "gcn_structs.h"
#include "cert_store.h"
#include "cert.h"
#include "disc_header.h"
#include "ptbl.h"
#include "bank_init.h"

// Disc image reader.
#include "reader.h"

// C includes.
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/**
 * Get a string description of an error number.
 * - Negative: POSIX error. (strerror())
 * - Positive; RVT-H error. (RvtH_Errors)
 * @param err Error number.
 * @return String description.
 */
const char *rvth_error(int err)
{
	// TODO: Update functions to only return POSIX error codes
	// for system-level issues. For anything weird encountered
	// within an RVT-H HDD or GCN/Wii disc image, an
	// RvtH_Errors code should be retured instead.
	static const char *const errtbl[] = {
		// tr: RVTH_ERROR_SUCCESS
		"Success",
		// tr: RVTH_ERROR_UNRECOGNIZED_FILE
		"Unrecognized file format",
		// tr: RVTH_ERROR_NHCD_TABLE_MAGIC
		"Bank table magic is incorrect",
		// tr: RVTH_ERROR_NO_BANKS
		"No banks found",
		// tr: RVTH_ERROR_BANK_UNKNOWN
		"Bank status is unknown",
		// tr: RVTH_ERROR_BANK_EMPTY
		"Bank is empty",
		// tr: RVTH_ERROR_BANK_DL_2
		"Bank is second bank of a dual-layer image",
		// tr: RVTH_ERROR_NOT_A_DEVICE
		"Operation can only be performed on a device, not an image file",
		// tr: RVTH_ERROR_BANK_IS_DELETED
		"Bank is deleted",
		// tr: RVTH_ERROR_BANK_NOT_DELETED
		"Bank is not deleted",
		// tr: RVTH_ERROR_NOT_HDD_IMAGE
		"RVT-H object is not an HDD image",
		// tr: RVTH_ERROR_NO_GAME_PARTITION
		"Wii game partition not found",
		// tr: RVTH_ERROR_INVALID_BANK_COUNT
		"RVT-H bank count field is invalid",
		// tr: RVTH_ERROR_IS_HDD_IMAGE
		"Operation cannot be performed on devices or HDD images",
		// tr: RVTH_ERROR_IS_RETAIL_CRYPTO
		"Cannot import a retail-encrypted Wii game",
		// tr: RVTH_ERROR_IMAGE_TOO_BIG
		"Source image does not fit in an RVT-H bank",
		// tr: RVTH_ERROR_BANK_NOT_EMPTY_OR_DELETED
		"Destination bank is not empty or deleted",
		// tr: RVTH_ERROR_NOT_WII_IMAGE
		"Wii-specific operation was requested on a non-Wii image",
		// tr: RVTH_ERROR_IS_UNENCRYPTED
		"Image is unencrypted",
		// tr: RVTH_ERROR_IS_ENCRYPTED
		"Image is encrypted",
		// tr: RVTH_ERROR_PARTITION_TABLE_CORRUPTED
		"Wii partition table is corrupted",
		// tr: RVTH_ERROR_PARTITION_HEADER_CORRUPTED
		"At least one Wii partition header is corrupted",
		// tr: RVTH_ERROR_ISSUER_UNKNOWN
		"Certificate has an unknown issuer",

		// 'import' command: Dual-Layer errors.

		// tr: RVTH_ERROR_IMPORT_DL_EXT_NO_BANK1
		"Extended Bank Table: Cannot use Bank 1 for a Dual-Layer image.",
		// tr: RVTH_ERROR_IMPORT_DL_LAST_BANK
		"Cannot use the last bank for a Dual-Layer image",
		// tr: RVTH_ERROR_BANK2DL_NOT_EMPTY_OR_DELETED
		"The second bank for the Dual-Layer image is not empty or deleted",
		// tr: RVTH_ERROR_IMPORT_DL_NOT_CONTIGUOUS
		"The two banks are not contiguous",

		// NDEV option.

		// tr: RVTH_ERROR_NDEV_GCN_NOT_SUPPORTED
		"NDEV headers for GCN are currently unsupported.",
	};
	static_assert(ARRAY_SIZE(errtbl) == RVTH_ERROR_MAX, "Missing error descriptions!");

	if (err < 0) {
		return strerror(-err);
	} else if (err >= ARRAY_SIZE(errtbl)) {
		return "(unknown)";
	}

	return errtbl[err];
}

/**
 * Open a Wii or GameCube disc image.
 * @param f_img	[in] RefFile*
 * @param pErr	[out,opt] Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 * @return RvtH struct pointer if the file is a supported image; NULL on error. (check errno)
 */
static RvtH *rvth_open_gcm(RefFile *f_img, int *pErr)
{
	RvtH *rvth = NULL;
	RvtH_BankEntry *entry;
	int ret = 0;	// errno or RvtH_Errors
	int err = 0;	// errno setting

	Reader *reader = NULL;
	int64_t len;
	uint8_t type;

	// Disc header.
	union {
		GCN_DiscHeader gcn;
		uint8_t sbuf[LBA_SIZE];
	} discHeader;

	// TODO: Detect CISO and WBFS.

	// Get the file length.
	// FIXME: This is obtained in rvth_open().
	// Pass it as a parameter?
	ret = ref_seeko(f_img, 0, SEEK_END);
	if (ret != 0) {
		// Seek error.
		err = errno;
		if (err == 0) {
			err = EIO;
		}
		ret = -err;
		goto fail;
	}
	len = ref_tello(f_img);

	// Rewind back to the beginning of the file.
	ret = ref_seeko(f_img, 0, SEEK_SET);
	if (ret != 0) {
		// Seek error.
		err = errno;
		if (err == 0) {
			err = EIO;
		}
		ret = -err;
		goto fail;
	}

	// Initialize the disc image reader.
	// We need to do this before anything else in order to
	// handle CISO and WBFS images.
	reader = reader_open(f_img, 0, BYTES_TO_LBA(len));
	if (!reader) {
		// Unable to open the reader.
		goto fail;
	}

	// Read the GCN disc header.
	// NOTE: Since this is a standalone disc image, we'll just
	// read the header directly.
	ret = reader_read(reader, discHeader.sbuf, 0, 1);
	if (ret < 0) {
		// Error...
		err = -ret;
		goto fail;
	}

	// Identify the disc type.
	type = rvth_disc_header_identify(&discHeader.gcn);
	if (type == RVTH_BankType_Wii_SL &&
	    reader->lba_len > NHCD_BANK_WII_SL_SIZE_RVTR_LBA)
	{
		// Dual-layer image.
		type = RVTH_BankType_Wii_DL;
	}

	// Allocate memory for the RvtH object
	rvth = calloc(1, sizeof(RvtH));
	if (!rvth) {
		// Error allocating memory.
		err = errno;
		if (err == 0) {
			err = ENOMEM;	// NOTE: Standalone
		}
		ret = -err;
		goto fail;
	}

	// Allocate memory for a single RvtH_BankEntry object.
	rvth->bank_count = 1;
	rvth->is_hdd = false;
	rvth->entries = calloc(1, sizeof(RvtH_BankEntry));
	if (!rvth->entries) {
		// Error allocating memory.
		err = errno;
		if (err == 0) {
			err = ENOMEM;
		}
		ret = -err;
		goto fail;
	};

	// Initialize the bank entry.
	// NOTE: Not using rvth_init_BankEntry() here.
	rvth->f_img = ref_dup(f_img);
	entry = rvth->entries;
	entry->lba_start = reader->lba_start;
	entry->lba_len = reader->lba_len;
	entry->type = type;
	entry->is_deleted = false;
	entry->reader = reader;

	// Timestamp.
	// TODO: Get the timestamp from the file.
	entry->timestamp = -1;

	if (type != RVTH_BankType_Empty) {
		// Copy the disc header.
		memcpy(&entry->discHeader, &discHeader.gcn, sizeof(entry->discHeader));

		// TODO: Error handling.
		// Initialize the region code.
		rvth_init_BankEntry_region(entry);
		// Initialize the encryption status.
		rvth_init_BankEntry_crypto(entry);
	}

	// Disc image loaded.
	return rvth;

fail:
	// Failed to open the disc image.
	if (reader) {
		reader_close(reader);
	}

	rvth_close(rvth);
	if (pErr) {
		*pErr = ret;
	}
	if (err != 0) {
		errno = err;
	}
	return NULL;
}

/**
 * Open an RVT-H disk image.
 * @param f_img	[in] RefFile*
 * @param pErr	[out,opt] Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 * @return RvtH struct pointer if the file is a supported image; NULL on error. (check errno)
 */
static RvtH *rvth_open_hdd(RefFile *f_img, int *pErr)
{
	NHCD_BankTable_Header nhcd_header;
	RvtH *rvth = NULL;
	RvtH_BankEntry *rvth_entry;
	int ret = 0;	// errno or RvtH_Errors
	int err = 0;	// errno setting

	unsigned int i;
	int64_t addr;
	size_t size;

	// Check the bank table header.
	ret = ref_seeko(f_img, LBA_TO_BYTES(NHCD_BANKTABLE_ADDRESS_LBA), SEEK_SET);
	if (ret != 0) {
		// Seek error.
		err = errno;
		if (err == 0) {
			err = EIO;
		}
		ret = -err;
		goto fail;
	}
	size = ref_read(&nhcd_header, 1, sizeof(nhcd_header), f_img);
	if (size != sizeof(nhcd_header)) {
		// Short read.
		err = errno;
		if (err == 0) {
			err = EIO;
		}
		ret = -err;
		goto fail;
	}

	// Check the magic number.
	if (nhcd_header.magic != be32_to_cpu(NHCD_BANKTABLE_MAGIC)) {
		// Incorrect magic number.
		err = EIO;
		ret = RVTH_ERROR_NHCD_TABLE_MAGIC;
		goto fail;
	}

	// Allocate memory for the RvtH object
	rvth = calloc(1, sizeof(RvtH));
	if (!rvth) {
		// Error allocating memory.
		err = errno;
		if (err == 0) {
			err = ENOMEM;
		}
		ret = -err;
		goto fail;
	}

	// Get the bank count.
	rvth->bank_count = be32_to_cpu(nhcd_header.bank_count);
	if (rvth->bank_count < 8 || rvth->bank_count > 32) {
		// Bank count is either too small or too large.
		// RVT-H systems are set to 8 banks at the factory,
		// but we're supporting up to 32 in case the user
		// has modified it.
		// TODO: More extensive "extra bank" testing.
		err = errno;
		if (err == 0) {
			err = ENOMEM;
		}
		ret = -err;
		goto fail;
	}

	// Allocate memory for the 8 RvtH_BankEntry objects.
	rvth->is_hdd = true;
	rvth->entries = calloc(rvth->bank_count, sizeof(RvtH_BankEntry));
	if (!rvth->entries) {
		// Error allocating memory.
		err = errno;
		if (err == 0) {
			err = ENOMEM;
		}
		ret = -err;
		goto fail;
	};

	rvth->f_img = ref_dup(f_img);
	rvth_entry = rvth->entries;
	addr = (uint32_t)(LBA_TO_BYTES(NHCD_BANKTABLE_ADDRESS_LBA) + NHCD_BLOCK_SIZE);
	for (i = 0; i < rvth->bank_count; i++, rvth_entry++, addr += 512) {
		NHCD_BankEntry nhcd_entry;
		uint32_t lba_start = 0, lba_len = 0;
		uint8_t type = RVTH_BankType_Unknown;

		if (i > 0 && (rvth_entry-1)->type == RVTH_BankType_Wii_DL) {
			// Second bank for a dual-layer Wii image.
			rvth_entry->type = RVTH_BankType_Wii_DL_Bank2;
			rvth_entry->timestamp = -1;
			continue;
		}

		ret = ref_seeko(f_img, addr, SEEK_SET);
		if (ret != 0) {
			// Seek error.
			err = errno;
			if (err == 0) {
				err = EIO;
			}
			ret = -err;
			goto fail;
		}
		size = ref_read(&nhcd_entry, 1, sizeof(nhcd_entry), f_img);
		if (size != sizeof(nhcd_entry)) {
			// Short read.
			err = errno;
			if (err == 0) {
				err = EIO;
			}
			ret = -err;
			goto fail;
		}

		// Check the type.
		switch (be32_to_cpu(nhcd_entry.type)) {
			default:
				// Unknown bank type...
				type = RVTH_BankType_Unknown;
				break;
			case NHCD_BankType_Empty:
				// "Empty" bank. May have a deleted image.
				type = RVTH_BankType_Empty;
				break;
			case NHCD_BankType_GCN:
				// GameCube
				type = RVTH_BankType_GCN;
				break;
			case NHCD_BankType_Wii_SL:
				// Wii (single-layer)
				type = RVTH_BankType_Wii_SL;
				break;
			case NHCD_BankType_Wii_DL:
				// Wii (dual-layer)
				// TODO: Cannot start in Bank 8.
				type = RVTH_BankType_Wii_DL;
				break;
		}

		// For valid types, use the listed LBAs if they're non-zero.
		if (type >= RVTH_BankType_GCN) {
			lba_start = be32_to_cpu(nhcd_entry.lba_start);
			lba_len = be32_to_cpu(nhcd_entry.lba_len);
		}

		if (lba_start == 0 || lba_len == 0) {
			// Invalid LBAs. Use the default starting offset.
			// Bank size will be determined by rvth_init_BankEntry().
			lba_start = NHCD_BANK_START_LBA(i, rvth->bank_count);
			lba_len = 0;
		}

		// Initialize the bank entry.
		rvth_init_BankEntry(rvth_entry, f_img, type,
			lba_start, lba_len, nhcd_entry.timestamp);
	}

	// RVT-H image loaded.
	return rvth;

fail:
	// Failed to open the HDD image.
	rvth_close(rvth);
	if (pErr) {
		*pErr = ret;
	}
	if (err != 0) {
		errno = err;
	}
	return NULL;
}

/**
 * Open an RVT-H disk image, GameCube disc image, or Wii disc image.
 * @param filename	[in] Filename.
 * @param pErr		[out,opt] Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 * @return RvtH struct pointer if the file is a supported image; NULL on error. (check errno)
 */
RvtH *rvth_open(const TCHAR *filename, int *pErr)
{
	RefFile *f_img;
	RvtH *rvth = NULL;
	int ret = 0;
	int64_t len;

	// Open the disk image.
	f_img = ref_open(filename);
	if (!f_img) {
		// Could not open the file.
		if (pErr) {
			*pErr = -errno;
		}
		return NULL;
	}

	// Determine if this is an HDD image or a disc image.
	ret = ref_seeko(f_img, 0, SEEK_END);
	if (ret != 0) {
		// Seek error.
		if (errno == 0) {
			errno = EIO;
		}
		goto end;
	}
	len = ref_tello(f_img);
	if (len == 0) {
		// File is empty.
		errno = EIO;
	} else if (len <= 2*LBA_TO_BYTES(NHCD_BANK_SIZE_LBA)) {
		// Two banks or less.
		// This is most likely a standalone disc image.
		rvth = rvth_open_gcm(f_img, pErr);
	} else {
		// More than two banks.
		// This is most likely an RVT-H HDD image.
		rvth = rvth_open_hdd(f_img, pErr);
	}

end:
	if (pErr) {
		*pErr = -errno;
	}
	// If the RvtH object was opened, it will have
	// called ref_dup() to increment the reference count.
	ref_close(f_img);
	return rvth;
}

/**
 * Close an opened RVT-H disk image.
 * @param rvth RVT-H disk image.
 */
void rvth_close(RvtH *rvth)
{
	unsigned int i;

	if (!rvth)
		return;

	// Close all bank entry files.
	// RefFile has a reference count, so we have to clear the count.
	for (i = 0; i < rvth->bank_count; i++) {
		if (rvth->entries[i].reader) {
			reader_close(rvth->entries[i].reader);
		}
		free(rvth->entries[i].ptbl);
	}

	// Free the bank entries array.
	free(rvth->entries);

	// Clear the main reference.
	if (rvth->f_img) {
		ref_close(rvth->f_img);
	}

	free(rvth);
}

/**
 * Is this RVT-H object an HDD image or a standalone disc image?
 * @param rvth RVT-H object.
 * @return True if the RVT-H object is an HDD image; false if it's a standalone disc image.
 */
bool rvth_is_hdd(const RvtH *rvth)
{
	if (!rvth) {
		errno = EINVAL;
		return false;
	}
	return rvth->is_hdd;
}

/**
 * Get the number of banks in an opened RVT-H disk image.
 * @param rvth RVT-H disk image.
 * @return Number of banks.
 */
unsigned int rvth_get_BankCount(const RvtH *rvth)
{
	if (!rvth) {
		errno = EINVAL;
		return 0;
	}

	return rvth->bank_count;
}

/**
 * Get a bank table entry.
 * @param rvth	[in] RVT-H disk image.
 * @param bank	[in] Bank number. (0-7)
 * @param pErr	[out,opt] Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 * @return Bank table entry, or NULL if out of range.
 */
const RvtH_BankEntry *rvth_get_BankEntry(const RvtH *rvth, unsigned int bank, int *pErr)
{
	if (!rvth) {
		errno = EINVAL;
		if (pErr) {
			*pErr = -EINVAL;
		}
		return NULL;
	} else if (bank >= rvth->bank_count) {
		errno = ERANGE;
		if (pErr) {
			*pErr = -EINVAL;
		}
		return NULL;
	}

	return &rvth->entries[bank];
}
