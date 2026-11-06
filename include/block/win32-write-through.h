/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLOCK_WIN32_WRITE_THROUGH_H
#define BLOCK_WIN32_WRITE_THROUGH_H

#include <windows.h>
#include <stdbool.h>
#include <wchar.h>

/*
 * NTFS write-through also persists the metadata needed to retrieve the data.
 * Other filesystems and device handles retain explicit flushes. Both flags
 * are required to bypass the system cache and request durable device writes.
 */
static inline bool win32_write_through_is_durable(HANDLE file, DWORD flags)
{
    const DWORD required = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
    FILE_STANDARD_INFO info;
    WCHAR filesystem[32];

    if ((flags & required) != required || GetFileType(file) != FILE_TYPE_DISK) {
        return false;
    }
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &info,
                                    sizeof(info)) || info.Directory) {
        return false;
    }
    if (!GetVolumeInformationByHandleW(file, NULL, 0, NULL, NULL, NULL,
                                     filesystem, sizeof(filesystem) /
                                                     sizeof(filesystem[0]))) {
        return false;
    }
    return wcscmp(filesystem, L"NTFS") == 0;
}

#endif
