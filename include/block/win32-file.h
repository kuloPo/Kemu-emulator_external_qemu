/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLOCK_WIN32_FILE_H
#define BLOCK_WIN32_FILE_H

#include <windows.h>
#include <errno.h>

static inline int win32_file_error(DWORD error)
{
    return error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL
           ? -ENOSPC : -EIO;
}

/* Windows 8 information classes queried with the Windows 7 SDK target. */
typedef struct Win32FileStorageInfo {
    ULONG logical_sector;
    ULONG physical_atomicity;
    ULONG physical_performance;
    ULONG filesystem_atomicity;
    ULONG flags;
    ULONG sector_offset;
    ULONG partition_offset;
} Win32FileStorageInfo;

#define WIN32_FILE_STORAGE_INFO ((FILE_INFO_BY_HANDLE_CLASS)16)
#define WIN32_FILE_ALIGNMENT_INFO ((FILE_INFO_BY_HANDLE_CLASS)17)

#endif
