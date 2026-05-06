/**
 * @file StartupMaintenance.cpp
 * @brief Startup compatibility policy helpers.
 */

#include "StartupMaintenance.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "utils/PathUtils.h"

namespace {

bool removeMirrorArtifactIfPresent(const QString& absolutePath, const QString& syncRoot,
                                   bool* removedArtifact) {
    if (removedArtifact) {
        *removedArtifact = false;
    }

    const QFileInfo entryInfo(absolutePath);
    if (!entryInfo.exists() && !PathUtils::isSymlink(entryInfo)) {
        return true;
    }

    const QString cleanAbsolutePath = QDir::cleanPath(QFileInfo(absolutePath).absoluteFilePath());
    const QString cleanSyncRoot = QDir::cleanPath(QFileInfo(syncRoot).absoluteFilePath());
    if (!PathUtils::isPathWithinRootBoundary(cleanAbsolutePath, cleanSyncRoot)) {
        qWarning() << "StartupMaintenance: refusing to purge path outside sync root:"
                   << cleanAbsolutePath;
        return false;
    }

    bool removed = false;
    if (PathUtils::isSymlink(entryInfo)) {
        removed = QFile::remove(cleanAbsolutePath);
    } else if (entryInfo.isDir()) {
        if (!PathUtils::isCanonicalPathWithinRoot(cleanAbsolutePath, cleanSyncRoot)) {
            qWarning() << "StartupMaintenance: refusing to recurse outside sync root:"
                       << cleanAbsolutePath;
            return false;
        }
        removed = QDir(cleanAbsolutePath).removeRecursively();
    } else {
        removed = QFile::remove(cleanAbsolutePath);
    }

    if (removedArtifact) {
        *removedArtifact = removed;
    }
    return removed;
}

}  // namespace

namespace StartupMaintenance {

bool shouldPurgeFuseRepresentationCache(const FuseMaintenanceInputs& inputs) {
    return inputs.currentNativeDocMode != inputs.previousNativeDocMode ||
           inputs.pendingRepresentationReset || inputs.pendingCachePurge ||
           inputs.currentRepresentationEpoch > inputs.storedRepresentationEpoch;
}

bool shouldRebuildMirrorRepresentation(const MirrorMaintenanceInputs& inputs) {
    return inputs.currentNativeDocMode != inputs.previousNativeDocMode ||
           inputs.pendingRepresentationReset ||
           inputs.currentRepresentationEpoch > inputs.storedRepresentationEpoch;
}

bool purgeMirrorNativeDocArtifacts(const QString& syncFolder, SyncDatabase& syncDatabase,
                                   MirrorRepresentationRebuildStats* stats) {
    if (syncFolder.isEmpty()) {
        qWarning() << "StartupMaintenance: sync folder is empty";
        return false;
    }

    const QString cleanSyncRoot = QDir::cleanPath(QFileInfo(syncFolder).absoluteFilePath());
    if (cleanSyncRoot.isEmpty()) {
        qWarning() << "StartupMaintenance: failed to resolve sync folder:" << syncFolder;
        return false;
    }

    MirrorRepresentationRebuildStats localStats;
    bool purgeOk = true;

    const QList<FileSyncState> fileStates = syncDatabase.getAllFiles();
    for (const FileSyncState& fileState : fileStates) {
        if (fileState.fileId.isEmpty()) {
            continue;
        }

        const NativeDocState nativeDocState = syncDatabase.getNativeDocState(fileState.fileId);
        if (!isNativeDocMimeType(nativeDocState.remoteMimeType)) {
            continue;
        }

        const QString absolutePath = QDir(cleanSyncRoot).filePath(fileState.localPath);
        bool removedArtifact = false;
        if (!removeMirrorArtifactIfPresent(absolutePath, cleanSyncRoot, &removedArtifact)) {
            qWarning() << "StartupMaintenance: failed to remove mirror native-doc artifact:"
                       << absolutePath;
            purgeOk = false;
            continue;
        }

        if (!syncDatabase.deleteFileStateById(fileState.fileId)) {
            qWarning() << "StartupMaintenance: failed to clear mirror mapping for native-doc:"
                       << fileState.fileId;
            purgeOk = false;
            continue;
        }

        syncDatabase.clearDeletedFile(fileState.localPath);
        if (removedArtifact) {
            ++localStats.removedArtifactCount;
        }
        ++localStats.clearedMappingCount;
    }

    syncDatabase.setChangeToken(QString());

    if (stats) {
        *stats = localStats;
    }
    return purgeOk;
}

SyncResetDecision classifySyncReset(SyncDatabase::SchemaCompatibility compatibility) {
    SyncResetDecision decision;

    switch (compatibility) {
        case SyncDatabase::SchemaCompatibility::Current:
            break;
        case SyncDatabase::SchemaCompatibility::ResetRequired:
            decision.requiresReset = true;
            decision.requestFullSyncAfterReset = true;
            break;
        case SyncDatabase::SchemaCompatibility::ResetBlockedByDirtyState:
            decision.requiresReset = true;
            decision.requiresExplicitDiscard = true;
            decision.requestFullSyncAfterReset = true;
            break;
        case SyncDatabase::SchemaCompatibility::UnsupportedFutureSchema:
            decision.unsupportedFutureSchema = true;
            break;
    }

    return decision;
}

}  // namespace StartupMaintenance