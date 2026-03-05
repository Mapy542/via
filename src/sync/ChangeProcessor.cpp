/**
 * @file ChangeProcessor.cpp
 * @brief Implementation of the Change Processor/Conflict Resolver Thread
 */

#include "ChangeProcessor.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTimer>

#include "ChangeQueue.h"
#include "SyncActionQueue.h"
#include "SyncDatabase.h"
#include "SyncSettings.h"
#include "api/GoogleDriveClient.h"

// Static registration for QMetaType
static const int conflictInfoTypeId = qRegisterMetaType<ConflictInfo>("ConflictInfo");
static const int conflictStrategyTypeId = qRegisterMetaType<ConflictResolutionStrategy>("ConflictResolutionStrategy");

ChangeProcessor::ChangeProcessor(ChangeQueue* changeQueue, SyncActionQueue* syncActionQueue, SyncDatabase* database,
                                 GoogleDriveClient* driveClient, QObject* parent)
    : QObject(parent),
      m_changeQueue(changeQueue),
      m_syncActionQueue(syncActionQueue),
      m_database(database),
      m_driveClient(driveClient),
      m_state(State::Stopped),
      m_conflictStrategy(ConflictResolutionStrategy::KeepNewest),
      m_cachedSettings(SyncSettings::load()),
      m_processingActive(false) {
    // Suppress unused variable warnings
    Q_UNUSED(conflictInfoTypeId);
    Q_UNUSED(conflictStrategyTypeId);

    // Connect to Change Queue's itemsAvailable signal (Jobs Available Wakeup)
    if (m_changeQueue) {
        connect(m_changeQueue, &ChangeQueue::itemsAvailable, this, &ChangeProcessor::onItemsAvailable);
    }
}

ChangeProcessor::~ChangeProcessor() { stop(); }

ChangeProcessor::State ChangeProcessor::state() const {
    QMutexLocker locker(&m_stateMutex);
    return m_state;
}

void ChangeProcessor::setConflictResolutionStrategy(ConflictResolutionStrategy strategy) {
    m_conflictStrategy = strategy;
    qDebug() << "Conflict resolution strategy set to:" << static_cast<int>(strategy);
}

ConflictResolutionStrategy ChangeProcessor::conflictResolutionStrategy() const { return m_conflictStrategy; }

void ChangeProcessor::setSyncFolder(const QString& path) {
    m_syncFolder = path;
    qDebug() << "ChangeProcessor sync folder set to:" << path;
}

QString ChangeProcessor::syncFolder() const { return m_syncFolder; }

void ChangeProcessor::onSyncFolderChanged(const QString& path) { setSyncFolder(path); }

QList<ConflictInfo> ChangeProcessor::unresolvedConflicts() const {
    QMutexLocker locker(&m_conflictsMutex);
    return m_unresolvedConflicts.values();
}

int ChangeProcessor::unresolvedConflictCount() const {
    QMutexLocker locker(&m_conflictsMutex);
    return m_unresolvedConflicts.count();
}

bool ChangeProcessor::isFileInOperation(const QString& localPath) const {
    QMutexLocker locker(&m_filesInOpMutex);
    return m_filesInOperation.contains(localPath);
}

void ChangeProcessor::markFileInOperation(const QString& localPath) {
    QMutexLocker locker(&m_filesInOpMutex);
    m_filesInOperation.insert(localPath);
    qDebug() << "File marked in operation:" << localPath;
}

void ChangeProcessor::unmarkFileInOperation(const QString& localPath) {
    QMutexLocker locker(&m_filesInOpMutex);
    m_filesInOperation.remove(localPath);
    qDebug() << "File unmarked from operation:" << localPath;
}

void ChangeProcessor::start() {
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state == State::Running) {
            qDebug() << "ChangeProcessor already running";
            return;
        }
        m_state = State::Running;
    }

    m_processingActive = true;
    m_cachedSettings = SyncSettings::load();
    emit stateChanged(State::Running);

    qInfo() << "ChangeProcessor started";

    rehydrateUnresolvedConflicts();

    // Start processing immediately if there are items
    QTimer::singleShot(0, this, &ChangeProcessor::processNextChange);
}

void ChangeProcessor::stop() {
    {
        QMutexLocker locker(&m_stateMutex);
        m_state = State::Stopped;
    }

    m_processingActive = false;
    emit stateChanged(State::Stopped);

    qInfo() << "ChangeProcessor stopped";
}

void ChangeProcessor::clearState() {
    {
        QMutexLocker locker(&m_conflictsMutex);
        m_unresolvedConflicts.clear();
    }
    {
        QMutexLocker locker(&m_filesInOpMutex);
        m_filesInOperation.clear();
    }
    qInfo() << "ChangeProcessor: in-memory state cleared (account sign-out)";
}

void ChangeProcessor::pause() {
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state != State::Running) {
            return;
        }
        m_state = State::Paused;
    }

    m_processingActive = false;
    emit stateChanged(State::Paused);

    qInfo() << "ChangeProcessor paused";
}

void ChangeProcessor::resume() {
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state != State::Paused) {
            return;
        }
        m_state = State::Running;
    }

    m_processingActive = true;
    emit stateChanged(State::Running);

    qInfo() << "ChangeProcessor resumed";

    // Resume processing
    QTimer::singleShot(0, this, &ChangeProcessor::processNextChange);
}

void ChangeProcessor::onItemsAvailable() {
    // Jobs Available Wakeup Signal/Slot handler
    // This is called when the Change Queue transitions from empty to non-empty
    qDebug() << "ChangeProcessor received items available signal";

    QMutexLocker locker(&m_stateMutex);
    if (m_state == State::Running && !m_processingActive) {
        m_processingActive = true;
        QTimer::singleShot(0, this, &ChangeProcessor::processNextChange);
    }
}

void ChangeProcessor::resolveConflict(const QString& localPath, ConflictResolutionStrategy strategy) {
    ConflictInfo conflict;
    {
        QMutexLocker locker(&m_conflictsMutex);
        if (!m_unresolvedConflicts.contains(localPath)) {
            qWarning() << "No unresolved conflict found for:" << localPath;
            return;
        }
        conflict = m_unresolvedConflicts.take(localPath);
    }

    if (m_database) {
        m_database->markConflictResolved(localPath);
    }

    resolveConflictInternal(conflict, strategy);
    emit conflictResolved(localPath, strategy);
}

void ChangeProcessor::rehydrateUnresolvedConflicts() {
    if (!m_database) {
        return;
    }

    QList<ConflictRecord> records = m_database->getUnresolvedConflicts();
    if (records.isEmpty()) {
        return;
    }

    QList<ConflictInfo> conflictsToEmit;
    {
        QMutexLocker locker(&m_conflictsMutex);
        m_unresolvedConflicts.clear();
        for (const ConflictRecord& record : records) {
            ConflictInfo info;
            info.conflictId = record.id;
            info.localPath = record.localPath;
            info.fileId = record.fileId;
            info.resolved = record.resolved;
            info.isConflicted = true;

            if (!record.versions.isEmpty()) {
                const ConflictVersion& latest = record.versions.last();
                info.localModifiedTime = latest.localModifiedTime;
                info.remoteModifiedTime = latest.remoteModifiedTime;
                info.dbSyncTime = latest.dbSyncTime;
            }

            m_unresolvedConflicts[info.localPath] = info;
            conflictsToEmit.append(info);
        }
    }

    for (const ConflictInfo& info : conflictsToEmit) {
        emit conflictDetected(info);
    }
}

void ChangeProcessor::processNextChange() {
    // Check if we should continue processing
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state != State::Running) {
            m_processingActive = false;
            return;
        }
    }

    // Check for items in the queue
    if (!m_changeQueue || m_changeQueue->isEmpty()) {
        m_processingActive = false;
        qDebug() << "ChangeProcessor idle - waiting for itemsAvailable signal";
        return;
    }

    // Dequeue and process the next change
    ChangeQueueItem change = m_changeQueue->dequeue();
    if (!change.isValid()) {
        // Invalid item, continue to next
        QTimer::singleShot(0, this, &ChangeProcessor::processNextChange);
        return;
    }

    qDebug() << "Processing change:" << change.localPath << "type:" << static_cast<int>(change.changeType)
             << "origin:" << static_cast<int>(change.origin);

    // Step 1: Validate the change
    if (!validateChange(change)) {
        emit changeSkipped(change.localPath, "Validation failed, skipping change");
        QTimer::singleShot(0, this, &ChangeProcessor::processNextChange);
        return;
    }

    if (m_database && !change.localPath.isEmpty() && m_database->hasUnresolvedConflict(change.localPath)) {
        qWarning() << "Change skipped - unresolved conflict exists:" << change.localPath;
        appendConflictVersionForChange(change);
        emit changeSkipped(change.localPath, "Unresolved conflict exists, queued as new version");
        QTimer::singleShot(0, this, &ChangeProcessor::processNextChange);
        return;
    }

    // Step 2: Check for conflicts (may involve long network call)
    ConflictInfo conflict = checkForConflict(change);
    if (conflict.isConflicted) {
        // Conflict detected
        qInfo() << "Conflict detected for:" << conflict.localPath;

        // Step 3: Resolve conflict based on strategy
        if (m_conflictStrategy == ConflictResolutionStrategy::AskUser) {
            ConflictInfo storedConflict = storeManualConflict(conflict);
            emit conflictDetected(storedConflict);
        } else {
            emit conflictDetected(conflict);
            // Auto-resolve
            resolveConflictInternal(conflict, m_conflictStrategy);
            emit conflictResolved(conflict.localPath, m_conflictStrategy);
        }
    } else {
        // No conflict - determine and queue sync actions
        determineAndQueueActions(change);
        emit changeProcessed(change.localPath);
    }

    // Continue processing next change
    QTimer::singleShot(0, this, &ChangeProcessor::processNextChange);
}

bool ChangeProcessor::validateChange(ChangeQueueItem& change) {
    if (!m_database) {
        qWarning() << "Validation failed - database unavailable";
        return false;
    }

    QString localPath = change.localPath;
    QString fileId = change.fileId;

    if (change.origin == ChangeOrigin::Local && fileId.isEmpty() && !localPath.isEmpty()) {
        fileId = m_database->getFileId(localPath);
    }

    FileSyncState dbState;
    bool hasDbState = false;
    if (!fileId.isEmpty()) {
        dbState = m_database->getFileStateById(fileId);
        hasDbState = !dbState.fileId.isEmpty() || !dbState.localPath.isEmpty();
    }
    if (!hasDbState && !localPath.isEmpty()) {
        dbState = m_database->getFileState(localPath);
        hasDbState = !dbState.fileId.isEmpty() || !dbState.localPath.isEmpty();
    }

    if (localPath.isEmpty() && !dbState.localPath.isEmpty()) {
        localPath = dbState.localPath;
    }
    if (fileId.isEmpty() && !dbState.fileId.isEmpty()) {
        fileId = dbState.fileId;
    }

    if (change.localPath != localPath) {
        change.localPath = localPath;
    }
    if (change.fileId != fileId) {
        change.fileId = fileId;
    }

    if (localPath.isEmpty() && fileId.isEmpty()) {
        qWarning() << "Validation failed - missing identifiers";
        return false;
    }

    const QString changeId = localPath.isEmpty() ? fileId : localPath;

    // Validation Step 1: File not in operation
    if (!localPath.isEmpty() && isFileInOperation(localPath)) {
        qWarning() << "Validation failed - file in operation (false trigger):" << localPath;
        return false;
    }

    // Validation Step 2: Folder additional checks
    if (change.origin == ChangeOrigin::Local && change.isDirectory && !localPath.isEmpty()) {
        switch (change.changeType) {
            case ChangeType::Create:
            case ChangeType::Modify:
                // False positive when child modified triggers parent folder create
                // Folder invalid if already exists and is not deleted
                {
                    FileSyncState folderState = m_database->getFileState(localPath);
                    if (!folderState.fileId.isEmpty() && m_database->wasFileDeleted(localPath) == false) {
                        qDebug() << "Validation failed - folder already exists:" << localPath;
                        return false;
                    }
                }
                break;
                // TODO: additional cases for folder validation if needed
            default:
                break;
        }
    }

    // Allow remote moves/renames even when modified time doesn't change.
    if (change.origin == ChangeOrigin::Remote && hasDbState && !dbState.localPath.isEmpty() && !localPath.isEmpty() &&
        localPath != dbState.localPath) {
        const QString oldPath = dbState.localPath;
        const QString newPath = localPath;
        QString oldDir = QFileInfo(oldPath).path();
        QString newDir = QFileInfo(newPath).path();
        if (oldDir == ".") {
            oldDir.clear();
        }
        if (newDir == ".") {
            newDir.clear();
        }

        change.localPath = oldPath;
        if (oldDir != newDir) {
            change.changeType = ChangeType::Move;
            change.moveDestination = newPath;
            change.renameTo.clear();
        } else {
            change.changeType = ChangeType::Rename;
            change.renameTo = QFileInfo(newPath).fileName();
            change.moveDestination.clear();
        }

        qDebug() << "Validation passed - remote path changed:" << changeId << "from" << oldPath << "to" << newPath;
        if (!dedupValidate(change)) {
            return false;
        }
        return true;
    }

    // Validation Step 3: File date modified > db recorded last sync time + 2 sec
    if (change.changeType == ChangeType::Move || change.changeType == ChangeType::Rename) {
        const bool hasSource = !localPath.isEmpty();
        if (!hasSource) {
            qWarning() << "Validation failed - move/rename missing source path:" << changeId;
            return false;
        }
        if (change.origin == ChangeOrigin::Local && fileId.isEmpty()) {
            qWarning() << "Validation failed - local move/rename missing fileId:" << localPath;
            return false;
        }
        if (change.changeType == ChangeType::Move) {
            if (change.moveDestination.isEmpty()) {
                qWarning() << "Validation failed - move missing destination:" << localPath;
                return false;
            }
            if (change.moveDestination == localPath) {
                qDebug() << "Validation failed - move destination matches source:" << localPath;
                return false;
            }
            FileSyncState destState = m_database->getFileState(change.moveDestination);
            if (!destState.fileId.isEmpty() && !fileId.isEmpty() && destState.fileId != fileId &&
                !m_database->wasFileDeleted(change.moveDestination)) {
                qDebug() << "Validation failed - move destination already mapped:" << change.moveDestination;
                return false;
            }
        } else {
            if (change.renameTo.isEmpty()) {
                qWarning() << "Validation failed - rename missing target name:" << localPath;
                return false;
            }
        }
    }

    if (change.changeType == ChangeType::Create || change.changeType == ChangeType::Modify) {
        if (change.origin == ChangeOrigin::Local && !change.isDirectory && hasDbState &&
            !dbState.localHashAtSync.isEmpty()) {
            if (change.localContentHash.isEmpty()) {
                change.localContentHash = computeLocalFileMd5(localPath);
            }
            if (!change.localContentHash.isEmpty() && change.localContentHash == dbState.localHashAtSync) {
                qDebug() << "Validation failed - local content unchanged by hash:" << changeId;
                return false;
            }
        }

        QDateTime dbSyncTime = hasDbState ? dbState.modifiedTimeAtSync : QDateTime();

        if (dbSyncTime.isValid()) {
            // Change must be at least 2 seconds newer than the DB record
            QDateTime changeModTime = change.modifiedTime.toUTC();
            if (change.origin == ChangeOrigin::Local && !localPath.isEmpty()) {  // TODO: bug??
                QFileInfo localInfo(QDir(m_syncFolder).filePath(localPath));
                if (localInfo.exists()) {
                    changeModTime = localInfo.lastModified().toUTC();
                }
            }
            if (!changeModTime.isValid()) {
                qDebug() << "Validation passed - missing change modified time:" << changeId;
                if (!dedupValidate(change)) {
                    return false;
                }
                return true;
            }
            qint64 diffSecs = dbSyncTime.secsTo(changeModTime);
            if (diffSecs < MIN_CHANGE_DIFF_SECS) {
                qDebug() << "Validation failed - change not new enough:" << changeId << "diff:" << diffSecs << "secs";
                return false;
            }
            qDebug() << "Validation passed for modification time check:" << changeId << "diff:" << diffSecs << "secs"
                     << " DB time:" << dbSyncTime.toString() << " Change time:" << change.modifiedTime.toString();
        }
        // If no DB record exists, this is a new file - allow it
    }

    // Deletion Validations:
    if (change.changeType == ChangeType::Delete) {
        if (localPath.isEmpty()) {
            qWarning() << "Validation failed - delete missing local path:" << changeId;
            return false;
        }
        if (fileId.isEmpty()) {
            fileId = m_database->getFileId(localPath);
            if (fileId.isEmpty()) {
                qWarning() << "Validation failed - delete missing fileId:" << localPath;
                return false;
            }
        }
        if (!m_database->wasFileDeleted(localPath)) {
            qDebug() << "Validation passed for deletion - file not marked as deleted in DB:" << localPath;
        } else {
            qDebug() << "Validation failed - file already marked as deleted in DB:" << localPath;
            return false;
        }
    }

    if (!dedupValidate(change)) {
        return false;
    }

    qDebug() << "Change validated:" << changeId;
    return true;
}

bool ChangeProcessor::dedupValidate(const ChangeQueueItem& change) {
    if (!m_syncActionQueue) {
        return true;
    }

    SyncActionItem action;
    if (!buildSyncActionForChange(change, action)) {
        return true;
    }

    if (!action.isValid()) {
        return true;
    }

    if (!m_syncActionQueue->containsDuplicatePending(action)) {
        return true;
    }

    QString skippedPath = change.localPath;
    if (skippedPath.isEmpty()) {
        skippedPath = action.localPath;
    }
    emit changeSkipped(skippedPath, "duplicate pending sync action");
    qInfo() << "Validation failed - duplicate pending sync action:" << skippedPath
            << "type:" << static_cast<int>(action.actionType) << "fileId:" << action.fileId;
    return false;
}

ConflictInfo ChangeProcessor::storeManualConflict(const ConflictInfo& conflict) {
    ConflictInfo stored = conflict;
    if (m_database && !conflict.localPath.isEmpty()) {
        int conflictId = m_database->upsertConflictRecord(conflict.localPath, conflict.fileId, QString());
        stored.conflictId = conflictId;

        ConflictVersion version;
        version.localModifiedTime = conflict.localModifiedTime;
        version.remoteModifiedTime = conflict.remoteModifiedTime;
        version.dbSyncTime = conflict.dbSyncTime;
        version.detectedAt = QDateTime::currentDateTime();
        m_database->addConflictVersion(conflictId, version);
    }

    {
        QMutexLocker locker(&m_conflictsMutex);
        if (!stored.localPath.isEmpty()) {
            m_unresolvedConflicts[stored.localPath] = stored;
        }
    }

    return stored;
}

void ChangeProcessor::appendConflictVersionForChange(const ChangeQueueItem& change) {
    ConflictInfo conflict;
    conflict.localPath = change.localPath;
    conflict.fileId = change.fileId;
    conflict.dbSyncTime = m_database ? m_database->getModifiedTimeAtSync(change.localPath) : QDateTime();
    conflict.isConflicted = true;

    if (change.origin == ChangeOrigin::Local) {
        conflict.localModifiedTime = change.modifiedTime;
    } else {
        conflict.remoteModifiedTime = change.modifiedTime;
    }

    ConflictInfo stored = storeManualConflict(conflict);
    emit conflictDetected(stored);
}

QString ChangeProcessor::computeLocalFileMd5(const QString& localPath) const {
    if (localPath.isEmpty()) {
        return QString();
    }

    QString absolutePath = QDir(m_syncFolder).filePath(localPath);
    QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile()) {
        return QString();
    }

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Md5);
    if (!hash.addData(&file)) {
        return QString();
    }

    return QString::fromLatin1(hash.result().toHex());
}

ConflictInfo ChangeProcessor::checkForConflict(ChangeQueueItem& change) {
    ConflictInfo conflict;

    // Only check for conflicts if we have both database and drive client
    if (!m_database) {
        return conflict;
    }

    qInfo() << "ConflictCheck start"
            << "origin=" << static_cast<int>(change.origin) << "type=" << static_cast<int>(change.changeType)
            << "isDir=" << change.isDirectory << "path=" << change.localPath << "fileId=" << change.fileId;

    if (change.origin == ChangeOrigin::Remote &&
        (change.isDirectory || change.changeType == ChangeType::Move || change.changeType == ChangeType::Rename)) {
        qInfo() << "ConflictCheck skip"
                << "reason=Skipped_PathOpRemote"
                << "type=" << static_cast<int>(change.changeType) << "isDir=" << change.isDirectory
                << "path=" << change.localPath << "moveDestination=" << change.moveDestination
                << "renameTo=" << change.renameTo;
        return conflict;
    }

    FileSyncState dbState;
    if (!change.fileId.isEmpty()) {
        dbState = m_database->getFileStateById(change.fileId);
    }
    if (dbState.fileId.isEmpty() && !change.localPath.isEmpty()) {
        dbState = m_database->getFileState(change.localPath);
    }

    const QString baselineRemoteMd5 = dbState.remoteMd5AtSync;
    const QString baselineLocalHash = dbState.localHashAtSync;

    // Remote-origin changes: detect local changes since last sync
    if (change.origin == ChangeOrigin::Remote) {
        QFileInfo localInfo(QDir(m_syncFolder).filePath(change.localPath));
        if (!localInfo.exists()) {
            return conflict;
        }

        QDateTime localModTime = localInfo.lastModified().toUTC();
        QDateTime remoteModTime = change.modifiedTime.toUTC();
        QDateTime dbSyncTime = m_database->getModifiedTimeAtSync(change.localPath);

        if (!change.isDirectory && change.localContentHash.isEmpty()) {
            change.localContentHash = computeLocalFileMd5(change.localPath);
        }

        if (!change.isDirectory && !change.localContentHash.isEmpty() && !change.remoteMd5.isEmpty() &&
            change.localContentHash == change.remoteMd5) {
            qInfo() << "ConflictCheck skip"
                    << "reason=NoConflict_HashEqual"
                    << "path=" << change.localPath;
            return conflict;
        }

        bool localChangedByHash = false;
        bool remoteChangedByHash = false;
        if (!change.isDirectory && !baselineLocalHash.isEmpty() && !change.localContentHash.isEmpty()) {
            localChangedByHash = (change.localContentHash != baselineLocalHash);
        }
        if (!change.isDirectory && !baselineRemoteMd5.isEmpty() && !change.remoteMd5.isEmpty()) {
            remoteChangedByHash = (change.remoteMd5 != baselineRemoteMd5);
        }

        if (!change.isDirectory && !change.localContentHash.isEmpty() && !change.remoteMd5.isEmpty() &&
            !baselineLocalHash.isEmpty() && !baselineRemoteMd5.isEmpty()) {
            if (localChangedByHash && remoteChangedByHash) {
                conflict.localPath = change.localPath;
                conflict.fileId = change.fileId;
                conflict.localModifiedTime = localModTime;
                conflict.remoteModifiedTime = remoteModTime;
                conflict.dbSyncTime = dbSyncTime;
                conflict.resolved = false;
                conflict.isConflicted = true;
                qInfo() << "Conflict (hash-based): local=" << localModTime << "remote=" << remoteModTime
                        << "db=" << conflict.dbSyncTime;
            } else {
                qInfo() << "ConflictCheck skip"
                        << "reason=NoConflict_HashBaseline"
                        << "path=" << change.localPath << "localChangedByHash=" << localChangedByHash
                        << "remoteChangedByHash=" << remoteChangedByHash;
            }
            return conflict;
        }

        bool localNewerThanBaseline = false;

        if (dbSyncTime.isValid()) {
            localNewerThanBaseline = dbSyncTime.toUTC().secsTo(localModTime) > MIN_CHANGE_DIFF_SECS;
        } else if (remoteModTime.isValid()) {
            localNewerThanBaseline = remoteModTime.secsTo(localModTime) > MIN_CHANGE_DIFF_SECS;
        }

        if (localNewerThanBaseline) {
            conflict.localPath = change.localPath;
            conflict.fileId = change.fileId;
            conflict.localModifiedTime = localModTime;
            conflict.remoteModifiedTime = remoteModTime;
            conflict.dbSyncTime = dbSyncTime;
            conflict.resolved = false;
            conflict.isConflicted = true;

            qInfo() << "Conflict: local=" << localModTime << "remote=" << remoteModTime << "db=" << conflict.dbSyncTime;
        } else {
            qInfo() << "ConflictCheck skip"
                    << "reason=NoConflict_Mtime"
                    << "path=" << change.localPath;
        }
    }

    // Local-origin changes: fetch remote metadata to detect divergence before upload
    if (change.origin == ChangeOrigin::Local && !change.fileId.isEmpty() && !change.isDirectory && m_driveClient) {
        if (change.localContentHash.isEmpty()) {
            change.localContentHash = computeLocalFileMd5(change.localPath);
        }

        DriveFile remoteFile = m_driveClient->getFileMetadataBlocking(change.fileId);
        if (!remoteFile.id.isEmpty()) {
            change.remoteMd5 = remoteFile.md5Checksum;
            const bool localEqualsRemote = !change.localContentHash.isEmpty() && !change.remoteMd5.isEmpty() &&
                                           (change.localContentHash == change.remoteMd5);
            if (localEqualsRemote) {
                qInfo() << "ConflictCheck skip"
                        << "reason=NoConflict_HashEqual"
                        << "path=" << change.localPath;
                return conflict;
            }

            const bool localChangedByHash = !baselineLocalHash.isEmpty() && !change.localContentHash.isEmpty() &&
                                            (change.localContentHash != baselineLocalHash);
            const bool remoteChangedByHash =
                !baselineRemoteMd5.isEmpty() && !change.remoteMd5.isEmpty() && (change.remoteMd5 != baselineRemoteMd5);

            if (!baselineLocalHash.isEmpty() && !baselineRemoteMd5.isEmpty() && localChangedByHash &&
                remoteChangedByHash) {
                QFileInfo localInfo(QDir(m_syncFolder).filePath(change.localPath));
                conflict.localPath = change.localPath;
                conflict.fileId = change.fileId;
                conflict.localModifiedTime = localInfo.lastModified().toUTC();
                conflict.remoteModifiedTime = remoteFile.modifiedTime.toUTC();
                conflict.dbSyncTime = m_database->getModifiedTimeAtSync(change.localPath);
                conflict.resolved = false;
                conflict.isConflicted = true;
                qInfo() << "Conflict (local-origin hash-based):" << change.localPath;
            } else {
                qInfo() << "ConflictCheck skip"
                        << "reason=NoConflict_HashBaseline"
                        << "path=" << change.localPath << "localChangedByHash=" << localChangedByHash
                        << "remoteChangedByHash=" << remoteChangedByHash;
            }
            return conflict;
        }
    }

    return conflict;
}

void ChangeProcessor::resolveConflictInternal(const ConflictInfo& conflict, ConflictResolutionStrategy strategy) {
    qInfo() << "Resolving conflict for:" << conflict.localPath << "with strategy:" << static_cast<int>(strategy);

    // Determine effective strategy for KeepNewest
    ConflictResolutionStrategy effectiveStrategy = strategy;
    if (strategy == ConflictResolutionStrategy::KeepNewest) {
        if (conflict.localModifiedTime > conflict.remoteModifiedTime) {
            effectiveStrategy = ConflictResolutionStrategy::KeepLocal;
            qInfo() << "KeepNewest: Local is newer";
        } else if (conflict.remoteModifiedTime > conflict.localModifiedTime) {
            effectiveStrategy = ConflictResolutionStrategy::KeepRemote;
            qInfo() << "KeepNewest: Remote is newer";
        } else {
            // Same modification time - keep both to be safe
            effectiveStrategy = ConflictResolutionStrategy::KeepBoth;
            qInfo() << "KeepNewest: Same time, keeping both";
        }
    }

    SyncActionItem action;

    switch (effectiveStrategy) {
        case ConflictResolutionStrategy::KeepLocal:
            // Upload local version to overwrite remote
            action.actionType = SyncActionType::Upload;
            action.localPath = conflict.localPath;
            action.fileId = conflict.fileId;
            action.modifiedTime = conflict.localModifiedTime;
            if (m_syncActionQueue) {
                m_syncActionQueue->enqueue(action);
            }
            break;

        case ConflictResolutionStrategy::KeepRemote:
            // Download remote version to overwrite local
            action.actionType = SyncActionType::Download;
            action.localPath = conflict.localPath;
            action.fileId = conflict.fileId;
            action.modifiedTime = conflict.remoteModifiedTime;
            if (m_syncActionQueue) {
                m_syncActionQueue->enqueue(action);
            }
            break;

        case ConflictResolutionStrategy::KeepBoth: {
            // Create a conflict copy of the local file
            QString conflictPath = generateConflictCopyPath(conflict.localPath);

            // First, rename local file to conflict copy (local operation)
            SyncActionItem moveAction;
            moveAction.actionType = SyncActionType::MoveLocal;
            moveAction.localPath = conflict.localPath;
            moveAction.moveDestination = conflictPath;
            moveAction.modifiedTime = conflict.localModifiedTime;
            if (m_syncActionQueue) {
                m_syncActionQueue->enqueue(moveAction);
            }

            // Then download remote version to original path
            action.actionType = SyncActionType::Download;
            action.localPath = conflict.localPath;
            action.fileId = conflict.fileId;
            action.modifiedTime = conflict.remoteModifiedTime;
            if (m_syncActionQueue) {
                m_syncActionQueue->enqueue(action);
            }

            // Finally, upload the conflict copy to remote
            SyncActionItem uploadAction;
            uploadAction.actionType = SyncActionType::Upload;
            uploadAction.localPath = conflictPath;
            uploadAction.modifiedTime = conflict.localModifiedTime;
            if (m_syncActionQueue) {
                m_syncActionQueue->enqueue(uploadAction);
            }
        } break;

        case ConflictResolutionStrategy::KeepNewest:
            // Should not reach here - handled above
            qWarning() << "Unexpected KeepNewest strategy in resolveConflictInternal";
            break;

        case ConflictResolutionStrategy::AskUser:
            // Don't auto-resolve - already stored for manual resolution
            break;
    }
}

void ChangeProcessor::determineAndQueueActions(const ChangeQueueItem& change) {
    const bool remoteReadOnly = m_cachedSettings.isRemoteReadOnly();
    const bool remoteNoDelete = m_cachedSettings.isRemoteNoDelete();

    if ((change.changeType == ChangeType::Create || change.changeType == ChangeType::Modify) && !change.isDirectory) {
        if (change.origin == ChangeOrigin::Remote && !change.remoteMd5.isEmpty()) {
            QString localHash = change.localContentHash;
            if (localHash.isEmpty()) {
                localHash = computeLocalFileMd5(change.localPath);
            }
            if (!localHash.isEmpty() && localHash == change.remoteMd5) {
                qInfo() << "Skipping remote change - local content already matches remote hash:" << change.localPath;
                return;
            }
        }

        if (change.origin == ChangeOrigin::Local && !change.localContentHash.isEmpty() && !change.remoteMd5.isEmpty() &&
            change.localContentHash == change.remoteMd5) {
            qInfo() << "Skipping local change - remote content already matches local hash:" << change.localPath;
            return;
        }
    }

    SyncActionItem action;
    if (!buildSyncActionForChange(change, action)) {
        return;
    }

    // Enforce sync mode on actions only (do not affect detection)
    if (remoteReadOnly &&
        (action.actionType == SyncActionType::Upload || action.actionType == SyncActionType::MoveRemote ||
         action.actionType == SyncActionType::RenameRemote || action.actionType == SyncActionType::DeleteRemote)) {
        qInfo() << "Skipping remote action due to read-only sync mode:" << action.localPath;
        return;
    }

    if (remoteNoDelete && action.actionType == SyncActionType::DeleteRemote) {
        qInfo() << "Skipping remote delete due to sync mode:" << action.localPath;
        return;
    }

    // Queue the action
    if (m_syncActionQueue && action.isValid()) {
        if (!m_syncActionQueue->enqueueIfNotDuplicate(action)) {
            QString skippedPath = change.localPath;
            if (skippedPath.isEmpty()) {
                skippedPath = action.localPath;
            }
            emit changeSkipped(skippedPath, "duplicate pending sync action");
            qInfo() << "Skipped duplicate sync action:" << static_cast<int>(action.actionType) << "for:" << skippedPath;
            return;
        }
        qDebug() << "Queued sync action:" << static_cast<int>(action.actionType) << "for:" << action.localPath;
    }
}

bool ChangeProcessor::buildSyncActionForChange(const ChangeQueueItem& change, SyncActionItem& action) const {
    action = SyncActionItem();
    action.localPath = change.localPath;
    action.fileId = change.fileId;
    action.modifiedTime = change.modifiedTime;
    action.isFolder = change.isDirectory;
    action.localContentHash = change.localContentHash;
    action.remoteMd5 = change.remoteMd5;

    switch (change.changeType) {
        case ChangeType::Create:
            action.actionType =
                change.origin == ChangeOrigin::Local ? SyncActionType::Upload : SyncActionType::Download;
            return true;

        case ChangeType::Modify:
            action.actionType =
                change.origin == ChangeOrigin::Local ? SyncActionType::Upload : SyncActionType::Download;
            return true;

        case ChangeType::Delete:
            action.actionType =
                change.origin == ChangeOrigin::Local ? SyncActionType::DeleteRemote : SyncActionType::DeleteLocal;
            return true;

        case ChangeType::Move:
            if (change.origin == ChangeOrigin::Local) {
                if (action.fileId.isEmpty()) {
                    qWarning() << "Skipping local move action - missing fileId for" << action.localPath;
                    return false;
                }
                const QString destPath = change.moveDestination;
                if (!destPath.isEmpty()) {
                    QString parentPath = QFileInfo(destPath).path();
                    if (parentPath == ".") {
                        parentPath.clear();
                    }
                    action.moveDestination = parentPath;
                }
                action.actionType = SyncActionType::MoveRemote;
            } else {
                action.moveDestination = change.moveDestination;
                action.actionType = SyncActionType::MoveLocal;
            }
            return true;

        case ChangeType::Rename:
            action.renameTo = change.renameTo;
            if (change.origin == ChangeOrigin::Local) {
                if (action.fileId.isEmpty()) {
                    qWarning() << "Skipping local rename action - missing fileId for" << action.localPath;
                    return false;
                }
                action.actionType = SyncActionType::RenameRemote;
            } else {
                action.actionType = SyncActionType::RenameLocal;
            }
            return true;
    }

    return false;
}

QString ChangeProcessor::generateConflictCopyPath(const QString& originalPath) const {
    QFileInfo info(originalPath);
    QString baseName = info.completeBaseName();
    QString suffix = info.suffix();
    QString dirPath = info.path();

    // Add conflict marker with UTC timestamp indicating this is the LOCAL version
    // Using UTC ensures consistent naming across different timezones
    QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd_HH-mm-ss");
    QString conflictName;

    if (suffix.isEmpty()) {
        conflictName = QString("%1/%2 (local conflict %3)").arg(dirPath, baseName, timestamp);
    } else {
        conflictName = QString("%1/%2 (local conflict %3).%4").arg(dirPath, baseName, timestamp, suffix);
    }

    // Ensure unique name - use absolute path for existence check since
    // originalPath is relative to sync root
    int counter = 1;
    while (QFile::exists(QDir(m_syncFolder).filePath(conflictName))) {
        if (suffix.isEmpty()) {
            conflictName = QString("%1/%2 (local conflict %3 %4)").arg(dirPath, baseName, timestamp).arg(counter);
        } else {
            conflictName =
                QString("%1/%2 (local conflict %3 %4).%5").arg(dirPath, baseName, timestamp).arg(counter).arg(suffix);
        }
        ++counter;
    }

    return conflictName;
}
