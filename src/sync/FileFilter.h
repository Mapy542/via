/**
 * @file FileFilter.h
 * @brief Shared remote file filter helper
 */

#ifndef FILEFILTER_H
#define FILEFILTER_H

#include "SyncSettings.h"

struct DriveFile;

namespace FileFilter {
bool shouldSkipRemoteFile(const DriveFile& file, const SyncSettings& settings);
}

#endif  // FILEFILTER_H
