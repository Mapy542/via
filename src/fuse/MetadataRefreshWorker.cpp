/**
 * @file MetadataRefreshWorker.cpp
 * @brief Implementation of background worker for FUSE metadata refresh
 *
 * Implements the "Metadata Refresh Thread" from the FUSE procedure flow chart.
 */

#include "MetadataRefreshWorker.h"

#include <QDebug>
#include <QMutexLocker>

#include "FileCache.h"
#include "MetadataCache.h"
#include "api/DriveChange.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "sync/SyncDatabase.h"

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
      m_state(State::Stopped),
      m_waitingForToken(false) {
    // Configure polling timer with default interval
    m_pollingTimer->setInterval(DEFAULT_POLL_INTERVAL_MS);

    // Connect timer
    connect(m_pollingTimer, &QTimer::timeout, this, &MetadataRefreshWorker::onPollingTimeout);

    // Connect to Google Drive client signals
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::changesReceived, this,
                &MetadataRefreshWorker::onChangesReceived);
        connect(m_driveClient, &GoogleDriveClient::startPageTokenReceived, this,
                &MetadataRefreshWorker::onStartPageTokenReceived);
        connect(m_driveClient, &GoogleDriveClient::error, this, &MetadataRefreshWorker::onApiError);
    }
}

MetadataRefreshWorker::~MetadataRefreshWorker() { stop(); }

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
        locker.unlock();
        // Invoke on the drive client's thread (main thread) to avoid cross-thread
        // QNetworkAccessManager usage
        QMetaObject::invokeMethod(m_driveClient, "getStartPageToken", Qt::QueuedConnection);
        return;
    }

    m_state = State::Running;
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
    m_state = State::Stopped;
    m_waitingForToken = false;

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
    m_state = State::Paused;

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
    m_pollingTimer->start();

    locker.unlock();
    emit stateChanged(State::Running);
    qDebug() << "MetadataRefreshWorker: Resumed";

    // Check for changes immediately
    checkNow();
}

void MetadataRefreshWorker::checkNow() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running) {
        return;
    }

    if (m_changeToken.isEmpty()) {
        qDebug() << "MetadataRefreshWorker: No change token available";
        return;
    }

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

void MetadataRefreshWorker::onPollingTimeout() { checkNow(); }

void MetadataRefreshWorker::onChangesReceived(const QList<DriveChange>& changes,
                                              const QString& newToken) {
    qDebug() << "MetadataRefreshWorker: Received" << changes.size() << "changes";

    // Process each change according to flow chart:
    // - Modified --> INVALIDATE_CACHE
    // - Deleted --> DELETE_META_DB
    // - Created --> UPDATE_META_CACHE
    int processedCount = 0;
    for (const DriveChange& change : changes) {
        processChange(change);
        processedCount++;
    }

    // Update and save the change token
    {
        QMutexLocker locker(&m_mutex);
        m_changeToken = newToken;
    }
    saveChangeToken(newToken);

    emit changeTokenUpdated(newToken);
    emit refreshCompleted(processedCount);
}

void MetadataRefreshWorker::onStartPageTokenReceived(const QString& token) {
    QMutexLocker locker(&m_mutex);

    m_changeToken = token;
    bool wasWaiting = m_waitingForToken;
    m_waitingForToken = false;

    locker.unlock();

    // Save token to database
    saveChangeToken(token);
    emit changeTokenUpdated(token);

    qInfo() << "MetadataRefreshWorker: Received start page token";

    // If we were waiting for token to start, complete the startup
    // Note: start() will now see m_changeToken is set and proceed directly
    // to starting the timer, avoiding any recursive token requests
    if (wasWaiting) {
        start();
    }
}

void MetadataRefreshWorker::onApiError(const QString& operation, const QString& errorMessage) {
    // Only handle errors related to changes API
    if (operation.contains(QStringLiteral("changes"), Qt::CaseInsensitive) ||
        operation.contains(QStringLiteral("token"), Qt::CaseInsensitive)) {
        qWarning() << "MetadataRefreshWorker: API error in" << operation << "-" << errorMessage;
        emit error(QStringLiteral("API error: ") + errorMessage);
    }
}

// ============================================================================
// Private Methods
// ============================================================================

void MetadataRefreshWorker::processChange(const DriveChange& change) {
    if (!change.isValid()) {
        qDebug() << "MetadataRefreshWorker: Skipping invalid change";
        return;
    }

    // Deleted file (removed or trashed)
    if (change.removed || change.file.trashed) {
        qDebug() << "MetadataRefreshWorker: File deleted -" << change.fileId;
        removeFromCaches(change.fileId);
        emit changeProcessed(change.fileId, QStringLiteral("deleted"));
        return;
    }

    // Check if we should process this file
    if (!shouldProcess(change.file)) {
        qDebug() << "MetadataRefreshWorker: Skipping file -" << change.file.name;
        return;
    }

    // For modified files, we need to invalidate the file cache
    // This forces a re-download on next access
    // Check if we already have this file in metadata cache
    bool isModification = false;
    if (m_metadataCache) {
        FuseFileMetadata existing = m_metadataCache->getMetadataByFileId(change.fileId);
        if (existing.isValid()) {
            // File exists, this is a modification
            isModification = true;
            invalidateFileCache(change.fileId);
        }
    }

    // Update metadata cache with new/modified file info
    updateMetadataCache(change.file);

    emit changeProcessed(change.fileId,
                         isModification ? QStringLiteral("modified") : QStringLiteral("created"));
}

void MetadataRefreshWorker::updateMetadataCache(const DriveFile& file) {
    if (!m_metadataCache) {
        return;
    }

    // Build FuseFileMetadata from DriveFile
    FuseFileMetadata metadata;
    metadata.fileId = file.id;
    metadata.name = file.name;
    metadata.parentId = file.parentId();
    metadata.isFolder = file.isFolder;
    metadata.size = file.size;
    metadata.mimeType = file.mimeType;
    metadata.createdTime = file.createdTime;
    metadata.modifiedTime = file.modifiedTime;
    metadata.cachedAt = QDateTime::currentDateTime();
    metadata.lastAccessed = QDateTime::currentDateTime();

    // Try to resolve the path from parent
    if (!metadata.parentId.isEmpty()) {
        QString rootId = m_metadataCache->rootFolderId();
        if (metadata.parentId == rootId) {
            // Parent is root folder — use bare name (no leading slash)
            // to match FuseDriver's path convention
            metadata.path = metadata.name;
        } else {
            QString parentPath = m_metadataCache->getPathByFileId(metadata.parentId);
            if (!parentPath.isEmpty()) {
                metadata.path = parentPath + QStringLiteral("/") + metadata.name;
            }
            // If parent path not found, we cannot resolve the full path yet.
            // The file may be in a folder that hasn't been browsed yet.
            // It will be properly resolved when the user navigates to that folder.
        }
    }

    // Only set if we have a valid path
    if (!metadata.path.isEmpty()) {
        m_metadataCache->setMetadata(metadata);
        qDebug() << "MetadataRefreshWorker: Updated metadata for" << metadata.path;
    } else {
        // Cannot resolve path - parent folder not in cache
        // This is expected for files in folders not yet browsed
        qDebug() << "MetadataRefreshWorker: Could not resolve path for" << file.name
                 << "(parent folder not in cache)";
    }
}

void MetadataRefreshWorker::removeFromCaches(const QString& fileId) {
    // Remove from metadata cache
    if (m_metadataCache) {
        m_metadataCache->removeByFileId(fileId);
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

bool MetadataRefreshWorker::shouldProcess(const DriveFile& file) const {
    // Skip files not owned by user (shared files)
    if (!file.ownedByMe) {
        return false;
    }

    // Skip Google Workspace files (Docs, Sheets, etc.) - they can't be downloaded as binary files.
    // Note: isGoogleDoc() checks mimeType.startsWith("application/vnd.google-apps.")
    // However, folders (application/vnd.google-apps.folder) and shortcuts should still be processed
    // since they don't require downloading actual file content.
    if (file.isGoogleDoc() && !file.isFolder && !file.isShortcut) {
        return false;
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
