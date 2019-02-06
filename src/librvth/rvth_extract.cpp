/***************************************************************************
 * RVT-H Tool (librvth)                                                    *
 * rvth_extract.cpp: RVT-H extract and import functions.                   *
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

#include "rvth.h"
#include "rvth_p.h"
#include "rvth_recrypt.h"
#include "ptbl.h"
#include "extract_crypt.h"

#include "byteswap.h"
#include "nhcd_structs.h"

// Disc image reader.
#include "reader/Reader.hpp"

// C includes.
#include <stdlib.h>

// C includes. (C++ namespace)
#include <cassert>
#include <cerrno>
#include <cstring>
#include <ctime>

/**
 * Copy a bank from an RVT-H HDD or standalone disc image to a writable standalone disc image.
 * @param rvth_dest	[out] Destination RvtH object.
 * @param rvth_src	[in] Source RvtH object.
 * @param bank_src	[in] Source bank number. (0-7)
 * @param callback	[in,opt] Progress callback.
 * @return Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 */
int rvth_copy_to_gcm(RvtH *rvth_dest, const RvtH *rvth_src, unsigned int bank_src, RvtH_Progress_Callback callback)
{
	const RvtH_BankEntry *entry_src;
	uint8_t *buf = NULL;
	uint32_t lba_copy_len;	// Total number of LBAs to copy. (entry_src->lba_len)
	uint32_t lba_count;
	uint32_t lba_buf_max;	// Highest LBA that can be written using the buffer.
	uint32_t lba_nonsparse;	// Last LBA written that wasn't sparse.
	unsigned int sprs;		// Sparse counter.

	// Callback state.
	RvtH_Progress_State state;

	int ret = 0;	// errno or RvtH_Errors
	int err = 0;	// errno setting

	// Destination disc image.
	RvtH_BankEntry *entry_dest;

	if (!rvth_dest || !rvth_src) {
		errno = EINVAL;
		return -EINVAL;
	} else if (bank_src >= rvth_src->bank_count) {
		errno = ERANGE;
		return -ERANGE;
	} else if (rvth_is_hdd(rvth_dest) || rvth_dest->bank_count != 1) {
		// Destination is not a standalone disc image.
		// Copying to HDDs will be handled differently.
		errno = EIO;
		return RVTH_ERROR_IS_HDD_IMAGE;
	}

	// Check if the source bank can be extracted.
	entry_src = &rvth_src->entries[bank_src];
	switch (entry_src->type) {
		case RVTH_BankType_GCN:
		case RVTH_BankType_Wii_SL:
		case RVTH_BankType_Wii_DL:
			// Bank can be extracted.
			break;

		case RVTH_BankType_Unknown:
		default:
			// Unknown bank status...
			errno = EIO;
			return RVTH_ERROR_BANK_UNKNOWN;

		case RVTH_BankType_Empty:
			// Bank is empty.
			errno = ENOENT;
			return RVTH_ERROR_BANK_EMPTY;

		case RVTH_BankType_Wii_DL_Bank2:
			// Second bank of a dual-layer Wii disc image.
			// TODO: Automatically select the first bank?
			errno = EIO;
			return RVTH_ERROR_BANK_DL_2;
	}

	// Process 1 MB at a time.
	#define BUF_SIZE 1048576
	#define LBA_COUNT_BUF BYTES_TO_LBA(BUF_SIZE)
	buf = (uint8_t*)malloc(BUF_SIZE);
	if (!buf) {
		// Error allocating memory.
		err = errno;
		if (err == 0) {
			err = ENOMEM;
		}
		ret = -err;
		goto end;
	}

	// FIXME: If the file existed and wasn't 0 bytes,
	// either truncate it or don't do sparse writes.

	// Make this a sparse file.
	entry_dest = &rvth_dest->entries[0];
	ret = rvth_dest->f_img->makeSparse(LBA_TO_BYTES(entry_dest->lba_len));
	if (ret != 0) {
		// Error managing the sparse file.
		// TODO: Delete the file?
		err = rvth_dest->f_img->lastError();
		if (err == 0) {
			err = ENOMEM;
		}
		ret = -err;
		goto end;
	}

	// Copy the bank table information.
	entry_dest->type	= entry_src->type;
	entry_dest->region_code	= entry_src->region_code;
	entry_dest->is_deleted	= false;
	entry_dest->crypto_type	= entry_src->crypto_type;
	entry_dest->ios_version	= entry_src->ios_version;
	entry_dest->ticket	= entry_src->ticket;
	entry_dest->tmd		= entry_src->tmd;

	// Copy the disc header.
	memcpy(&entry_dest->discHeader, &entry_src->discHeader, sizeof(entry_dest->discHeader));

	// Timestamp.
	if (entry_src->timestamp >= 0) {
		entry_dest->timestamp = entry_src->timestamp;
	} else {
		entry_dest->timestamp = time(NULL);
	}

	// Number of LBAs to copy.
	lba_copy_len = entry_src->lba_len;

	if (callback) {
		// Initialize the callback state.
		state.type = RVTH_PROGRESS_EXTRACT;
		state.rvth = rvth_src;
		state.rvth_gcm = rvth_dest;
		state.bank_rvth = bank_src;
		state.bank_gcm = 0;
		state.lba_processed = 0;
		state.lba_total = lba_copy_len;
	}

	// TODO: Optimize seeking? (Reader::write() seeks every time.)
	lba_buf_max = entry_dest->lba_len & ~(LBA_COUNT_BUF-1);
	lba_nonsparse = 0;
	for (lba_count = 0; lba_count < lba_buf_max; lba_count += LBA_COUNT_BUF) {
		if (callback) {
			bool bRet;
			state.lba_processed = lba_count;
			bRet = callback(&state);
			if (!bRet) {
				// Stop processing.
				err = ECANCELED;
				goto end;
			}
		}

		// TODO: Error handling.
		entry_src->reader->read(buf, lba_count, LBA_COUNT_BUF);

		if (lba_count == 0) {
			// Make sure we copy the disc header in if the
			// header was zeroed by the RVT-H's "Flush" function.
			// TODO: Move this outside of the `for` loop.
			// TODO: Also check for NDDEMO?
			const GCN_DiscHeader *const origHdr = (const GCN_DiscHeader*)buf;
			if (origHdr->magic_wii != be32_to_cpu(WII_MAGIC) &&
			    origHdr->magic_gcn != be32_to_cpu(GCN_MAGIC))
			{
				// Missing magic number. Need to restore the disc header.
				memcpy(buf, &entry_src->discHeader, sizeof(entry_src->discHeader));
			}
		}

		// Check for empty 4 KB blocks.
		for (sprs = 0; sprs < BUF_SIZE; sprs += 4096) {
			if (!rvth_is_block_empty(&buf[sprs], 4096)) {
				// 4 KB block is not empty.
				lba_nonsparse = lba_count + (sprs / 512);
				entry_dest->reader->write(&buf[sprs], lba_nonsparse, 8);
				lba_nonsparse += 7;
			}
		}
	}

	// Process any remaining LBAs.
	if (lba_count < lba_copy_len) {
		const unsigned int lba_left = lba_copy_len - lba_count;
		const unsigned int sz_left = (unsigned int)BYTES_TO_LBA(lba_left);

		if (callback) {
			bool bRet;
			state.lba_processed = lba_count;
			bRet = callback(&state);
			if (!bRet) {
				// Stop processing.
				err = ECANCELED;
				goto end;
			}
		}
		entry_src->reader->read(buf, lba_count, lba_left);

		// Check for empty 512-byte blocks.
		for (sprs = 0; sprs < sz_left; sprs += 512) {
			if (!rvth_is_block_empty(&buf[sprs], 512)) {
				// 512-byte block is not empty.
				lba_nonsparse = lba_count + (sprs / 512);
				entry_dest->reader->write(&buf[sprs], lba_nonsparse, 1);
			}
		}
	}

	if (callback) {
		bool bRet;
		state.lba_processed = lba_copy_len;
		bRet = callback(&state);
		if (!bRet) {
			// Stop processing.
			err = ECANCELED;
			goto end;
		}
	}

	// lba_nonsparse should be equal to lba_copy_len-1.
	if (lba_nonsparse != lba_copy_len-1) {
		// Last LBA was sparse.
		// We'll need to write an actual zero block.
		// TODO: Maybe not needed if ftruncate() succeeded?
		// TODO: Check for errors.
		memset(buf, 0, 512);
		entry_dest->reader->write(buf, lba_copy_len-1, 1);
	}

	// Finished extracting the disc image.
	entry_dest->reader->flush();

end:
	free(buf);
	if (err != 0) {
		errno = err;
	}
	return ret;
}

/**
 * Extract a disc image from the RVT-H disk image.
 * Compatibility wrapper; this function calls rvth_create_gcm() and rvth_copy_to_gcm().
 * @param rvth		[in] RVT-H disk image.
 * @param bank		[in] Bank number. (0-7)
 * @param filename	[in] Destination filename.
 * @param recrypt_key	[in] Key for recryption. (-1 for default; otherwise, see RvtH_CryptoType_e)
 * @param flags		[in] Flags. (See RvtH_Extract_Flags.)
 * @param callback	[in,opt] Progress callback.
 * @return Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 */
int rvth_extract(const RvtH *rvth, unsigned int bank, const TCHAR *filename,
	int recrypt_key, uint32_t flags, RvtH_Progress_Callback callback)
{
	RvtH *rvth_dest = NULL;
	RvtH_BankEntry *entry;
	uint32_t gcm_lba_len;
	bool unenc_to_enc;

	int ret;

	if (!rvth || !filename || filename[0] == 0) {
		errno = EINVAL;
		return -EINVAL;
	} else if (bank >= rvth->bank_count) {
		// Bank number is out of range.
		errno = ERANGE;
		return -ERANGE;
	}

	// TODO: If recryption is needed, validate parts of the partitions,
	// e.g. certificate chain length, before copying.

	// Create a standalone disc image.
	entry = &rvth->entries[bank];
	unenc_to_enc = (entry->type >= RVTH_BankType_Wii_SL &&
			entry->crypto_type == RVTH_CryptoType_None &&
			recrypt_key > RVTH_CryptoType_Unknown);
	if (unenc_to_enc) {
		// Converting from unencrypted to encrypted.
		// Need to convert 31k sectors to 32k.
		uint32_t lba_tmp;
		const pt_entry_t *game_pte = rvth_ptbl_find_game(entry);
		if (!game_pte) {
			// No game partition...
			errno = EIO;
			ret = RVTH_ERROR_NO_GAME_PARTITION;
			goto end;
		}

		// TODO: Read the partition header to determine the data offset.
		// Assuming 0x8000 partition header size for now.
		lba_tmp = game_pte->lba_len - BYTES_TO_LBA(0x8000);
		gcm_lba_len = (lba_tmp / 3968 * 4096);
		if (lba_tmp % 3968 != 0) {
			gcm_lba_len += 4096;
		}
		// Assuming 0x8000 header + 0x18000 H3 table.
		gcm_lba_len += BYTES_TO_LBA(0x20000) + game_pte->lba_start;
	} else {
		// Use the bank size as-is.
		gcm_lba_len = entry->lba_len;
	}

	if (flags & RVTH_EXTRACT_PREPEND_SDK_HEADER) {
		if (entry->type == RVTH_BankType_GCN) {
			// FIXME: Not supported.
			return RVTH_ERROR_NDEV_GCN_NOT_SUPPORTED;
		}
		// Prepend 32k to the GCM.
		gcm_lba_len += BYTES_TO_LBA(32768);
	}

	ret = 0;
	rvth_dest = rvth_create_gcm(filename, gcm_lba_len, &ret);
	if (!rvth_dest) {
		// Error creating the standalone disc image.
		if (ret == 0) {
			ret = -EIO;
		}
		goto end;
	}

	if (flags & RVTH_EXTRACT_PREPEND_SDK_HEADER) {
		// Prepend 32k to the GCM.
		size_t size;
		Reader *const reader = rvth_dest->entries[0].reader;
		uint8_t *sdk_header = (uint8_t*)calloc(1, SDK_HEADER_SIZE_BYTES);
		if (!sdk_header) {
			ret = -errno;
			if (ret == 0) {
				ret = -ENOMEM;
			}
			goto end;
		}

		// TODO: Get headers for GC1L and NN2L.
		// TODO: Optimize by using 32-bit writes?
		switch (entry->type) {
			case RVTH_BankType_GCN:
				// FIXME; GameCube GCM seems to use the same values,
				// but it doesn't load with NDEV.
				// Checksum field is always 0xAB0B.
				// TODO: Delete the file?
				rvth_close(rvth_dest);
				return RVTH_ERROR_NDEV_GCN_NOT_SUPPORTED;
			case RVTH_BankType_Wii_SL:
			case RVTH_BankType_Wii_DL:
				// 0x0000: FF FF 00 00
				sdk_header[0x0000] = 0xFF;
				sdk_header[0x0001] = 0xFF;
				// 0x082C: 00 00 E0 06
				sdk_header[0x082E] = 0xE0;
				sdk_header[0x082F] = 0x06;
				// TODO: Checksum at 0x0830? (If 00 00, seems to work for all discs.)
				// 0x0844: 01 00 00 00
				sdk_header[0x0844] = 0x01;
				break;
			default:
				// Should not get here...
				assert(!"Incorrect bank type.");
				free(sdk_header);
				goto end;
		}

		size = reader->write(sdk_header, 0, SDK_HEADER_SIZE_LBA);
		if (size != SDK_HEADER_SIZE_LBA) {
			// Write error.
			ret = -errno;
			if (ret == 0) {
				ret = -EIO;
			}
			goto end;
		}
		free(sdk_header);

		// Remove the SDK header from the reader's offsets.
		reader->lba_adjust(SDK_HEADER_SIZE_LBA);
	}

	// Copy the bank from the source image to the destination GCM.
	if (unenc_to_enc) {
		ret = rvth_copy_to_gcm_doCrypt(rvth_dest, rvth, bank, callback);
	} else {
		ret = rvth_copy_to_gcm(rvth_dest, rvth, bank, callback);
	}
	if (ret == 0 && recrypt_key > RVTH_CryptoType_Unknown) {
		// Recrypt the disc image.
		if (entry->crypto_type != recrypt_key) {
			ret = rvth_recrypt_partitions(rvth_dest, 0, static_cast<RvtH_CryptoType_e>(recrypt_key), callback);
		}
	}

end:
	// TODO: Delete the file on error?
	if (rvth_dest) {
		rvth_close(rvth_dest);
	}
	return ret;
}

/**
 * Copy a bank from an RVT-H HDD or standalone disc image to an RVT-H system.
 * @param rvth_dest	[out] Destination RvtH object.
 * @param bank_dest	[out] Destination bank number. (0-7)
 * @param rvth_src	[in] Source RvtH object.
 * @param bank_src	[in] Source bank number. (0-7)
 * @param callback	[in,opt] Progress callback.
 * @return Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 */
int rvth_copy_to_hdd(RvtH *rvth_dest, unsigned int bank_dest, const RvtH *rvth_src,
	unsigned int bank_src, RvtH_Progress_Callback callback)
{
	const RvtH_BankEntry *entry_src;
	uint32_t lba_copy_len;	// Total number of LBAs to copy. (entry_src->lba_len)
	uint32_t lba_count;
	uint32_t lba_buf_max;	// Highest LBA that can be written using the buffer.
	uint8_t *buf = NULL;

	// Callback state.
	RvtH_Progress_State state;

	int ret = 0;	// errno or RvtH_Errors
	int err = 0;	// errno setting

	// Destination disc image.
	RvtH_BankEntry *entry_dest;
	unsigned int bank_count_dest;

	if (!rvth_dest || !rvth_src) {
		errno = EINVAL;
		return -EINVAL;
	} else if (bank_src >= rvth_src->bank_count ||
		   bank_dest >= rvth_dest->bank_count)
	{
		errno = ERANGE;
		return -ERANGE;
	} else if (!rvth_is_hdd(rvth_dest)) {
		// Destination is not an HDD.
		errno = EIO;
		return RVTH_ERROR_NOT_HDD_IMAGE;
	}

	// Check if the source bank can be imported.
	entry_src = &rvth_src->entries[bank_src];
	switch (entry_src->type) {
		case RVTH_BankType_GCN:
		case RVTH_BankType_Wii_SL:
		case RVTH_BankType_Wii_DL:
			// Bank can be imported.
			break;

		case RVTH_BankType_Unknown:
		default:
			// Unknown bank status...
			errno = EIO;
			return RVTH_ERROR_BANK_UNKNOWN;

		case RVTH_BankType_Empty:
			// Bank is empty.
			errno = ENOENT;
			return RVTH_ERROR_BANK_EMPTY;

		case RVTH_BankType_Wii_DL_Bank2:
			// Second bank of a dual-layer Wii disc image.
			// TODO: Automatically select the first bank?
			errno = EIO;
			return RVTH_ERROR_BANK_DL_2;
	}

	// Get the bank count of the destination RVTH device.
	bank_count_dest = rvth_get_BankCount(rvth_dest);
	// Destination bank entry.
	entry_dest = &rvth_dest->entries[bank_dest];

	// Source image length cannot be larger than a single bank.
	if (entry_src->type == RVTH_BankType_Wii_DL) {
		// Special cases for DL:
		// - Destination bank must not be the last bank.
		// - For extended bank tables, destination bank must not be the first bank.
		// - Both the selected bank and the next bank must be empty or deleted.
		const RvtH_BankEntry *entry_dest2;

		if (bank_count_dest > 8) {
			// Extended bank table.
			if (bank_dest == 0) {
				// Cannot use bank 0.
				errno = EINVAL;
				return RVTH_ERROR_IMPORT_DL_EXT_NO_BANK1;
			}
		}

		// Cannot use the last bank for DL images.
		if (bank_dest == bank_count_dest-1) {
			errno = EINVAL;
			return RVTH_ERROR_IMPORT_DL_LAST_BANK;
		}

		// Check that the first bank is empty or deleted.
		// NOTE: Checked below, but we should check this before
		// checking the second bank.
		if (entry_dest->type != RVTH_BankType_Empty &&
		    !entry_dest->is_deleted)
		{
			errno = EEXIST;
			return RVTH_ERROR_BANK_NOT_EMPTY_OR_DELETED;
		}

		// Check that the second bank is empty or deleted.
		entry_dest2 = &rvth_dest->entries[bank_dest+1];
		if (entry_dest2->type != RVTH_BankType_Empty &&
		    !entry_dest2->is_deleted)
		{
			errno = EEXIST;
			return RVTH_ERROR_BANK2DL_NOT_EMPTY_OR_DELETED;
		}

		// Verify that the two banks are contiguous.
		// FIXME: This should always be the case except for bank 1 on
		// devices with non-extended bank tables. lba_len is reduced
		// if the bank originally had a GameCube image, so we can't
		// check this right now.
		/*if (entry_dest->lba_start + entry_dest->lba_len != entry_dest2->lba_start) {
			// Not contiguous.
			errno = EIO;
			return RVTH_ERROR_IMPORT_DL_NOT_CONTIGUOUS;
		}*/

		// Verify that the image fits in two banks.
		if (entry_src->lba_len > NHCD_BANK_SIZE_LBA*2) {
			// Image is too big.
			errno = ENOSPC;
			return RVTH_ERROR_IMAGE_TOO_BIG;
		}
	} else if (entry_src->lba_len > NHCD_BANK_SIZE_LBA) {
		// Single-layer image is too big for this bank.
		errno = ENOSPC;
		return RVTH_ERROR_IMAGE_TOO_BIG;
	} else if (bank_dest == 0) {
		// Special handling for bank 1 if the bank table is extended.
		// TODO: entry_dest->lba_len should be the full bank size
		// if the bank is empty or deleted.
		// TODO: Add a separate field, lba_max_len?
		if (bank_count_dest > 8) {
			// Image cannot be larger than NHCD_EXTBANKTABLE_BANK_1_SIZE_LBA.
			if (entry_src->lba_len > NHCD_EXTBANKTABLE_BANK_1_SIZE_LBA) {
				errno = ENOSPC;
				return RVTH_ERROR_IMAGE_TOO_BIG;
			}
		}
	}

	// Destination bank must be either empty or deleted.
	if (entry_dest->type != RVTH_BankType_Empty &&
	    !entry_dest->is_deleted)
	{
		errno = EEXIST;
		return RVTH_ERROR_BANK_NOT_EMPTY_OR_DELETED;
	}

	// Make the destination RVT-H object writable.
	ret = rvth_make_writable(rvth_dest);
	if (ret != 0) {
		// Could not make the RVT-H object writable.
		if (ret < 0) {
			err = -ret;
		} else {
			err = EROFS;
		}
		goto end;
	}

	// If no reader is set up for the destination bank, set one up now.
	if (!entry_dest->reader) {
		entry_dest->reader = Reader::open(rvth_dest->f_img,
			entry_dest->lba_start, entry_dest->lba_len);
		if (!entry_dest->reader) {
			// Cannot create a reader...
			err = errno;
			if (err == 0) {
				err = EIO;
			}
			goto end;
		}
	}

	// Process 1 MB at a time.
	#define BUF_SIZE 1048576
	#define LBA_COUNT_BUF BYTES_TO_LBA(BUF_SIZE)
	buf = (uint8_t*)malloc(BUF_SIZE);
	if (!buf) {
		// Error allocating memory.
		err = errno;
		if (err == 0) {
			err = ENOMEM;
		}
		ret = -err;
		goto end;
	}

	// Copy the bank table information.
	entry_dest->lba_len	= entry_src->lba_len;
	entry_dest->type	= entry_src->type;
	entry_dest->region_code	= entry_src->region_code;
	entry_dest->is_deleted	= false;
	entry_dest->crypto_type	= entry_src->crypto_type;
	entry_dest->ios_version	= entry_src->ios_version;
	entry_dest->ticket	= entry_src->ticket;
	entry_dest->tmd		= entry_src->tmd;

	// Copy the disc header.
	memcpy(&entry_dest->discHeader, &entry_src->discHeader, sizeof(entry_dest->discHeader));

	// Timestamp.
	if (entry_src->timestamp >= 0) {
		entry_dest->timestamp = entry_src->timestamp;
	} else {
		entry_dest->timestamp = time(NULL);
	}

	// NOTE: We're only writing up to the source image file size.
	// There's no point in wiping the rest of the bank.
	lba_copy_len = entry_src->lba_len;

	if (callback) {
		// Initialize the callback state.
		state.type = RVTH_PROGRESS_IMPORT;
		state.rvth = rvth_dest;
		state.rvth_gcm = rvth_src;
		state.bank_rvth = bank_dest;
		state.bank_gcm = bank_src;
		state.lba_processed = 0;
		state.lba_total = lba_copy_len;
	}

	// TODO: Special indicator.
	// TODO: Optimize seeking? (Reader::write() seeks every time.)
	lba_buf_max = entry_dest->lba_len & ~(LBA_COUNT_BUF-1);
	for (lba_count = 0; lba_count < lba_buf_max; lba_count += LBA_COUNT_BUF) {
		if (callback) {
			bool bRet;
			state.lba_processed = lba_count;
			bRet = callback(&state);
			if (!bRet) {
				// Stop processing.
				err = ECANCELED;
				goto end;
			}
		}

		// TODO: Restore the disc header here if necessary?
		// GCMs being imported generally won't have the first
		// 16 KB zeroed out...

		// TODO: Error handling.
		entry_src->reader->read(buf, lba_count, LBA_COUNT_BUF);
		entry_dest->reader->write(buf, lba_count, LBA_COUNT_BUF);
	}

	// Process any remaining LBAs.
	if (lba_count < lba_copy_len) {
		const unsigned int lba_left = lba_copy_len - lba_count;
		entry_src->reader->read(buf, lba_count, lba_left);
		entry_dest->reader->write(buf, lba_count, lba_left);
	}

	if (callback) {
		bool bRet;
		state.lba_processed = lba_copy_len;
		bRet = callback(&state);
		if (!bRet) {
			// Stop processing.
			err = ECANCELED;
			goto end;
		}
	}

	// Flush the buffers.
	entry_dest->reader->flush();

	// Update the bank table.
	// TODO: Check for errors.
	rvth_write_BankEntry(rvth_dest, bank_dest);

	// Finished importing the disc image.

end:
	free(buf);
	if (err != 0) {
		errno = err;
	}
	return ret;
}

/**
 * Import a disc image into an RVT-H disk image.
 * Compatibility wrapper; this function calls rvth_open() and rvth_copy_to_hdd().
 * @param rvth		[in] RVT-H disk image.
 * @param bank		[in] Bank number. (0-7)
 * @param filename	[in] Source GCM filename.
 * @param callback	[in,opt] Progress callback.
 * @return Error code. (If negative, POSIX error; otherwise, see RvtH_Errors.)
 */
int rvth_import(RvtH *rvth, unsigned int bank, const TCHAR *filename, RvtH_Progress_Callback callback)
{
	RvtH *rvth_src;
	int ret;

	if (!rvth || !filename || filename[0] == 0) {
		errno = EINVAL;
		return -EINVAL;
	} else if (bank >= rvth->bank_count) {
		// Bank number is out of range.
		errno = ERANGE;
		return -ERANGE;
	}

	// Open the standalone disc image.
	ret = 0;
	rvth_src = rvth_open(filename, &ret);
	if (!rvth_src) {
		// Error opening the standalone disc image.
		if (ret == 0) {
			ret = -EIO;
		}
		return ret;
	} else if (rvth_is_hdd(rvth_src) || rvth_src->bank_count > 1) {
		// Not a standalone disc image.
		rvth_close(rvth_src);
		errno = EINVAL;
		return RVTH_ERROR_IS_HDD_IMAGE;
	} else if (rvth_src->bank_count == 0) {
		// Unrecognized file format.
		// TODO: Distinguish between unrecognized and no banks.
		errno = EINVAL;
		return RVTH_ERROR_NO_BANKS;
	}

	// Copy the bank from the source GCM to the HDD.
	// TODO: HDD to HDD?
	// NOTE: `bank` parameter starts at 0, not 1.
	ret = rvth_copy_to_hdd(rvth, bank, rvth_src, 0, callback);
	if (ret == 0) {
		// Must convert to debug realsigned for use on RVT-H.
		const RvtH_BankEntry *entry = rvth_get_BankEntry(rvth, bank, NULL);
		if (entry &&
			(entry->type == RVTH_BankType_Wii_SL ||
			 entry->type == RVTH_BankType_Wii_DL) &&
			(entry->crypto_type == RVTH_CryptoType_Retail ||
			 entry->crypto_type == RVTH_CryptoType_Korean ||
		         entry->ticket.sig_status != RVTH_SigStatus_OK ||
			 entry->tmd.sig_status != RVTH_SigStatus_OK))
		{
			// Retail or Korean encryption, or invalid signature.
			// Convert to Debug.
			ret = rvth_recrypt_partitions(rvth, bank, RVTH_CryptoType_Debug, callback);
		}
		else
		{
			// No recryption needed.
			// Write the identifier to indicate that this bank was imported.
			ret = rvth_recrypt_id(rvth, bank);
		}
	}
	rvth_close(rvth_src);
	return ret;
}
