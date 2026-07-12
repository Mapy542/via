/**
 * @file FuseReplayWorker.cpp
 * @brief Durable replay worker for local-first FUSE journal operations.
 */

#include "FuseReplayWorker.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QtGlobal>
#include <functional>

#include "api/GoogleDriveClient.h"

namespace {}  // namespace

FuseReplayWorker::FuseReplayWorker(SyncDatabase* database, GoogleDriveClient* driveClient,
                                   QObject* parent)
    : QObject(parent),
      m_database(database),
      m_driveClient(driveClient),
      m_syncTimer(new QTimer(this)),
      m_syncSettings(SyncSettings::load()) {
    m_syncTimer->setInterval(m_syncIntervalMs);
    connect(m_syncTimer, &QTimer::timeout, this, &FuseReplayWorker::onSyncTimerTimeout);

    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::fileUploadedDetailed, this,
                &FuseReplayWorker::onFileUploadedDetailed, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::fileUpdated, this,
                &FuseReplayWorker::onFileUpdated, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::folderCreatedDetailed, this,
                &FuseReplayWorker::onFolderCreatedDetailed, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::fileMovedDetailed, this,
                &FuseReplayWorker::onFileMovedDetailed, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::fileMovedAndRenamedDetailed, this,
                &FuseReplayWorker::onFileMovedAndRenamedDetailed, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::fileRenamedDetailed, this,
                &FuseReplayWorker::onFileRenamedDetailed, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::fileDeleted, this,
                &FuseReplayWorker::onFileDeleted, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::fileTrashed, this,
                &FuseReplayWorker::onFileTrashed, Qt::DirectConnection);
        connect(m_driveClient, &GoogleDriveClient::errorDetailed, this,
                &FuseReplayWorker::onDriveErrorDetailed, Qt::DirectConnection);
    }
}

FuseReplayWorker::~FuseReplayWorker() {
    stop();
}

int FuseReplayWorker::syncIntervalMs() const {
    QMutexLocker locker(&m_mutex);
    return m_syncIntervalMs;
}

void FuseReplayWorker::setSyncIntervalMs(int ms) {
    QMutexLocker locker(&m_mutex);
    m_syncIntervalMs = qMax(ms, 250);
    m_syncTimer->setInterval(m_syncIntervalMs);
}

FuseReplayWorkerState FuseReplayWorker::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

void FuseReplayWorker::start() {
    QMutexLocker locker(&m_mutex);
    if (m_state == FuseReplayWorkerState::Running) {
        return;
    }
    m_syncTimer->start();
    setState(FuseReplayWorkerState::Running);
    locker.unlock();
    syncNow();
}

void FuseReplayWorker::stop() {
    QMutexLocker locker(&m_mutex);
    m_syncTimer->stop();
    m_remoteInFlight = false;
    m_remoteDone = true;
    m_remoteSuccess = false;
    m_remoteCondition.wakeAll();
    setState(FuseReplayWorkerState::Stopped);
}

void FuseReplayWorker::pause() {
    QMutexLocker locker(&m_mutex);
    if (m_state != FuseReplayWorkerState::Running) {
        return;
    }
    m_syncTimer->stop();
    setState(FuseReplayWorkerState::Paused);
}

void FuseReplayWorker::resume() {
    QMutexLocker locker(&m_mutex);
    if (m_state != FuseReplayWorkerState::Paused) {
        return;
    }
    m_syncTimer->start();
    setState(FuseReplayWorkerState::Running);
    locker.unlock();
    syncNow();
}

void FuseReplayWorker::syncNow() {
    QMutexLocker locker(&m_mutex);
    if (m_state != FuseReplayWorkerState::Running) {
        return;
    }
    if (m_remoteInFlight) {
        return;
    }
    locker.unlock();
    processPendingEntries();
}

void FuseReplayWorker::reloadSettings() {
    m_syncSettings = SyncSettings::load();
}

void FuseReplayWorker::onSyncTimerTimeout() {
    syncNow();
}

void FuseReplayWorker::onFileUploadedDetailed(const DriveFile& file, const QString& localPath) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("uploadFile")) ||
        m_currentLocalPath != localPath) {
        return;
    }
    m_lastCompletedFile = file;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onFileUpdated(const DriveFile& file) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("updateFile")) ||
        m_currentRemoteFileId != file.id) {
        return;
    }
    m_lastCompletedFile = file;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onFolderCreatedDetailed(const DriveFile& folder, const QString& localPath) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("createFolder")) ||
        m_currentLocalPath != localPath) {
        return;
    }
    m_lastCompletedFile = folder;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onFileMovedDetailed(const DriveFile& file) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("moveFile")) ||
        m_currentRemoteFileId != file.id) {
        return;
    }
    m_lastCompletedFile = file;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onFileMovedAndRenamedDetailed(const DriveFile& file) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("moveAndRenameFile")) ||
        m_currentRemoteFileId != file.id) {
        return;
    }
    m_lastCompletedFile = file;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onFileRenamedDetailed(const DriveFile& file) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("renameFile")) ||
        m_currentRemoteFileId != file.id) {
        return;
    }
    m_lastCompletedFile = file;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onFileDeleted(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("deleteFile")) ||
        m_currentRemoteFileId != fileId) {
        return;
    }
    m_lastCompletedFile = DriveFile{};
    m_lastCompletedFile.id = fileId;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onFileTrashed(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !m_currentOperation.startsWith(QStringLiteral("trashFile")) ||
        m_currentRemoteFileId != fileId) {
        return;
    }
    m_lastCompletedFile = DriveFile{};
    m_lastCompletedFile.id = fileId;
    m_lastError.clear();
    m_lastHttpStatus = 0;
    m_remoteSuccess = true;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::onDriveErrorDetailed(const QString& operation, const QString& errorMsg,
                                            int httpStatus, const QString& fileId,
                                            const QString& localPath) {
    QMutexLocker locker(&m_mutex);
    if (!m_remoteInFlight || !operation.startsWith(m_currentOperation)) {
        return;
    }
    if (!m_currentRemoteFileId.isEmpty() && !fileId.isEmpty() && m_currentRemoteFileId != fileId) {
        return;
    }
    if (!m_currentLocalPath.isEmpty() && !localPath.isEmpty() && m_currentLocalPath != localPath) {
        return;
    }
    m_lastError = errorMsg;
    m_lastHttpStatus = httpStatus;
    m_remoteSuccess = false;
    m_remoteDone = true;
    m_remoteCondition.wakeAll();
}

void FuseReplayWorker::processPendingEntries() {
    if (!m_database || !m_driveClient) {
        return;
    }

    const QList<FuseJournalEntry> allEntries = m_database->getAllFuseJournalEntries();
    QHash<qint64, FuseJournalEntry> entriesById;
    for (const FuseJournalEntry& entry : allEntries) {
        entriesById.insert(entry.entryId, entry);
    }

    const QList<FuseJournalEntry> pendingEntries = m_database->getPendingFuseJournalEntries();
    if (pendingEntries.isEmpty()) {
        emit replayCycleCompleted(0, 0);
        return;
    }

    {
        QMutexLocker locker(&m_mutex);
        if (m_state != FuseReplayWorkerState::Running) {
            return;
        }
        setState(FuseReplayWorkerState::Replaying);
    }

    int completedCount = 0;
    int deferredOrFailedCount = 0;

    for (const FuseJournalEntry& entry : pendingEntries) {
        {
            QMutexLocker locker(&m_mutex);
            if (m_state == FuseReplayWorkerState::Paused ||
                m_state == FuseReplayWorkerState::Stopped) {
                break;
            }
        }

        if (entry.dependencyEntryId > 0) {
            const FuseJournalEntry dependency = entriesById.value(entry.dependencyEntryId);
            if (dependency.entryId <= 0 || dependency.status != FuseJournalEntryStatus::Completed) {
                continue;
            }
        }

        const std::optional<RemoteMutationType> remoteMutation =
            remoteMutationTypeForEntry(entry.operationType);
        if (remoteMutation.has_value() && !m_syncSettings.allowsRemoteMutation(*remoteMutation)) {
            continue;
        }

        switch (processEntry(entry)) {
            case EntryProcessResult::Completed:
                completedCount++;
                break;
            case EntryProcessResult::Failed:
                deferredOrFailedCount++;
                break;
            case EntryProcessResult::Deferred:
                break;
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        if (m_state == FuseReplayWorkerState::Replaying) {
            setState(FuseReplayWorkerState::Running);
        }
    }

    emit replayCycleCompleted(completedCount, deferredOrFailedCount);
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::processEntry(const FuseJournalEntry& entry) {
    const FuseNode node = findNodeForEntry(entry);

    switch (entry.operationType) {
        case FuseJournalOperationType::CreateFile:
            return replayCreateFile(entry, node);
        case FuseJournalOperationType::CreateDirectory:
            return replayCreateDirectory(entry, node);
        case FuseJournalOperationType::WriteGeneration:
        case FuseJournalOperationType::Truncate:
            return replayWriteOrTruncate(entry, node);
        case FuseJournalOperationType::Rename:
        case FuseJournalOperationType::Move:
            return replayRenameOrMove(entry, node);
        case FuseJournalOperationType::Trash:
        case FuseJournalOperationType::Delete:
            return replayTrashOrDelete(entry, node);
        case FuseJournalOperationType::Restore:
            return handleConflict(entry, QStringLiteral("Restore replay is not implemented yet"),
                                  0);
        case FuseJournalOperationType::UpdateNativeDocMetadata:
        case FuseJournalOperationType::UpdateShortcutMetadata:
            return completeLocalOnlyEntry(entry, node,
                                          QStringLiteral("Metadata-only local replay"));
    }

    return EntryProcessResult::Deferred;
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::replayCreateFile(
    const FuseJournalEntry& entry, FuseNode node) {
    if (node.nodeId.isEmpty()) {
        return completeLocalOnlyEntry(entry, node, QStringLiteral("Node removed before replay"));
    }
    if (!node.remoteFileId.isEmpty()) {
        return completeLocalOnlyEntry(entry, node,
                                      QStringLiteral("Remote file ID already assigned"));
    }

    QString parentRemoteId;
    if (!resolveRemoteParentId(node.parentNodeId, entry.remoteParentId, &parentRemoteId)) {
        return EntryProcessResult::Deferred;
    }

    FuseNodeContentState contentState = m_database->getFuseNodeContentState(node.nodeId);
    if (contentState.nodeId.isEmpty() || contentState.localContentPath.isEmpty() ||
        !QFileInfo::exists(contentState.localContentPath)) {
        m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Failed,
                                                 QStringLiteral("Local create content missing"),
                                                 entry.retryCount + 1);
        emit replayEntryBlocked(entry.entryId, QStringLiteral("Local create content missing"));
        return EntryProcessResult::Failed;
    }

    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::InFlight);

    DriveFile createdFile;
    QString error;
    int httpStatus = 0;
    const QString localPath = contentState.localContentPath;
    const QString uploadName = node.name;
    if (!waitForRemoteCompletion(
            QStringLiteral("uploadFile"), QString(), localPath,
            [this, localPath, parentRemoteId, uploadName]() {
                QMetaObject::invokeMethod(m_driveClient, "uploadFile", Qt::QueuedConnection,
                                          Q_ARG(QString, localPath), Q_ARG(QString, parentRemoteId),
                                          Q_ARG(QString, uploadName));
            },
            &createdFile, &error, &httpStatus)) {
        if (isTransientReplayError(error, httpStatus)) {
            m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Pending,
                                                     error, entry.retryCount + 1);
            return EntryProcessResult::Deferred;
        }
        if (isConflictError(error, httpStatus) || isNotFoundError(error, httpStatus)) {
            return handleConflict(entry, error, httpStatus);
        }

        m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Failed,
                                                 error, entry.retryCount + 1);
        emit replayEntryBlocked(entry.entryId, error);
        return EntryProcessResult::Failed;
    }

    node.remoteFileId = createdFile.id;
    node.remoteParentId = parentRemoteId;
    node.remoteName = createdFile.name.isEmpty() ? node.name : createdFile.name;
    node.isPendingCreate = false;
    node.size = createdFile.size > 0 ? createdFile.size : contentState.size;
    node.lastSyncedAt = QDateTime::currentDateTimeUtc();
    if (createdFile.modifiedTime.isValid()) {
        node.modifiedTime = createdFile.modifiedTime;
    }
    m_database->saveFuseNode(node);

    contentState.remoteAckGeneration = contentState.localGeneration;
    if (createdFile.size > 0) {
        contentState.size = createdFile.size;
    }
    m_database->saveFuseNodeContentState(contentState);

    FuseMetadata metadata = fuseMetadataFromNode(node);
    metadata.fileId = createdFile.id;
    metadata.parentId = parentRemoteId;
    metadata.mimeType = createdFile.mimeType.isEmpty() ? node.mimeType : createdFile.mimeType;
    metadata.size = node.size;
    metadata.createdTime =
        createdFile.createdTime.isValid() ? createdFile.createdTime : node.createdTime;
    metadata.modifiedTime = node.modifiedTime;
    metadata.cachedAt = QDateTime::currentDateTimeUtc();
    metadata.lastAccessed = QDateTime::currentDateTimeUtc();
    m_database->saveFuseMetadata(metadata);

    FuseOperationAck ack;
    ack.journalEntryId = entry.entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = node.nodeId;
    ack.remoteFileId = createdFile.id;
    ack.remoteParentId = parentRemoteId;
    ack.acknowledgedGeneration = contentState.remoteAckGeneration;
    ack.payloadJson = QStringLiteral("{\"operation\":\"create-file\"}");
    ack.acknowledgedAt = QDateTime::currentDateTimeUtc();
    m_database->saveFuseOperationAck(ack);
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Completed,
                                             QString(), -1, ack.acknowledgedAt);
    emit replayEntryCompleted(entry.entryId);
    return EntryProcessResult::Completed;
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::replayCreateDirectory(
    const FuseJournalEntry& entry, FuseNode node) {
    if (node.nodeId.isEmpty()) {
        return completeLocalOnlyEntry(entry, node, QStringLiteral("Node removed before replay"));
    }
    if (!node.remoteFileId.isEmpty()) {
        return completeLocalOnlyEntry(entry, node,
                                      QStringLiteral("Remote directory ID already assigned"));
    }

    QString parentRemoteId;
    if (!resolveRemoteParentId(node.parentNodeId, entry.remoteParentId, &parentRemoteId)) {
        return EntryProcessResult::Deferred;
    }

    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::InFlight);

    DriveFile createdFolder;
    QString error;
    int httpStatus = 0;
    const QString localPath = relativeFusePath(node.path);
    const QString folderName = node.name;
    if (!waitForRemoteCompletion(
            QStringLiteral("createFolder"), QString(), localPath,
            [this, folderName, parentRemoteId, localPath]() {
                QMetaObject::invokeMethod(
                    m_driveClient, "createFolder", Qt::QueuedConnection, Q_ARG(QString, folderName),
                    Q_ARG(QString, parentRemoteId), Q_ARG(QString, localPath));
            },
            &createdFolder, &error, &httpStatus)) {
        if (isTransientReplayError(error, httpStatus)) {
            m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Pending,
                                                     error, entry.retryCount + 1);
            return EntryProcessResult::Deferred;
        }
        if (isConflictError(error, httpStatus) || isNotFoundError(error, httpStatus)) {
            return handleConflict(entry, error, httpStatus);
        }

        m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Failed,
                                                 error, entry.retryCount + 1);
        emit replayEntryBlocked(entry.entryId, error);
        return EntryProcessResult::Failed;
    }

    node.remoteFileId = createdFolder.id;
    node.remoteParentId = parentRemoteId;
    node.remoteName = createdFolder.name.isEmpty() ? node.name : createdFolder.name;
    node.isPendingCreate = false;
    node.lastSyncedAt = QDateTime::currentDateTimeUtc();
    if (createdFolder.modifiedTime.isValid()) {
        node.modifiedTime = createdFolder.modifiedTime;
    }
    m_database->saveFuseNode(node);

    FuseMetadata metadata = fuseMetadataFromNode(node);
    metadata.fileId = createdFolder.id;
    metadata.parentId = parentRemoteId;
    metadata.mimeType = createdFolder.mimeType;
    metadata.createdTime =
        createdFolder.createdTime.isValid() ? createdFolder.createdTime : node.createdTime;
    metadata.modifiedTime = node.modifiedTime;
    metadata.cachedAt = QDateTime::currentDateTimeUtc();
    metadata.lastAccessed = QDateTime::currentDateTimeUtc();
    m_database->saveFuseMetadata(metadata);

    FuseOperationAck ack;
    ack.journalEntryId = entry.entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = node.nodeId;
    ack.remoteFileId = createdFolder.id;
    ack.remoteParentId = parentRemoteId;
    ack.payloadJson = QStringLiteral("{\"operation\":\"create-directory\"}");
    ack.acknowledgedAt = QDateTime::currentDateTimeUtc();
    m_database->saveFuseOperationAck(ack);
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Completed,
                                             QString(), -1, ack.acknowledgedAt);
    emit replayEntryCompleted(entry.entryId);
    return EntryProcessResult::Completed;
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::replayWriteOrTruncate(
    const FuseJournalEntry& entry, const FuseNode& node) {
    if (node.nodeId.isEmpty()) {
        return completeLocalOnlyEntry(entry, node, QStringLiteral("Node removed before replay"));
    }

    FuseNodeContentState state = m_database->getFuseNodeContentState(node.nodeId);
    if (!node.remoteFileId.isEmpty() && state.remoteAckGeneration >= entry.localGeneration) {
        return completeLocalOnlyEntry(entry, node,
                                      QStringLiteral("Generation already acknowledged"));
    }

    if (node.remoteFileId.isEmpty()) {
        return EntryProcessResult::Deferred;
    }

    if (!entry.remoteFileId.isEmpty()) {
        // Remote-backed content uploads created after a file already has a Drive ID
        // remain owned by DirtySyncWorker. Once it advances remoteAckGeneration this
        // journal entry becomes a local-only acknowledgement.
        return EntryProcessResult::Deferred;
    }

    if (state.nodeId.isEmpty() || state.localContentPath.isEmpty() ||
        !QFileInfo::exists(state.localContentPath)) {
        m_database->updateFuseJournalEntryStatus(
            entry.entryId, FuseJournalEntryStatus::Failed,
            QStringLiteral("Local content missing for replayed write"), entry.retryCount + 1);
        return EntryProcessResult::Failed;
    }

    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::InFlight);

    DriveFile updatedFile;
    QString error;
    int httpStatus = 0;
    if (!waitForRemoteCompletion(
            QStringLiteral("updateFile"), node.remoteFileId, state.localContentPath,
            [this, node, state]() {
                QMetaObject::invokeMethod(m_driveClient, "updateFile", Qt::QueuedConnection,
                                          Q_ARG(QString, node.remoteFileId),
                                          Q_ARG(QString, state.localContentPath));
            },
            &updatedFile, &error, &httpStatus)) {
        if (isTransientReplayError(error, httpStatus)) {
            m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Pending,
                                                     error, entry.retryCount + 1);
            return EntryProcessResult::Deferred;
        }
        if (isConflictError(error, httpStatus) || isNotFoundError(error, httpStatus)) {
            return handleConflict(entry, error, httpStatus);
        }

        m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Failed,
                                                 error, entry.retryCount + 1);
        return EntryProcessResult::Failed;
    }

    state.remoteAckGeneration = qMax(state.remoteAckGeneration, entry.localGeneration);
    if (updatedFile.size > 0) {
        state.size = updatedFile.size;
    }
    m_database->saveFuseNodeContentState(state);

    FuseNode updatedNode = node;
    updatedNode.size = state.size;
    updatedNode.lastSyncedAt = QDateTime::currentDateTimeUtc();
    if (updatedFile.modifiedTime.isValid()) {
        updatedNode.modifiedTime = updatedFile.modifiedTime;
    }
    m_database->saveFuseNode(updatedNode);

    FuseMetadata metadata = m_database->getFuseMetadata(node.remoteFileId);
    if (!metadata.fileId.isEmpty()) {
        metadata.size = state.size;
        if (updatedFile.modifiedTime.isValid()) {
            metadata.modifiedTime = updatedFile.modifiedTime;
        }
        metadata.cachedAt = QDateTime::currentDateTimeUtc();
        metadata.lastAccessed = QDateTime::currentDateTimeUtc();
        m_database->saveFuseMetadata(metadata);
    }

    FuseOperationAck ack;
    ack.journalEntryId = entry.entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = node.nodeId;
    ack.remoteFileId = node.remoteFileId;
    ack.remoteParentId = node.remoteParentId;
    ack.acknowledgedGeneration = state.remoteAckGeneration;
    ack.payloadJson = QStringLiteral("{\"operation\":\"write-after-create\"}");
    ack.acknowledgedAt = QDateTime::currentDateTimeUtc();
    m_database->saveFuseOperationAck(ack);
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Completed,
                                             QString(), -1, ack.acknowledgedAt);
    emit replayEntryCompleted(entry.entryId);
    return EntryProcessResult::Completed;
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::replayRenameOrMove(
    const FuseJournalEntry& entry, FuseNode node) {
    const QString remoteFileId =
        !node.remoteFileId.isEmpty() ? node.remoteFileId : entry.remoteFileId;
    if (remoteFileId.isEmpty()) {
        return completeLocalOnlyEntry(
            entry, node,
            QStringLiteral("Provisional namespace update requires no remote call yet"));
    }

    const QString desiredPath = !node.nodeId.isEmpty() ? node.path : entry.destinationPath;
    const QString desiredName = !node.nodeId.isEmpty() ? node.name : entry.destinationVisibleName;
    QString desiredParentRemoteId;
    if (!resolveRemoteParentId(
            !node.nodeId.isEmpty() ? node.parentNodeId : entry.destinationParentNodeId,
            entry.remoteParentId, &desiredParentRemoteId)) {
        return EntryProcessResult::Deferred;
    }

    const QString oldParentRemoteId =
        !entry.remoteParentId.isEmpty()
            ? entry.remoteParentId
            : (!node.remoteParentId.isEmpty() ? node.remoteParentId : QStringLiteral("root"));
    const QString oldName = !entry.visibleName.isEmpty()
                                ? entry.visibleName
                                : (!node.remoteName.isEmpty() ? node.remoteName : desiredName);
    const bool parentChanged = desiredParentRemoteId != oldParentRemoteId;
    const bool nameChanged = desiredName != oldName;

    if (!parentChanged && !nameChanged) {
        return completeLocalOnlyEntry(entry, node,
                                      QStringLiteral("Remote namespace already aligned"));
    }

    const QString operation =
        parentChanged && nameChanged
            ? QStringLiteral("moveAndRenameFile")
            : (parentChanged ? QStringLiteral("moveFile") : QStringLiteral("renameFile"));
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::InFlight);

    DriveFile updatedFile;
    QString error;
    int httpStatus = 0;
    if (!waitForRemoteCompletion(
            operation, remoteFileId, QString(),
            [this, operation, remoteFileId, desiredParentRemoteId, oldParentRemoteId,
             desiredName]() {
                if (operation == QStringLiteral("moveAndRenameFile")) {
                    QMetaObject::invokeMethod(
                        m_driveClient, "moveAndRenameFile", Qt::QueuedConnection,
                        Q_ARG(QString, remoteFileId), Q_ARG(QString, desiredParentRemoteId),
                        Q_ARG(QString, oldParentRemoteId), Q_ARG(QString, desiredName));
                } else if (operation == QStringLiteral("moveFile")) {
                    QMetaObject::invokeMethod(m_driveClient, "moveFile", Qt::QueuedConnection,
                                              Q_ARG(QString, remoteFileId),
                                              Q_ARG(QString, desiredParentRemoteId),
                                              Q_ARG(QString, oldParentRemoteId));
                } else {
                    QMetaObject::invokeMethod(m_driveClient, "renameFile", Qt::QueuedConnection,
                                              Q_ARG(QString, remoteFileId),
                                              Q_ARG(QString, desiredName));
                }
            },
            &updatedFile, &error, &httpStatus)) {
        if (isTransientReplayError(error, httpStatus)) {
            m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Pending,
                                                     error, entry.retryCount + 1);
            return EntryProcessResult::Deferred;
        }
        if (isConflictError(error, httpStatus) || isNotFoundError(error, httpStatus)) {
            return handleConflict(entry, error, httpStatus);
        }

        m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Failed,
                                                 error, entry.retryCount + 1);
        emit replayEntryBlocked(entry.entryId, error);
        return EntryProcessResult::Failed;
    }

    if (!node.nodeId.isEmpty()) {
        node.remoteFileId = remoteFileId;
        node.remoteParentId = desiredParentRemoteId;
        node.remoteName = desiredName;
        node.lastSyncedAt = QDateTime::currentDateTimeUtc();
        if (updatedFile.modifiedTime.isValid()) {
            node.modifiedTime = updatedFile.modifiedTime;
        }
        m_database->saveFuseNode(node);

        FuseMetadata metadata = m_database->getFuseMetadata(remoteFileId);
        if (!metadata.fileId.isEmpty()) {
            metadata.path = relativeFusePath(desiredPath);
            metadata.name = desiredName;
            metadata.remoteName = desiredName;
            metadata.parentId = desiredParentRemoteId;
            if (updatedFile.modifiedTime.isValid()) {
                metadata.modifiedTime = updatedFile.modifiedTime;
            }
            metadata.cachedAt = QDateTime::currentDateTimeUtc();
            metadata.lastAccessed = QDateTime::currentDateTimeUtc();
            m_database->saveFuseMetadata(metadata);
            if (metadata.isFolder) {
                m_database->updateFuseChildrenPaths(remoteFileId, relativeFusePath(entry.path),
                                                    metadata.path);
            }
        }
    }

    FuseOperationAck ack;
    ack.journalEntryId = entry.entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = node.nodeId;
    ack.remoteFileId = remoteFileId;
    ack.remoteParentId = desiredParentRemoteId;
    ack.payloadJson = QStringLiteral("{\"operation\":\"namespace\"}");
    ack.acknowledgedAt = QDateTime::currentDateTimeUtc();
    m_database->saveFuseOperationAck(ack);
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Completed,
                                             QString(), -1, ack.acknowledgedAt);
    emit replayEntryCompleted(entry.entryId);
    return EntryProcessResult::Completed;
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::replayTrashOrDelete(
    const FuseJournalEntry& entry, const FuseNode& node) {
    const QString remoteFileId =
        !node.remoteFileId.isEmpty() ? node.remoteFileId : entry.remoteFileId;
    if (remoteFileId.isEmpty()) {
        return completeLocalOnlyEntry(entry, node,
                                      QStringLiteral("Removed before remote creation"));
    }

    const QString operation = entry.operationType == FuseJournalOperationType::Delete
                                  ? QStringLiteral("deleteFile")
                                  : QStringLiteral("trashFile");
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::InFlight);

    DriveFile completedFile;
    QString error;
    int httpStatus = 0;
    if (!waitForRemoteCompletion(
            operation, remoteFileId, QString(),
            [this, operation, remoteFileId]() {
                if (operation == QStringLiteral("deleteFile")) {
                    QMetaObject::invokeMethod(m_driveClient, "deleteFile", Qt::QueuedConnection,
                                              Q_ARG(QString, remoteFileId));
                } else {
                    QMetaObject::invokeMethod(m_driveClient, "trashFile", Qt::QueuedConnection,
                                              Q_ARG(QString, remoteFileId));
                }
            },
            &completedFile, &error, &httpStatus)) {
        if (isTransientReplayError(error, httpStatus)) {
            m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Pending,
                                                     error, entry.retryCount + 1);
            return EntryProcessResult::Deferred;
        }
        if (isConflictError(error, httpStatus) || isNotFoundError(error, httpStatus)) {
            return handleConflict(entry, error, httpStatus);
        }

        m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Failed,
                                                 error, entry.retryCount + 1);
        emit replayEntryBlocked(entry.entryId, error);
        return EntryProcessResult::Failed;
    }

    FuseOperationAck ack;
    ack.journalEntryId = entry.entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = node.nodeId;
    ack.remoteFileId = remoteFileId;
    ack.payloadJson = QStringLiteral("{\"operation\":\"remove\"}");
    ack.acknowledgedAt = QDateTime::currentDateTimeUtc();
    m_database->saveFuseOperationAck(ack);
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Completed,
                                             QString(), -1, ack.acknowledgedAt);
    emit replayEntryCompleted(entry.entryId);
    return EntryProcessResult::Completed;
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::completeLocalOnlyEntry(
    const FuseJournalEntry& entry, const FuseNode& node, const QString& reason) {
    const QDateTime acknowledgedAt = QDateTime::currentDateTimeUtc();
    FuseOperationAck ack;
    ack.journalEntryId = entry.entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = node.nodeId;
    ack.remoteFileId = node.remoteFileId;
    ack.remoteParentId = node.remoteParentId;
    ack.payloadJson = reason;
    ack.acknowledgedAt = acknowledgedAt;
    m_database->saveFuseOperationAck(ack);
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::Completed,
                                             reason, -1, acknowledgedAt);
    emit replayEntryCompleted(entry.entryId);
    return EntryProcessResult::Completed;
}

FuseReplayWorker::EntryProcessResult FuseReplayWorker::handleConflict(const FuseJournalEntry& entry,
                                                                      const QString& error,
                                                                      int httpStatus) {
    const QString strategy = m_syncSettings.conflictStrategy;

    if (isNotFoundError(error, httpStatus)) {
        return completeLocalOnlyEntry(entry, findNodeForEntry(entry), error);
    }

    if (strategy == QStringLiteral("keep-remote")) {
        return completeLocalOnlyEntry(entry, findNodeForEntry(entry), error);
    }

    FuseOperationAck ack;
    ack.journalEntryId = entry.entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = entry.nodeId;
    ack.remoteFileId = entry.remoteFileId;
    ack.remoteParentId = entry.remoteParentId;
    ack.payloadJson = QStringLiteral("{\"conflictStrategy\":\"%1\"}").arg(strategy);
    ack.lastError = error;
    ack.acknowledgedAt = QDateTime::currentDateTimeUtc();
    m_database->saveFuseOperationAck(ack);
    m_database->updateFuseJournalEntryStatus(entry.entryId, FuseJournalEntryStatus::BlockedConflict,
                                             error, entry.retryCount + 1);
    emit replayEntryBlocked(entry.entryId, error);
    return EntryProcessResult::Failed;
}

bool FuseReplayWorker::waitForRemoteCompletion(const QString& operation,
                                               const QString& remoteFileId,
                                               const QString& localPath,
                                               const std::function<void()>& startCall,
                                               DriveFile* completedFile, QString* errorOut,
                                               int* httpStatusOut) {
    {
        QMutexLocker locker(&m_mutex);
        m_remoteInFlight = true;
        m_remoteDone = false;
        m_remoteSuccess = false;
        m_currentOperation = operation;
        m_currentRemoteFileId = remoteFileId;
        m_currentLocalPath = localPath;
        m_lastCompletedFile = DriveFile{};
        m_lastError.clear();
        m_lastHttpStatus = 0;
    }

    startCall();

    QMutexLocker locker(&m_mutex);
    QDeadlineTimer deadline(m_remoteTimeoutMs);
    while (!m_remoteDone) {
        if (!m_remoteCondition.wait(&m_mutex, deadline)) {
            break;
        }
    }

    const bool finished = m_remoteDone;
    const bool success = finished && m_remoteSuccess;
    const DriveFile resultFile = m_lastCompletedFile;
    const QString error =
        finished ? m_lastError : QStringLiteral("Remote replay timed out after 30 seconds");
    const int httpStatus = m_lastHttpStatus;

    m_remoteInFlight = false;
    m_remoteDone = false;
    m_remoteSuccess = false;
    m_currentOperation.clear();
    m_currentRemoteFileId.clear();
    m_currentLocalPath.clear();
    locker.unlock();

    if (completedFile) {
        *completedFile = resultFile;
    }
    if (errorOut) {
        *errorOut = error;
    }
    if (httpStatusOut) {
        *httpStatusOut = httpStatus;
    }

    return success;
}

bool FuseReplayWorker::resolveRemoteParentId(const QString& parentNodeId,
                                             const QString& fallbackRemoteParentId,
                                             QString* remoteParentIdOut) const {
    if (!remoteParentIdOut) {
        return false;
    }

    if (!parentNodeId.isEmpty()) {
        const FuseNode parentNode = m_database->getFuseNode(parentNodeId);
        if (parentNode.nodeId.isEmpty() || parentNode.remoteFileId.isEmpty()) {
            return false;
        }
        *remoteParentIdOut = parentNode.remoteFileId;
        return true;
    }

    *remoteParentIdOut =
        fallbackRemoteParentId.isEmpty() ? QStringLiteral("root") : fallbackRemoteParentId;
    return true;
}

FuseNode FuseReplayWorker::findNodeForEntry(const FuseJournalEntry& entry) const {
    if (entry.nodeId.isEmpty() || !m_database) {
        return {};
    }

    FuseNode node = m_database->getFuseNode(entry.nodeId);
    if (!node.nodeId.isEmpty()) {
        return node;
    }

    if (!entry.remoteFileId.isEmpty()) {
        const QList<FuseNode> nodes = m_database->getAllFuseNodes();
        for (const FuseNode& candidate : nodes) {
            if (candidate.remoteFileId == entry.remoteFileId) {
                return candidate;
            }
        }
    }

    return {};
}

std::optional<RemoteMutationType> FuseReplayWorker::remoteMutationTypeForEntry(
    FuseJournalOperationType operationType) {
    switch (operationType) {
        case FuseJournalOperationType::CreateFile:
            return RemoteMutationType::CreateFile;
        case FuseJournalOperationType::CreateDirectory:
            return RemoteMutationType::CreateFolder;
        case FuseJournalOperationType::Rename:
            return RemoteMutationType::Rename;
        case FuseJournalOperationType::Move:
            return RemoteMutationType::Move;
        case FuseJournalOperationType::Trash:
            return RemoteMutationType::Trash;
        case FuseJournalOperationType::Delete:
            return RemoteMutationType::Delete;
        default:
            return std::nullopt;
    }
}

QString FuseReplayWorker::relativeFusePath(const QString& fusePath) {
    QString normalized = QDir::cleanPath(fusePath);
    if (normalized == QStringLiteral(".")) {
        normalized.clear();
    }
    if (normalized == QStringLiteral("/")) {
        return QString();
    }
    if (normalized.startsWith(QLatin1Char('/'))) {
        normalized.remove(0, 1);
    }
    return normalized;
}

bool FuseReplayWorker::isNotFoundError(const QString& error, int httpStatus) {
    return httpStatus == 404 || error.contains(QStringLiteral("not found"), Qt::CaseInsensitive) ||
           error.contains(QStringLiteral("404"));
}

bool FuseReplayWorker::isConflictError(const QString& error, int httpStatus) {
    return httpStatus == 409 || error.contains(QStringLiteral("conflict"), Qt::CaseInsensitive) ||
           error.contains(QStringLiteral("already exists"), Qt::CaseInsensitive);
}

bool FuseReplayWorker::isTransientReplayError(const QString& error, int httpStatus) {
    const QString lowered = error.toLower();
    return httpStatus == 0 || lowered.contains(QStringLiteral("timed out")) ||
           lowered.contains(QStringLiteral("timeout")) ||
           lowered.contains(QStringLiteral("connection")) ||
           lowered.contains(QStringLiteral("temporar")) ||
           lowered.contains(QStringLiteral("reset"));
}

FuseMetadata FuseReplayWorker::fuseMetadataFromNode(const FuseNode& node) {
    FuseMetadata metadata;
    metadata.fileId = node.remoteFileId;
    metadata.path = relativeFusePath(node.path);
    metadata.name = node.name;
    metadata.remoteName = node.remoteName.isEmpty() ? node.name : node.remoteName;
    metadata.nativeDocModeOverride = node.nativeDocModeOverride;
    metadata.parentId = node.remoteParentId;
    metadata.isFolder = node.isFolder;
    metadata.size = node.size;
    metadata.mimeType = node.mimeType;
    metadata.remoteMimeType = node.remoteMimeType;
    metadata.webViewLink = node.webViewLink;
    metadata.createdTime = node.createdTime;
    metadata.modifiedTime = node.modifiedTime;
    metadata.cachedAt =
        node.lastSyncedAt.isValid() ? node.lastSyncedAt : QDateTime::currentDateTimeUtc();
    metadata.lastAccessed = node.lastAccessed;
    return metadata;
}

void FuseReplayWorker::setState(FuseReplayWorkerState newState) {
    if (m_state != newState) {
        m_state = newState;
        emit stateChanged(newState);
    }
}