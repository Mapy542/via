/**
 * @file FileFilter.cpp
 * @brief Shared remote file filter helper
 */

#include "FileFilter.h"

#include "api/DriveFile.h"

namespace FileFilter {
bool shouldSkipRemoteFile(const DriveFile& file, const SyncSettings&) {
    if (file.trashed) {
        return false;  // We want to sync trashed files to detect deletions
    }

    if (file.isGoogleDoc() && !file.isFolder && !file.isShortcut) {
        return true;
    }

    if (!file.ownedByMe) {
        return true;
    }

    return false;
}
}  // namespace FileFilter
