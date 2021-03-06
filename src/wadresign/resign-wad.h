/***************************************************************************
 * RVT-H Tool: WAD Resigner                                                *
 * resign-wad.h: Re-sign a WAD file.                                       *
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

#ifndef __RVTHTOOL_WADRESIGN_RESIGN_WAD_H__
#define __RVTHTOOL_WADRESIGN_RESIGN_WAD_H__

#include "librvth/tcharx.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 'resign' command.
 * @param src_wad	[in] Source WAD.
 * @param dest_wad	[in] Destination WAD.
 * @param recrypt_key	[in] Key for recryption. (-1 for default)
 * @return 0 on success; negative POSIX error code or positive ID code on error.
 */
int resign_wad(const TCHAR *src_wad, const TCHAR *dest_wad, int recrypt_key);

#ifdef __cplusplus
}
#endif

#endif /* __RVTHTOOL_WADRESIGN_PRINT_INFO_H__ */
