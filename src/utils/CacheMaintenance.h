/**
 * @file CacheMaintenance.h
 * @brief Helpers for restart-gated FUSE cache maintenance.
 */

#ifndef CACHEMAINTENANCE_H
#define CACHEMAINTENANCE_H

#include <QString>

class SyncDatabase;

namespace CacheMaintenance {

/**
 * @brief Purge evictable FUSE cache files and representation state.
 *
 * Deletes all entries under the evictable cache root and clears
 * representation tables in SyncDatabase. Pending uploads stored under
 * AppLocalDataLocation and fuse_dirty_files are intentionally preserved.
 *
 * @param evictableCacheRoot App-specific cache root under CacheLocation
 * @param syncDatabase Sync database whose representation state should be cleared
 * @return true when both the database reset and on-disk purge succeed
 */
bool purgeFuseRepresentationCache(const QString& evictableCacheRoot, SyncDatabase& syncDatabase);

}  // namespace CacheMaintenance

#endif  // CACHEMAINTENANCE_H