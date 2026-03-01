/**
 * @file DirtySyncWorker.cpp
 * @brief Implementation of background worker for uploading dirty files in FUSE mode
 */

#include "DirtySyncWorker.h"

#include <QDebug>
#include <QFileInfo>
#include <QMutexLocker>

#include "FileCache.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "sync/SyncDatabase.h"

DirtySyncWorker::DirtySyncWorker(FileCache* fileCache, GoogleDriveClient* driveClient,
                                 SyncDatabase* database, QObject* parent)
    : QObject(parent),
      m_fileCache(fileCache),
      m_driveClient(driveClient),
      m_database(database),
      m_syncTimer(new QTimer(this)),
      m_state(DirtySyncWorkerState::Stopped),
      m_syncIntervalMs(DEFAULT_SYNC_INTERVAL_MS),
      m_maxRetries(DEFAULT_MAX_RETRIES),
      m_uploadTimeoutMs(DEFAULT_UPLOAD_TIMEOUT_MS),
      m_uploadedCount(0),
      m_failedCount(0),
      m_uploadInProgress(false),
      m_uploadSuccess(false),
      m_flushing(false) {
    // Configure sync timer
    m_syncTimer->setInterval(m_syncIntervalMs);
    connect(m_syncTimer, &QTimer::timeout, this, &DirtySyncWorker::onSyncTimerTimeout);

    // Connect to GoogleDriveClient signals for upload completion
    if (m_driveClient) {
        // Connect to fileUpdated signal (used by updateFile for existing files)
        connect(m_driveClient, &GoogleDriveClient::fileUpdated, this,
                &DirtySyncWorker::onFileUploaded);
        // Also connect to fileUploaded for new file uploads
        connect(m_driveClient, &GoogleDriveClient::fileUploaded, this,
                &DirtySyncWorker::onFileUploaded);
        // H5 fix: use errorDetailed so we can filter by fileId
        connect(m_driveClient, &GoogleDriveClient::errorDetailed, this,
                &DirtySyncWorker::onUploadErrorDetailed);
    }
}

DirtySyncWorker::~DirtySyncWorker() {
    // Ensure worker is stopped
    if (m_state != DirtySyncWorkerState::Stopped) {
        stop();
    }
}

// ============================================================================
// Configuration
// ============================================================================

int DirtySyncWorker::syncIntervalMs() const {
    QMutexLocker locker(&m_mutex);
    return m_syncIntervalMs;
}

void DirtySyncWorker::setSyncIntervalMs(int ms) {
    QMutexLocker locker(&m_mutex);

    // Enforce minimum interval
    m_syncIntervalMs = qMax(ms, MIN_SYNC_INTERVAL_MS);

    // Update timer if running
    if (m_syncTimer->isActive()) {
        m_syncTimer->setInterval(m_syncIntervalMs);
    }

    qDebug() << "DirtySyncWorker: Sync interval set to" << m_syncIntervalMs << "ms";
}

int DirtySyncWorker::maxRetries() const {
    QMutexLocker locker(&m_mutex);
    return m_maxRetries;
}

void DirtySyncWorker::setMaxRetries(int count) {
    QMutexLocker locker(&m_mutex);
    m_maxRetries = qMax(count, 0);
}

int DirtySyncWorker::uploadTimeoutMs() const {
    QMutexLocker locker(&m_mutex);
    return m_uploadTimeoutMs;
}

void DirtySyncWorker::setUploadTimeoutMs(int ms) {
    QMutexLocker locker(&m_mutex);
    m_uploadTimeoutMs = qMax(ms, MIN_UPLOAD_TIMEOUT_MS);
    qDebug() << "DirtySyncWorker: Upload timeout set to" << m_uploadTimeoutMs << "ms";
}

DirtySyncWorkerState DirtySyncWorker::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

// ============================================================================
// Statistics
// ============================================================================

int DirtySyncWorker::pendingCount() const {
    if (!m_fileCache) {
        return 0;
    }
    return m_fileCache->getDirtyFiles().count();
}

int DirtySyncWorker::uploadedCount() const {
    QMutexLocker locker(&m_mutex);
    return m_uploadedCount;
}

int DirtySyncWorker::failedCount() const {
    QMutexLocker locker(&m_mutex);
    return m_failedCount;
}

// ============================================================================
// Public Slots
// ============================================================================

void DirtySyncWorker::start() {
    QMutexLocker locker(&m_mutex);

    if (m_state == DirtySyncWorkerState::Running) {
        qDebug() << "DirtySyncWorker: Already running";
        return;
    }

    qInfo() << "DirtySyncWorker: Starting with interval" << m_syncIntervalMs << "ms";

    // Reset statistics
    m_uploadedCount = 0;
    m_failedCount = 0;

    // Start periodic timer
    m_syncTimer->setInterval(m_syncIntervalMs);
    m_syncTimer->start();

    setState(DirtySyncWorkerState::Running);

    // Trigger immediate first sync
    locker.unlock();
    syncNow();
}

void DirtySyncWorker::stop() {
    QMutexLocker locker(&m_mutex);

    if (m_state == DirtySyncWorkerState::Stopped) {
        return;
    }

    qInfo() << "DirtySyncWorker: Stopping";

    // Stop the timer
    m_syncTimer->stop();

    // Wake up any waiting operations
    m_uploadCondition.wakeAll();
    m_flushCondition.wakeAll();

    setState(DirtySyncWorkerState::Stopped);

    qInfo() << "DirtySyncWorker: Stopped. Uploaded:" << m_uploadedCount
            << "Failed:" << m_failedCount;
}

void DirtySyncWorker::pause() {
    QMutexLocker locker(&m_mutex);

    if (m_state != DirtySyncWorkerState::Running) {
        return;
    }

    qInfo() << "DirtySyncWorker: Pausing";

    m_syncTimer->stop();
    setState(DirtySyncWorkerState::Paused);
}

void DirtySyncWorker::resume() {
    QMutexLocker locker(&m_mutex);

    if (m_state != DirtySyncWorkerState::Paused) {
        return;
    }

    qInfo() << "DirtySyncWorker: Resuming";

    m_syncTimer->start();
    setState(DirtySyncWorkerState::Running);

    // Trigger immediate sync on resume
    locker.unlock();
    syncNow();
}

void DirtySyncWorker::flushAndStop() {
    qInfo() << "DirtySyncWorker: Flushing all dirty files before stop";

    {
        QMutexLocker locker(&m_mutex);

        if (m_state == DirtySyncWorkerState::Stopped) {
            emit flushCompleted(true);
            return;
        }

        // Stop the timer to prevent concurrent syncs
        m_syncTimer->stop();
        m_flushing = true;
    }

    // Process all remaining dirty files
    processDirtyFiles();

    // Check if all files were uploaded
    bool success = true;
    if (m_fileCache) {
        QList<DirtyFileEntry> remaining = m_fileCache->getDirtyFiles();
        if (!remaining.isEmpty()) {
            qWarning() << "DirtySyncWorker: Flush incomplete," << remaining.size()
                       << "files still dirty";
            success = false;
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        m_flushing = false;
        m_flushCondition.wakeAll();
    }

    emit flushCompleted(success);

    // Now stop
    stop();
}

void DirtySyncWorker::syncNow() {
    QMutexLocker locker(&m_mutex);

    if (m_state == DirtySyncWorkerState::Stopped || m_state == DirtySyncWorkerState::Paused) {
        qDebug() << "DirtySyncWorker: Cannot sync - worker is not running";
        return;
    }

    if (m_state == DirtySyncWorkerState::Uploading) {
        qDebug() << "DirtySyncWorker: Sync already in progress";
        return;
    }

    locker.unlock();
    processDirtyFiles();
}

// ============================================================================
// Private Slots
// ============================================================================

void DirtySyncWorker::onSyncTimerTimeout() {
    QMutexLocker locker(&m_mutex);

    // Skip if not running or already uploading
    if (m_state != DirtySyncWorkerState::Running) {
        return;
    }

    locker.unlock();
    processDirtyFiles();
}

void DirtySyncWorker::onFileUploaded(const DriveFile& file) {
    QMutexLocker locker(&m_mutex);

    // Check if this is the file we're waiting for
    if (m_uploadInProgress && m_currentUploadFileId == file.id) {
        qDebug() << "DirtySyncWorker: Upload completed for" << file.id;
        m_uploadSuccess = true;
        m_uploadError.clear();
        m_uploadCondition.wakeAll();
    }
}

void DirtySyncWorker::onUploadErrorDetailed(const QString& operation, const QString& errorMsg,
                                            int httpStatus, const QString& fileId,
                                            const QString& localPath) {
    Q_UNUSED(httpStatus)
    Q_UNUSED(localPath)

    // Filter for upload-related errors
    if (!operation.startsWith("upload") && !operation.startsWith("update")) {
        return;
    }

    QMutexLocker locker(&m_mutex);

    // H5 fix: only react when the error is for the file we are uploading
    if (m_uploadInProgress && m_currentUploadFileId == fileId) {
        qWarning() << "DirtySyncWorker: Upload error for" << fileId << ":" << operation << "-"
                   << errorMsg;
        m_uploadSuccess = false;
        m_uploadError = errorMsg;
        m_uploadCondition.wakeAll();
    }
}

// ============================================================================
// Private Methods
// ============================================================================

void DirtySyncWorker::processDirtyFiles() {
    if (!m_fileCache || !m_driveClient) {
        qWarning() << "DirtySyncWorker: Missing dependencies";
        return;
    }

    // Get list of dirty files
    QList<DirtyFileEntry> dirtyFiles = m_fileCache->getDirtyFiles();

    if (dirtyFiles.isEmpty()) {
        qDebug() << "DirtySyncWorker: No dirty files to process";
        emit syncCycleCompleted(0, 0);
        return;
    }

    qInfo() << "DirtySyncWorker: Processing" << dirtyFiles.size() << "dirty files";

    {
        QMutexLocker locker(&m_mutex);
        setState(DirtySyncWorkerState::Uploading);
    }

    int cycleUploaded = 0;
    int cycleFailed = 0;

    for (const DirtyFileEntry& entry : dirtyFiles) {
        // Check if we should stop
        {
            QMutexLocker locker(&m_mutex);
            if (m_state == DirtySyncWorkerState::Stopped && !m_flushing) {
                break;
            }
        }

        // GPT5.3 #8: skip files that exceeded max retries
        {
            QMutexLocker locker(&m_mutex);
            int retries = m_retryCounts.value(entry.fileId, 0);
            if (retries >= m_maxRetries) {
                qWarning() << "DirtySyncWorker: Skipping" << entry.path
                           << "- exceeded max retries (" << retries << ")";
                cycleFailed++;
                m_failedCount++;
                emit uploadFailed(entry.fileId, entry.path, QStringLiteral("Exceeded max retries"));
                continue;
            }
        }

        emit uploadStarted(entry.fileId, entry.path);

        // Attempt upload
        bool success = uploadFile(entry.fileId, entry.path);

        if (success) {
            // Clear dirty flag and reset retry count
            m_fileCache->clearDirty(entry.fileId);
            cycleUploaded++;

            {
                QMutexLocker locker(&m_mutex);
                m_uploadedCount++;
                m_retryCounts.remove(entry.fileId);
            }

            emit uploadCompleted(entry.fileId, entry.path);
            qInfo() << "DirtySyncWorker: Uploaded" << entry.path;
        } else {
            // Mark as failed and bump retry count (GPT5.3 #8)
            m_fileCache->markUploadFailed(entry.fileId);
            cycleFailed++;

            QString errorMsg;
            {
                QMutexLocker locker(&m_mutex);
                m_failedCount++;
                m_retryCounts[entry.fileId] = m_retryCounts.value(entry.fileId, 0) + 1;
                errorMsg = m_uploadError;
            }

            emit uploadFailed(entry.fileId, entry.path, errorMsg);
            qWarning() << "DirtySyncWorker: Failed to upload" << entry.path << ":" << errorMsg;
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        if (m_state == DirtySyncWorkerState::Uploading) {
            setState(DirtySyncWorkerState::Running);
        }
    }

    emit syncCycleCompleted(cycleUploaded, cycleFailed);

    qInfo() << "DirtySyncWorker: Sync cycle completed. Uploaded:" << cycleUploaded
            << "Failed:" << cycleFailed;
}

bool DirtySyncWorker::uploadFile(const QString& fileId, const QString& path) {
    Q_UNUSED(path)

    if (!m_driveClient || !m_fileCache) {
        return false;
    }

    // Get the cached file path
    QString cachePath = m_fileCache->getCachePathForFile(fileId);
    if (cachePath.isEmpty() || !QFileInfo::exists(cachePath)) {
        qWarning() << "DirtySyncWorker: Cached file not found for" << fileId;
        return false;
    }

    // Setup upload tracking
    int timeoutMs;
    {
        QMutexLocker locker(&m_mutex);
        m_currentUploadFileId = fileId;
        m_uploadInProgress = true;
        m_uploadSuccess = false;
        m_uploadError.clear();
        timeoutMs = m_uploadTimeoutMs;
    }

    // Initiate upload on the drive client's thread (main thread) to avoid cross-thread
    // QNetworkAccessManager usage
    QMetaObject::invokeMethod(m_driveClient, "updateFile", Qt::QueuedConnection,
                              Q_ARG(QString, fileId), Q_ARG(QString, cachePath));

    // Wait for upload completion with timeout
    {
        QMutexLocker locker(&m_mutex);

        if (!m_uploadCondition.wait(&m_mutex, timeoutMs)) {
            qWarning() << "DirtySyncWorker: Upload timeout for" << fileId;
            m_uploadInProgress = false;
            m_currentUploadFileId.clear();
            return false;
        }

        bool success = m_uploadSuccess;
        m_uploadInProgress = false;
        m_currentUploadFileId.clear();

        return success;
    }
}

void DirtySyncWorker::setState(DirtySyncWorkerState newState) {
    // Assumes mutex is already held by caller
    if (m_state != newState) {
        m_state = newState;
        emit stateChanged(newState);
    }
}
