/**
 * @file CacheMaintenance.cpp
 * @brief Helpers for restart-gated FUSE cache maintenance.
 */

#include "CacheMaintenance.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include "sync/SyncDatabase.h"

namespace {

bool removeEntry(const QFileInfo& entryInfo) {
    const QString path = entryInfo.absoluteFilePath();
    return entryInfo.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
}

}  // namespace

namespace CacheMaintenance {

bool purgeFuseRepresentationCache(const QString& evictableCacheRoot, SyncDatabase& syncDatabase) {
    if (evictableCacheRoot.isEmpty()) {
        qWarning() << "CacheMaintenance: cache root is empty";
        return false;
    }

    if (!syncDatabase.clearFuseRepresentationState()) {
        qWarning() << "CacheMaintenance: failed to clear FUSE representation state";
        return false;
    }

    QDir cacheDir(evictableCacheRoot);
    if (!cacheDir.exists()) {
        qInfo() << "CacheMaintenance: cache root already absent:" << evictableCacheRoot;
        return true;
    }

    bool diskOk = true;
    const QFileInfoList entries = cacheDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QFileInfo& entryInfo : entries) {
        if (!removeEntry(entryInfo)) {
            qWarning() << "CacheMaintenance: failed to remove" << entryInfo.absoluteFilePath();
            diskOk = false;
        }
    }

    if (diskOk) {
        qInfo() << "CacheMaintenance: purged evictable FUSE cache root:" << evictableCacheRoot;
    } else {
        qWarning() << "CacheMaintenance: cache purge incomplete for" << evictableCacheRoot;
    }

    return diskOk;
}

}  // namespace CacheMaintenance