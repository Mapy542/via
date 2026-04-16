/**
 * @file MirrorPathResolver.h
 * @brief Shared helpers for resolving Drive names to unique mirror paths
 */

#ifndef MIRRORPATHRESOLVER_H
#define MIRRORPATHRESOLVER_H

#include <QSet>
#include <QString>

struct SyncSettings;
class SyncDatabase;

namespace MirrorPathResolver {

/**
 * @brief Resolve a desired relative local path to a unique mirror path.
 *
 * Reuses an existing fileId->localPath mapping when it still matches the same
 * parent and remote base name, otherwise claims the unsuffixed name if
 * available and falls back to the configured duplicate-name strategy.
 */
QString resolveUniqueLocalPath(const QString& desiredLocalPath, const QString& fileId,
                               const SyncDatabase* database, const SyncSettings& settings,
                               const QString& syncFolder,
                               const QSet<QString>* additionalClaims = nullptr,
                               const QString& currentLocalPath = QString(),
                               bool reuseExistingMapping = true);

/**
 * @brief Resolve a remote file name within a parent path to a unique mirror path.
 */
QString resolveRemoteLocalPath(const QString& parentLocalPath, const QString& remoteName,
                               const QString& fileId, const SyncDatabase* database,
                               const SyncSettings& settings, const QString& syncFolder,
                               const QSet<QString>* additionalClaims = nullptr,
                               bool reuseExistingMapping = true);

}  // namespace MirrorPathResolver

#endif  // MIRRORPATHRESOLVER_H