/**
 * @file MetadataRefreshWorker.cpp
 * @brief Implementation of background worker for FUSE metadata refresh
 *
 * Implements the "Metadata Refresh Thread" from the FUSE procedure flow chart.
 */

#include "MetadataRefreshWorker.h"

#include <QDebug>
#include <QFile>
#include <QMutexLocker>
#include <QSettings>
#include <QtGlobal>

#include "FileCache.h"
#include "MetadataCache.h"
#include "api/DriveChange.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "sync/SyncDatabase.h"
#include "sync/TrashPolicy.h"
#include "utils/NativeDocSupport.h"

namespace {
constexpr int kTransientRetryBaseDelayMs = 300;
constexpr int kTransientRetryMaxDelayMs = 5000;
constexpr int kTransientFailureSurfaceThreshold = 3;

bool isTransientPollError(const QString& error) {
    const QString lowered = error.toLower();
    return lowered.contains(QStringLiteral("goaway")) ||
           lowered.contains(QStringLiteral("protocol error")) ||
           lowered.contains(QStringLiteral("stream error")) ||
           lowered.contains(QStringLiteral("timed out")) ||
           lowered.contains(QStringLiteral("timeout")) ||
           lowered.contains(QStringLiteral("operation canceled")) ||
           lowered.contains(QStringLiteral("operation cancelled")) ||
           lowered.contains(QStringLiteral("connection reset")) ||
           lowered.contains(QStringLiteral("connection closed")) ||
           lowered.contains(QStringLiteral("remote host closed"));
}

bool isExpiredChangeTokenError(const QString& error, int httpStatus) {
    if (httpStatus == 410) {
        return true;
    }

    const QString lowered = error.toLower();
    return ((lowered.contains(QStringLiteral("page token")) ||
             lowered.contains(QStringLiteral("start page token")) ||
             lowered.contains(QStringLiteral("sync token"))) &&
            (lowered.contains(QStringLiteral("expired")) ||
             lowered.contains(QStringLiteral("invalid")) ||
             lowered.contains(QStringLiteral("no longer valid")) ||
             lowered.contains(QStringLiteral("gone"))));
}

int transientRetryDelayMs(int consecutiveFailures) {
    int delayMs = kTransientRetryBaseDelayMs;
    const int clampedFailures = qBound(1, consecutiveFailures, 5);
    for (int attempt = 1; attempt < clampedFailures; ++attempt) {
        delayMs = qMin(delayMs * 2, kTransientRetryMaxDelayMs);
    }
    return delayMs;
}

bool resolvesToTrashRelativePath(const DriveFile& file, const MetadataCache* metadataCache,
                                 const SyncDatabase* database) {
    if (file.name.isEmpty()) {
        return false;
    }

    const QString parentId = file.parentId();
    if (parentId.isEmpty()) {
        return TrashPolicy::isTrashRelativePath(file.name);
    }

    QString parentPath;
    if (metadataCache) {
        const QString rootId = metadataCache->rootFolderId();
        if (parentId == QStringLiteral("root") || (!rootId.isEmpty() && parentId == rootId)) {
            parentPath = QStringLiteral("/");
        } else {
            parentPath = metadataCache->getPathByFileId(parentId);
        }
    }

    if (parentPath.isEmpty() && database) {
        const FuseMetadata parentMeta = database->getFuseMetadata(parentId);
        if (!parentMeta.path.isEmpty()) {
            parentPath = QDir::cleanPath(parentMeta.path);
        }
    }

    if (parentPath.isEmpty()) {
        return TrashPolicy::isTrashRelativePath(file.name);
    }

    const QString candidatePath =
        parentPath == QStringLiteral("/") ? file.name : QDir(parentPath).filePath(file.name);
    return TrashPolicy::isTrashRelativePath(candidatePath);
}

bool pathWithinNodeSubtree(const QString& candidatePath, const QString& rootPath) {
    const QString normalizedCandidate = QDir::cleanPath(candidatePath);
    const QString normalizedRoot = QDir::cleanPath(rootPath);
    if (normalizedCandidate.isEmpty() || normalizedRoot.isEmpty()) {
        return false;
    }
    if (normalizedCandidate == normalizedRoot) {
        return true;
    }
    return normalizedCandidate.startsWith(normalizedRoot + QLatin1Char('/'));
}

struct RemoteNodeSnapshot {
    QList<FuseNode> nodes;
    bool hasPendingNamespaceOp = false;
    bool hasLocalGenerationAhead = false;
};

RemoteNodeSnapshot snapshotRemoteNodes(SyncDatabase* database, const QString& remoteFileId) {
    RemoteNodeSnapshot snapshot;
    if (!database || remoteFileId.isEmpty()) {
        return snapshot;
    }

    const QList<FuseNode> nodes = database->getAllFuseNodes();
    for (const FuseNode& node : nodes) {
        if (node.remoteFileId == remoteFileId) {
            snapshot.nodes.append(node);
            const FuseNodeContentState state = database->getFuseNodeContentState(node.nodeId);
            if (!state.nodeId.isEmpty() && state.localGeneration > state.remoteAckGeneration) {
                snapshot.hasLocalGenerationAhead = true;
            }
        }
    }

    if (snapshot.nodes.isEmpty()) {
        return snapshot;
    }

    const QList<FuseJournalEntry> journalEntries = database->getAllFuseJournalEntries();
    for (const FuseJournalEntry& entry : journalEntries) {
        if (entry.status == FuseJournalEntryStatus::Completed) {
            continue;
        }

        for (const FuseNode& node : snapshot.nodes) {
            if (entry.nodeId != node.nodeId) {
                continue;
            }

            switch (entry.operationType) {
                case FuseJournalOperationType::CreateFile:
                case FuseJournalOperationType::CreateDirectory:
                case FuseJournalOperationType::Rename:
                case FuseJournalOperationType::Move:
                case FuseJournalOperationType::Trash:
                case FuseJournalOperationType::Delete:
                case FuseJournalOperationType::Restore:
                case FuseJournalOperationType::UpdateNativeDocMetadata:
                case FuseJournalOperationType::UpdateShortcutMetadata:
                    snapshot.hasPendingNamespaceOp = true;
                    break;
                case FuseJournalOperationType::WriteGeneration:
                case FuseJournalOperationType::Truncate:
                    break;
            }
        }

        if (snapshot.hasPendingNamespaceOp) {
            break;
        }
    }

    return snapshot;
}

void deleteNodeSubtree(SyncDatabase* database, const QList<FuseNode>& subtreeNodes) {
    if (!database || subtreeNodes.isEmpty()) {
        return;
    }

    for (const FuseNode& node : subtreeNodes) {
        const FuseNodeContentState state = database->getFuseNodeContentState(node.nodeId);
        if (!state.nodeId.isEmpty()) {
            if (!state.localContentPath.isEmpty()) {
                QFile::remove(state.localContentPath);
            }
            database->deleteFuseNodeContentState(node.nodeId);
        }

        if (!node.remoteFileId.isEmpty()) {
            database->deleteNativeDocState(node.remoteFileId);
            database->deleteFuseMetadata(node.remoteFileId);
        }

        database->deleteFuseNode(node.nodeId);
    }
}

void reconcileRemoteNodeMetadata(SyncDatabase* database, const FuseFileMetadata& metadata) {
    if (!database || metadata.fileId.isEmpty() || !metadata.isValid()) {
        return;
    }

    const QList<FuseNode> nodes = database->getAllFuseNodes();
    for (const FuseNode& node : nodes) {
        if (node.remoteFileId != metadata.fileId) {
            continue;
        }

        const QString oldPath = node.path;
        const QString newPath = QStringLiteral("/") + metadata.path;

        FuseNode updated = node;
        updated.path = newPath;
        updated.name = metadata.name;
        updated.remoteName = metadata.remoteName;
        updated.remoteParentId = metadata.parentId;
        updated.mimeType = metadata.mimeType;
        updated.remoteMimeType = metadata.remoteMimeType;
        updated.webViewLink = metadata.webViewLink;
        updated.nativeDocModeOverride = metadata.nativeDocModeOverride;
        updated.size = metadata.size;
        updated.modifiedTime = metadata.modifiedTime;
        updated.lastAccessed = metadata.lastAccessed;
        updated.lastSyncedAt = QDateTime::currentDateTimeUtc();
        database->saveFuseNode(updated);

        const FuseNodeContentState state = database->getFuseNodeContentState(node.nodeId);
        if (!state.nodeId.isEmpty()) {
            FuseNodeContentState updatedState = state;
            if (updatedState.remoteAckGeneration >= updatedState.localGeneration) {
                updatedState.size = metadata.size;
            }
            database->saveFuseNodeContentState(updatedState);
        }

        if (!node.isFolder || oldPath == newPath) {
            continue;
        }

        const QList<FuseNode> allNodes = database->getAllFuseNodes();
        for (const FuseNode& candidate : allNodes) {
            if (candidate.nodeId == node.nodeId ||
                !pathWithinNodeSubtree(candidate.path, oldPath)) {
                continue;
            }

            FuseNode descendant = candidate;
            descendant.path = newPath + candidate.path.mid(oldPath.size());
            database->saveFuseNode(descendant);
        }
    }
}
}  // namespace

// Key used to store the FUSE change token in the fuse_sync_state table
const QString MetadataRefreshWorker::FUSE_CHANGE_TOKEN_KEY = QStringLiteral("fuse_change_token");

MetadataRefreshWorker::MetadataRefreshWorker(MetadataCache* metadataCache, FileCache* fileCache,
                                             SyncDatabase* database, GoogleDriveClient* driveClient,
                                             QObject* parent)
    : QObject(parent),
      m_metadataCache(metadataCache),
      m_fileCache(fileCache),
      m_database(database),
      m_driveClient(driveClient),
      m_pollingTimer(new QTimer(this)),
      m_retryTimer(new QTimer(this)),
      m_state(State::Stopped),
      m_waitingForToken(false) {
    // Configure polling timer with default interval
    m_pollingTimer->setInterval(DEFAULT_POLL_INTERVAL_MS);
    m_retryTimer->setSingleShot(true);

    // Connect timer
    connect(m_pollingTimer, &QTimer::timeout, this, &MetadataRefreshWorker::onPollingTimeout);
    connect(m_retryTimer, &QTimer::timeout, this, [this]() {
        {
            QMutexLocker locker(&m_mutex);
            m_retryScheduled = false;
        }
        checkNow();
    });

    // Connect to Google Drive client signals
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::changesReceived, this,
                &MetadataRefreshWorker::onChangesReceived);
        connect(m_driveClient, &GoogleDriveClient::startPageTokenReceived, this,
                &MetadataRefreshWorker::onStartPageTokenReceived);
        connect(m_driveClient, &GoogleDriveClient::errorDetailed, this,
                &MetadataRefreshWorker::onApiErrorDetailed);
        connect(m_driveClient, &GoogleDriveClient::error, this, &MetadataRefreshWorker::onApiError);
    }
}

MetadataRefreshWorker::~MetadataRefreshWorker() {
    stop();
}

// ============================================================================
// Configuration
// ============================================================================

void MetadataRefreshWorker::setPollingInterval(int intervalMs) {
    QMutexLocker locker(&m_mutex);
    m_pollingTimer->setInterval(intervalMs);
}

int MetadataRefreshWorker::pollingInterval() const {
    QMutexLocker locker(&m_mutex);
    return m_pollingTimer->interval();
}

void MetadataRefreshWorker::setChangeToken(const QString& token) {
    QMutexLocker locker(&m_mutex);
    m_changeToken = token;
}

QString MetadataRefreshWorker::changeToken() const {
    QMutexLocker locker(&m_mutex);
    return m_changeToken;
}

MetadataRefreshWorker::State MetadataRefreshWorker::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

// ============================================================================
// Control Methods
// ============================================================================

void MetadataRefreshWorker::start() {
    QMutexLocker locker(&m_mutex);

    if (m_state == State::Running) {
        qDebug() << "MetadataRefreshWorker: Already running";
        return;
    }

    if (!m_driveClient) {
        locker.unlock();
        emit error(QStringLiteral("Google Drive client not available"));
        return;
    }

    // Load change token from database (fuse_sync_state table)
    m_changeToken = loadChangeToken();

    // If no token, need to get start page token first
    if (m_changeToken.isEmpty()) {
        qDebug() << "MetadataRefreshWorker: No change token, requesting start page token";
        m_waitingForToken = true;
        m_changesRequestInFlight = true;
        m_retryScheduled = false;
        m_pendingCheckRequested = false;
        m_retryTimer->stop();
        locker.unlock();
        // Invoke on the drive client's thread (main thread) to avoid cross-thread
        // QNetworkAccessManager usage
        QMetaObject::invokeMethod(m_driveClient, "getStartPageToken", Qt::QueuedConnection);
        return;
    }

    m_state = State::Running;
    m_retryScheduled = false;
    m_retryTimer->stop();
    m_pollingTimer->start();
    locker.unlock();

    emit stateChanged(State::Running);
    qInfo() << "MetadataRefreshWorker: Started with polling interval" << m_pollingTimer->interval()
            << "ms";

    // Perform an immediate check
    checkNow();
}

void MetadataRefreshWorker::stop() {
    QMutexLocker locker(&m_mutex);

    m_pollingTimer->stop();
    m_retryTimer->stop();
    m_state = State::Stopped;
    m_waitingForToken = false;
    m_changesRequestInFlight = false;
    m_retryScheduled = false;
    m_pendingCheckRequested = false;
    m_consecutiveTransientFailures = 0;

    locker.unlock();
    emit stateChanged(State::Stopped);
    qInfo() << "MetadataRefreshWorker: Stopped";
}

void MetadataRefreshWorker::pause() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running) {
        return;
    }

    m_pollingTimer->stop();
    m_retryTimer->stop();
    m_state = State::Paused;
    m_retryScheduled = false;
    m_pendingCheckRequested = false;

    locker.unlock();
    emit stateChanged(State::Paused);
    qDebug() << "MetadataRefreshWorker: Paused";
}

void MetadataRefreshWorker::resume() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Paused) {
        return;
    }

    m_state = State::Running;
    m_retryTimer->stop();
    m_retryScheduled = false;
    m_pollingTimer->start();

    locker.unlock();
    emit stateChanged(State::Running);
    qDebug() << "MetadataRefreshWorker: Resumed";

    // Check for changes immediately
    checkNow();
}

void MetadataRefreshWorker::checkNow() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running && !m_waitingForToken) {
        return;
    }

    if (m_changesRequestInFlight || m_retryScheduled) {
        m_pendingCheckRequested = true;
        qDebug()
            << "MetadataRefreshWorker: Remote changes request already in flight; deferring check";
        return;
    }

    if (m_changeToken.isEmpty()) {
        qDebug() << "MetadataRefreshWorker: No change token available";
        m_waitingForToken = true;
        m_changesRequestInFlight = true;
        locker.unlock();
        QMetaObject::invokeMethod(m_driveClient, "getStartPageToken", Qt::QueuedConnection);
        return;
    }

    m_changesRequestInFlight = true;
    QString token = m_changeToken;
    locker.unlock();

    qDebug() << "MetadataRefreshWorker: Checking for remote changes";
    // Invoke on the drive client's thread (main thread) to avoid cross-thread QNetworkAccessManager
    // usage
    QMetaObject::invokeMethod(m_driveClient, "listChanges", Qt::QueuedConnection,
                              Q_ARG(QString, token));
}

// ============================================================================
// Private Slots
// ============================================================================

void MetadataRefreshWorker::onPollingTimeout() {
    checkNow();
}

void MetadataRefreshWorker::onChangesReceived(const QList<DriveChange>& changes,
                                              const QString& newToken, bool hasMorePages) {
    qDebug() << "MetadataRefreshWorker: Received" << changes.size() << "changes"
             << "hasMorePages:" << hasMorePages;

    // Process each change according to flow chart:
    // - Modified --> INVALIDATE_CACHE
    // - Deleted --> DELETE_META_DB
    // - Created --> UPDATE_META_CACHE
    int processedCount = 0;
    QList<DriveChange> pendingChanges = changes;
    bool madeProgress = true;
    while (!pendingChanges.isEmpty() && madeProgress) {
        madeProgress = false;
        QList<DriveChange> unresolvedChanges;
        for (const DriveChange& change : pendingChanges) {
            if (processChange(change)) {
                processedCount++;
                madeProgress = true;
            } else {
                unresolvedChanges.append(change);
            }
        }
        pendingChanges = unresolvedChanges;
    }

    if (!pendingChanges.isEmpty()) {
        qWarning() << "MetadataRefreshWorker: Could not resolve" << pendingChanges.size()
                   << "remote changes; retaining change token for retry";
        emit error(
            QStringLiteral("Remote changes could not be applied because their parent "
                           "folders are not available yet"));

        {
            QMutexLocker locker(&m_mutex);
            m_changesRequestInFlight = false;
            m_retryScheduled = (m_state == State::Running);
        }
        if (m_state == State::Running) {
            m_retryTimer->start(kTransientRetryBaseDelayMs);
        } else {
            m_retryTimer->stop();
        }
        emit refreshCompleted(processedCount);
        return;
    }

    // Update and save the change token
    bool runDeferredCheck = false;
    bool tokenUpdated = false;
    {
        QMutexLocker locker(&m_mutex);
        tokenUpdated = (m_changeToken != newToken);
        m_changeToken = newToken;
        m_changesRequestInFlight = false;
        m_retryScheduled = false;
        runDeferredCheck = m_pendingCheckRequested;
        m_pendingCheckRequested = false;
        m_consecutiveTransientFailures = 0;
    }
    m_retryTimer->stop();
    saveChangeToken(newToken);

    emit changeTokenUpdated(newToken);
    emit refreshCompleted(processedCount);

    if (hasMorePages && tokenUpdated) {
        QTimer::singleShot(0, this, &MetadataRefreshWorker::checkNow);
    } else if (runDeferredCheck) {
        QTimer::singleShot(0, this, &MetadataRefreshWorker::checkNow);
    }
}

void MetadataRefreshWorker::onStartPageTokenReceived(const QString& token) {
    QMutexLocker locker(&m_mutex);

    m_changeToken = token;
    bool wasWaiting = m_waitingForToken;
    const State stateBeforeUnlock = m_state;
    m_waitingForToken = false;
    m_changesRequestInFlight = false;
    m_retryScheduled = false;
    m_pendingCheckRequested = false;
    m_consecutiveTransientFailures = 0;

    locker.unlock();
    m_retryTimer->stop();

    // Save token to database
    saveChangeToken(token);
    emit changeTokenUpdated(token);

    qInfo() << "MetadataRefreshWorker: Received start page token";

    // If we were waiting for token to start, complete the startup
    // Note: start() will now see m_changeToken is set and proceed directly
    // to starting the timer, avoiding any recursive token requests
    if (wasWaiting) {
        if (stateBeforeUnlock == State::Running) {
            QTimer::singleShot(0, this, &MetadataRefreshWorker::checkNow);
        } else {
            start();
        }
    }
}

void MetadataRefreshWorker::onApiErrorDetailed(const QString& operation,
                                               const QString& errorMessage, int httpStatus,
                                               const QString& fileId, const QString& localPath) {
    Q_UNUSED(fileId)
    Q_UNUSED(localPath)

    if (!operation.contains(QStringLiteral("changes"), Qt::CaseInsensitive) &&
        !operation.contains(QStringLiteral("token"), Qt::CaseInsensitive)) {
        return;
    }

    if (isExpiredChangeTokenError(errorMessage, httpStatus)) {
        recoverExpiredChangeToken(errorMessage);
    }
}

void MetadataRefreshWorker::onApiError(const QString& operation, const QString& errorMessage) {
    // Only handle errors related to changes API
    if (operation.contains(QStringLiteral("changes"), Qt::CaseInsensitive) ||
        operation.contains(QStringLiteral("token"), Qt::CaseInsensitive)) {
        if (isExpiredChangeTokenError(errorMessage, 0)) {
            return;
        }

        const bool transient = isTransientPollError(errorMessage);
        bool shouldRetry = false;
        bool shouldSurface = false;
        int retryDelayMs = 0;

        {
            QMutexLocker locker(&m_mutex);
            m_changesRequestInFlight = false;
            if (transient) {
                m_consecutiveTransientFailures += 1;
                shouldRetry = (m_state == State::Running || m_waitingForToken);
                shouldSurface = m_consecutiveTransientFailures >= kTransientFailureSurfaceThreshold;
                retryDelayMs = transientRetryDelayMs(m_consecutiveTransientFailures);
                m_retryScheduled = shouldRetry;
            } else {
                m_retryScheduled = false;
                m_pendingCheckRequested = false;
                m_consecutiveTransientFailures = 0;
            }
        }

        if (transient) {
            qWarning() << "MetadataRefreshWorker: transient API error in" << operation << "-"
                       << errorMessage << "retrying in" << retryDelayMs << "ms";
            if (shouldSurface) {
                emit error(QStringLiteral("API error: ") + errorMessage);
            }
            if (shouldRetry) {
                m_retryTimer->start(retryDelayMs);
            } else {
                m_retryTimer->stop();
            }
            return;
        }

        m_retryTimer->stop();
        qWarning() << "MetadataRefreshWorker: API error in" << operation << "-" << errorMessage;
        emit error(QStringLiteral("API error: ") + errorMessage);
    }
}

void MetadataRefreshWorker::recoverExpiredChangeToken(const QString& errorMessage) {
    bool shouldRequestToken = false;
    {
        QMutexLocker locker(&m_mutex);
        m_changeToken.clear();
        m_waitingForToken = (m_state == State::Running || m_waitingForToken);
        shouldRequestToken = m_waitingForToken && m_driveClient;
        m_changesRequestInFlight = false;
        m_retryScheduled = false;
        m_pendingCheckRequested = false;
        m_consecutiveTransientFailures = 0;
    }
    m_retryTimer->stop();

    saveChangeToken(QString());

    qWarning() << "MetadataRefreshWorker: Drive change token expired, requesting a new start"
               << "page token" << errorMessage;
    emit error(QStringLiteral(
        "Drive change token expired; restarting remote change tracking. Cached metadata may "
        "refresh lazily until listings are revisited."));

    if (shouldRequestToken) {
        QMetaObject::invokeMethod(m_driveClient, "getStartPageToken", Qt::QueuedConnection);
    }
}

// ============================================================================
// Private Methods
// ============================================================================

bool MetadataRefreshWorker::processChange(const DriveChange& change) {
    if (!change.isValid()) {
        qDebug() << "MetadataRefreshWorker: Skipping invalid change";
        return true;
    }

    const RemoteNodeSnapshot nodeSnapshot = snapshotRemoteNodes(m_database, change.fileId);

    // Deleted file (removed or trashed)
    if (change.removed || change.file.trashed) {
        qDebug() << "MetadataRefreshWorker: File deleted -" << change.fileId;

        if (m_database) {
            const FuseOperationAck ack =
                m_database->getPendingFuseOperationAckByRemoteFileId(change.fileId);
            if (ack.ackId > 0) {
                m_database->markFuseOperationAckApplied(ack.ackId);
            }
        }

        // Capture display path before removeFromCaches wipes the metadata.
        const QString displayPath = resolveDisplayPath(change.fileId, change.file.name);

        if (nodeSnapshot.hasPendingNamespaceOp || nodeSnapshot.hasLocalGenerationAhead) {
            qDebug() << "MetadataRefreshWorker: Preserving local node state for remote delete"
                     << change.fileId;
            emit changeProcessed(change.fileId, QStringLiteral("deleted"));
            if (!displayPath.isEmpty()) {
                emit changeProcessedDetailed(displayPath, QStringLiteral("deleted"));
            }
            return true;
        }

        if (!nodeSnapshot.nodes.isEmpty()) {
            QList<FuseNode> subtreeNodes;
            const QList<FuseNode> allNodes = m_database->getAllFuseNodes();
            for (const FuseNode& rootNode : nodeSnapshot.nodes) {
                for (const FuseNode& candidate : allNodes) {
                    if (pathWithinNodeSubtree(candidate.path, rootNode.path)) {
                        subtreeNodes.append(candidate);
                    }
                }
            }

            if (m_metadataCache && !displayPath.isEmpty()) {
                m_metadataCache->dropSubtreeFromCache(displayPath);
            }
            deleteNodeSubtree(m_database, subtreeNodes);
        }

        removeFromCaches(change.fileId);
        emit changeProcessed(change.fileId, QStringLiteral("deleted"));
        if (!displayPath.isEmpty()) {
            emit changeProcessedDetailed(displayPath, QStringLiteral("deleted"));
        }
        return true;
    }

    // Check if we should process this file
    if (!shouldProcess(change.file)) {
        qDebug() << "MetadataRefreshWorker: Skipping file -" << change.file.name;
        return true;
    }

    // For modified files, we need to invalidate the file cache
    // This forces a re-download on next access
    // Check if we already have this file in metadata cache
    bool isModification = false;
    bool skipMetadataUpdate = false;
    bool selfOriginatedAck = false;
    if (m_database) {
        const FuseOperationAck ack =
            m_database->getPendingFuseOperationAckByRemoteFileId(change.fileId);
        if (ack.ackId > 0) {
            selfOriginatedAck = true;
            m_database->markFuseOperationAckApplied(ack.ackId);
        }
    }

    if (m_metadataCache) {
        FuseFileMetadata existing = m_metadataCache->getMetadataByFileId(change.fileId);
        if (existing.isValid()) {
            // File exists, this is a modification
            isModification = true;

            // Fix 1: If this change was caused by our own upload, skip
            // the invalidation — the local cache already has the correct
            // content and invalidating would delete the file from under
            // any open FUSE handles.
            if (selfOriginatedAck || (m_fileCache && !selfOriginatedAck &&
                                      m_fileCache->consumeRecentlyUploaded(change.fileId))) {
                qDebug() << "MetadataRefreshWorker: Skipping invalidation of self-uploaded file"
                         << change.fileId;
            } else {
                if (nodeSnapshot.hasLocalGenerationAhead) {
                    qDebug() << "MetadataRefreshWorker: Skipping invalidation of locally newer node"
                             << change.fileId;
                    skipMetadataUpdate = true;
                } else if (nodeSnapshot.hasPendingNamespaceOp) {
                    qDebug() << "MetadataRefreshWorker: Skipping metadata overwrite while local "
                                "namespace op is pending"
                             << change.fileId;
                    skipMetadataUpdate = true;
                } else {
                    invalidateFileCache(change.fileId);
                }
            }
        }
    }

    // Update metadata cache with new/modified file info
    FuseFileMetadata metadata;
    if (!skipMetadataUpdate) {
        metadata = updateMetadataCache(change.file);
        if (metadata.isValid()) {
            reconcileRemoteNodeMetadata(m_database, metadata);
        }
        if (!metadata.isValid()) {
            return false;
        }
    }

    QString changeType = isModification ? QStringLiteral("modified") : QStringLiteral("created");
    const QString displayPath = resolveDisplayPath(change.fileId, change.file.name);
    emit changeProcessed(change.fileId, changeType);
    if (!displayPath.isEmpty()) {
        emit changeProcessedDetailed(displayPath, changeType);
    }
    return true;
}

FuseFileMetadata MetadataRefreshWorker::updateMetadataCache(const DriveFile& file) {
    if (!m_metadataCache) {
        return FuseFileMetadata();
    }

    const FuseFileMetadata metadata = m_metadataCache->upsertRemoteMetadata(file);
    if (metadata.isValid()) {
        qDebug() << "MetadataRefreshWorker: Updated metadata for" << metadata.path;
    } else {
        // Cannot resolve path - parent folder not in cache
        // This is expected for files in folders not yet browsed
        qDebug() << "MetadataRefreshWorker: Could not resolve path for" << file.name
                 << "(parent folder not in cache)";
    }

    return metadata;
}

void MetadataRefreshWorker::removeFromCaches(const QString& fileId) {
    // Fix 1: If this deletion change was caused by our own action and the
    // file was recently uploaded, consume the marker.  In practice,
    // removeFromCaches is only called for truly deleted/trashed files so
    // this guard is a safety net.
    if (m_fileCache) {
        m_fileCache->consumeRecentlyUploaded(fileId);
    }

    // Remove from metadata cache
    if (m_metadataCache) {
        m_metadataCache->removeByFileId(fileId);
    }

    if (m_database && !fileId.isEmpty()) {
        m_database->deleteNativeDocState(fileId);
    }

    // Remove from file cache (if cached)
    if (m_fileCache) {
        m_fileCache->removeFromCache(fileId);
    }

    qDebug() << "MetadataRefreshWorker: Removed from caches -" << fileId;
}

void MetadataRefreshWorker::invalidateFileCache(const QString& fileId) {
    if (m_fileCache) {
        m_fileCache->invalidate(fileId);
        qDebug() << "MetadataRefreshWorker: Invalidated file cache for" << fileId;
    }
}

QString MetadataRefreshWorker::resolveDisplayPath(const QString& fileId,
                                                  const QString& fallbackName) const {
    if (m_metadataCache) {
        const FuseFileMetadata metadata = m_metadataCache->getMetadataByFileId(fileId);
        if (!metadata.path.isEmpty()) {
            return metadata.path;
        }
    }

    if (m_database) {
        const FuseMetadata metadata = m_database->getFuseMetadata(fileId);
        if (!metadata.path.isEmpty()) {
            return metadata.path;
        }
    }

    return fallbackName;
}

bool MetadataRefreshWorker::shouldProcess(const DriveFile& file) const {
    // Skip files not owned by user (shared files)
    if (!file.ownedByMe) {
        return false;
    }

    if (resolvesToTrashRelativePath(file, m_metadataCache, m_database)) {
        return false;
    }

    // Google Workspace files (Docs, Sheets, etc.) are handled based on the
    // native-doc serving mode.  In "hide" mode they are skipped entirely;
    // in other modes the policy helper decides per-type visibility.
    if (file.isGoogleDoc() && !file.isFolder && !file.isShortcut) {
        QString nativeDocModeOverride;
        if (m_metadataCache) {
            const FuseFileMetadata cached = m_metadataCache->getMetadataByFileId(file.id);
            if (cached.isValid()) {
                nativeDocModeOverride = cached.nativeDocModeOverride;
            }
        }

        if (nativeDocModeOverride.isEmpty() && m_database) {
            nativeDocModeOverride = m_database->getNativeDocState(file.id).nativeDocModeOverride;
        }

        QSettings settings;
        const NativeDocMode globalMode =
            nativeDocModeFromString(settings.value("advanced/nativeDocMode", "hide").toString());
        NativeDocRepresentation rep =
            effectiveNativeDocRepresentation(file.mimeType, nativeDocModeOverride, globalMode);
        return rep.visible;
    }

    // Skip trashed files (handled separately via change.removed)
    if (file.trashed) {
        return false;
    }

    return true;
}

QString MetadataRefreshWorker::loadChangeToken() const {
    if (!m_database) {
        return QString();
    }
    return m_database->getFuseSyncState(FUSE_CHANGE_TOKEN_KEY);
}

void MetadataRefreshWorker::saveChangeToken(const QString& token) {
    if (!m_database) {
        return;
    }
    m_database->setFuseSyncState(FUSE_CHANGE_TOKEN_KEY, token);
}
