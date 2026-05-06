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

    if (!file.ownedByMe) {
        return true;
    }

    // Native-doc visibility depends on shared policy plus per-file overrides,
    // so mirror callers decide that after loading SyncDatabase state.
    return false;
}
}  // namespace FileFilter
