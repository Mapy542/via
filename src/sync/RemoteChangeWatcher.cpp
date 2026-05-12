/**
 * @file RemoteChangeWatcher.cpp
 * @brief Implementation of remote Google Drive change watcher
 */

#include "RemoteChangeWatcher.h"

#include <QDebug>
#include <QMutexLocker>
#include <QSet>
#include <QtGlobal>

#include "ChangeQueue.h"
#include "FileFilter.h"
#include "MirrorPathResolver.h"
#include "SyncDatabase.h"
#include "SyncSettings.h"
#include "TrashPolicy.h"
#include "api/DriveChange.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "utils/NativeDocSupport.h"
#include "utils/PathUtils.h"

namespace {

constexpr int kTransientRetryBaseDelayMs = 300;
constexpr int kTransientRetryMaxDelayMs = 5000;
constexpr int kTransientFailureSurfaceThreshold = 3;

QString nativeDocModeOverrideForFile(const SyncDatabase* database, const QString& fileId) {
    if (!database || fileId.isEmpty()) {
        return QString();
    }

    return database->getNativeDocState(fileId).nativeDocModeOverride;
}

void persistObservedNativeDocState(SyncDatabase* database, const DriveFile& file,
                                   const QString& nativeDocModeOverride) {
    if (!database || file.id.isEmpty()) {
        return;
    }

    if (!isNativeDocMimeType(file.mimeType) && nativeDocModeOverride.isEmpty()) {
        database->deleteNativeDocState(file.id);
        return;
    }

    NativeDocState state;
    state.fileId = file.id;
    state.remoteName = file.name;
    state.remoteMimeType = file.mimeType;
    state.webViewLink = file.webViewLink;
    state.nativeDocModeOverride = nativeDocModeOverride;
    database->saveNativeDocState(state);
}

bool isTransientPollError(const QString& error) {
    const QString lowered = error.toLower();
    return lowered.contains(QStringLiteral("goaway")) ||
           lowered.contains(QStringLiteral("protocol error")) ||
           lowered.contains(QStringLiteral("stream error")) ||
           lowered.contains(QStringLiteral("connection reset")) ||
           lowered.contains(QStringLiteral("connection closed")) ||
           lowered.contains(QStringLiteral("remote host closed"));
}

int transientRetryDelayMs(int consecutiveFailures) {
    int delayMs = kTransientRetryBaseDelayMs;
    const int clampedFailures = qBound(1, consecutiveFailures, 5);
    for (int attempt = 1; attempt < clampedFailures; ++attempt) {
        delayMs = qMin(delayMs * 2, kTransientRetryMaxDelayMs);
    }
    return delayMs;
}

}  // namespace

const int RemoteChangeWatcher::DEFAULT_POLL_INTERVAL_MS;
const int RemoteChangeWatcher::CHANGE_BATCH_SLICE_SIZE;

RemoteChangeWatcher::RemoteChangeWatcher(ChangeQueue* changeQueue, GoogleDriveClient* driveClient,
                                         SyncDatabase* syncDatabase, QObject* parent)
    : QObject(parent),
      m_changeQueue(changeQueue),
      m_syncDatabase(syncDatabase),
      m_driveClient(driveClient),
      m_pollingTimer(new QTimer(this)),
      m_state(State::Stopped),
      m_waitingForToken(false) {
    // Configure polling timer
    m_settings = SyncSettings::load();
    m_batchThrottle.setSettings(m_settings);
    m_pollingTimer->setInterval(m_settings.remotePollIntervalMs > 0
                                    ? m_settings.remotePollIntervalMs
                                    : DEFAULT_POLL_INTERVAL_MS);

    // Connect signals
    connect(m_pollingTimer, &QTimer::timeout, this, &RemoteChangeWatcher::onPollingTimeout);

    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::changesReceived, this,
                &RemoteChangeWatcher::onChangesReceived);
        connect(m_driveClient, &GoogleDriveClient::startPageTokenReceived, this,
                &RemoteChangeWatcher::onStartPageTokenReceived);
        connect(m_driveClient, &GoogleDriveClient::error, this, &RemoteChangeWatcher::onApiError);
    }
}

RemoteChangeWatcher::~RemoteChangeWatcher() {
    stop();
}

void RemoteChangeWatcher::setPollingInterval(int intervalMs) {
    QMutexLocker locker(&m_mutex);
    m_pollingTimer->setInterval(intervalMs);
}

int RemoteChangeWatcher::pollingInterval() const {
    QMutexLocker locker(&m_mutex);
    return m_pollingTimer->interval();
}

void RemoteChangeWatcher::setChangeToken(const QString& token) {
    QMutexLocker locker(&m_mutex);
    m_changeToken = token;
}

QString RemoteChangeWatcher::changeToken() const {
    QMutexLocker locker(&m_mutex);
    return m_changeToken;
}

RemoteChangeWatcher::State RemoteChangeWatcher::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

void RemoteChangeWatcher::setFolderIdToPath(const QHash<QString, QString>& mapping) {
    QMutexLocker locker(&m_mutex);
    m_folderIdToPath = mapping;
}

void RemoteChangeWatcher::start() {
    QMutexLocker locker(&m_mutex);

    if (m_state == State::Running) {
        return;
    }

    m_settings = SyncSettings::load();
    if (m_settings.remotePollIntervalMs > 0) {
        m_pollingTimer->setInterval(m_settings.remotePollIntervalMs);
    }

    if (!m_driveClient) {
        emit error("Cannot start: Drive client not set");
        return;
    }

    if (m_changeToken.isEmpty()) {
        // Need to get the start page token first
        m_waitingForToken = true;
        m_changesRequestInFlight = true;
        m_pendingCheckRequested = false;
        locker.unlock();
        m_driveClient->getStartPageToken();
        return;
    }

    m_state = State::Running;
    m_changesRequestInFlight = false;
    m_pendingCheckRequested = false;
    m_pollingTimer->start();
    locker.unlock();

    emit stateChanged(State::Running);
    qInfo() << "RemoteChangeWatcher started, polling interval:" << m_pollingTimer->interval()
            << "ms";

    // Do an immediate check
    checkNow();
}

void RemoteChangeWatcher::stop() {
    QMutexLocker locker(&m_mutex);

    m_pollingTimer->stop();
    m_state = State::Stopped;
    m_waitingForToken = false;
    m_changesRequestInFlight = false;
    m_pendingCheckRequested = false;
    m_consecutiveTransientFailures = 0;
    m_pendingChanges.clear();
    m_pendingToken.clear();
    m_pendingHasMorePages = false;
    m_pendingChangeIndex = 0;
    m_batchThrottle.reset();

    locker.unlock();

    emit stateChanged(State::Stopped);
    qInfo() << "RemoteChangeWatcher stopped";
}

void RemoteChangeWatcher::clearChangeToken() {
    QMutexLocker locker(&m_mutex);
    m_changeToken.clear();
    m_folderIdToPath.clear();
    m_recentlyProcessedFileIds.clear();
    qInfo() << "RemoteChangeWatcher: change token and dedup cache cleared (account sign-out)";
}

void RemoteChangeWatcher::pause() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running) {
        return;
    }

    m_pollingTimer->stop();
    m_state = State::Paused;

    locker.unlock();

    emit stateChanged(State::Paused);
    qDebug() << "RemoteChangeWatcher paused";
}

void RemoteChangeWatcher::resume() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Paused) {
        return;
    }

    m_state = State::Running;
    m_pollingTimer->start();

    locker.unlock();

    emit stateChanged(State::Running);
    qDebug() << "RemoteChangeWatcher resumed";

    // Do an immediate check to catch any missed changes
    checkNow();
}

void RemoteChangeWatcher::reloadSettings() {
    QMutexLocker locker(&m_mutex);

    m_settings = SyncSettings::load();
    m_batchThrottle.setSettings(m_settings);
    if (m_settings.remotePollIntervalMs > 0) {
        m_pollingTimer->setInterval(m_settings.remotePollIntervalMs);
    }
}

void RemoteChangeWatcher::checkNow() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running && !m_waitingForToken) {
        return;
    }

    if (m_changesRequestInFlight) {
        m_pendingCheckRequested = true;
        qDebug() << "Remote changes request already in flight; deferring check";
        return;
    }

    if (m_changeToken.isEmpty()) {
        qDebug() << "No change token, requesting start page token";
        m_waitingForToken = true;
        m_changesRequestInFlight = true;
        locker.unlock();
        m_driveClient->getStartPageToken();
        return;
    }

    m_changesRequestInFlight = true;

    QString token = m_changeToken;
    locker.unlock();

    qDebug() << "Checking for remote changes with token:" << token;
    m_driveClient->listChanges(token);
}

void RemoteChangeWatcher::onPollingTimeout() {
    checkNow();
}

void RemoteChangeWatcher::onChangesReceived(const QList<DriveChange>& changes,
                                            const QString& newToken, bool hasMorePages) {
    qDebug() << "Received" << changes.count() << "remote changes, hasMorePages:" << hasMorePages;

    // Ignore if we're not running (e.g. in fuse-only mode, another component
    // triggered the shared GoogleDriveClient and this signal fired to us)
    {
        QMutexLocker locker(&m_mutex);
        if (m_state != State::Running) {
            m_changesRequestInFlight = false;
            m_pendingCheckRequested = false;
            m_pendingChanges.clear();
            m_pendingToken.clear();
            m_pendingHasMorePages = false;
            m_pendingChangeIndex = 0;
            return;
        }

        m_pendingChanges = changes;
        m_pendingToken = newToken;
        m_pendingHasMorePages = hasMorePages;
        m_pendingChangeIndex = 0;
    }

    if (changes.isEmpty()) {
        finalizePendingChangeBatch();
        return;
    }

    processPendingChangeBatch();
}

void RemoteChangeWatcher::processPendingChangeBatch() {
    const int entryDelayMs = m_batchThrottle.nextDelayMs();
    if (entryDelayMs > 0) {
        QTimer::singleShot(entryDelayMs, this, &RemoteChangeWatcher::processPendingChangeBatch);
        return;
    }

    int processedCount = 0;

    while (m_pendingChangeIndex < m_pendingChanges.size() &&
           processedCount < CHANGE_BATCH_SLICE_SIZE) {
        processChange(m_pendingChanges.at(m_pendingChangeIndex));
        ++m_pendingChangeIndex;
        ++processedCount;
    }

    if (m_pendingChangeIndex < m_pendingChanges.size()) {
        QTimer::singleShot(m_batchThrottle.nextDelayMs(), this,
                           &RemoteChangeWatcher::processPendingChangeBatch);
        return;
    }

    finalizePendingChangeBatch();
}

void RemoteChangeWatcher::finalizePendingChangeBatch() {
    const QString newToken = m_pendingToken;
    const bool hasMorePages = m_pendingHasMorePages;

    bool runDeferredCheck = false;
    bool tokenUpdated = false;

    // Commit the new token and clear in-flight state only after all changes
    // have been processed so late checkNow() calls are preserved.
    {
        QMutexLocker locker(&m_mutex);
        tokenUpdated = (m_changeToken != newToken);
        m_changeToken = newToken;
        m_changesRequestInFlight = false;
        runDeferredCheck = m_pendingCheckRequested;
        m_pendingCheckRequested = false;
        m_consecutiveTransientFailures = 0;
        m_pendingChanges.clear();
        m_pendingToken.clear();
        m_pendingHasMorePages = false;
        m_pendingChangeIndex = 0;
    }

    emit changeTokenUpdated(newToken);

    // If there are more pages to fetch, immediately request them
    // Otherwise, wait for the next poll interval
    if (hasMorePages && tokenUpdated) {
        qDebug() << "More pages available, fetching next page immediately";
        QTimer::singleShot(0, this, &RemoteChangeWatcher::checkNow);
    } else if (runDeferredCheck) {
        // Only run deferred check if we're caught up
        QTimer::singleShot(0, this, &RemoteChangeWatcher::checkNow);
    }
}

void RemoteChangeWatcher::onStartPageTokenReceived(const QString& token) {
    QMutexLocker locker(&m_mutex);

    // Ignore if we're not running or waiting for a token
    // (another component may have triggered the shared GoogleDriveClient)
    if (m_state == State::Stopped && !m_waitingForToken) {
        return;
    }

    m_changeToken = token;
    bool wasWaiting = m_waitingForToken;
    m_waitingForToken = false;
    m_changesRequestInFlight = false;
    m_pendingCheckRequested = false;
    m_consecutiveTransientFailures = 0;

    locker.unlock();

    emit changeTokenUpdated(token);
    qInfo() << "Received start page token:" << token;

    // If we were waiting for this token to start, now start
    if (wasWaiting) {
        start();
    }
}

void RemoteChangeWatcher::onApiError(const QString& operation, const QString& error) {
    if (operation.contains("changes", Qt::CaseInsensitive) ||
        operation.contains("token", Qt::CaseInsensitive)) {
        bool runDeferredCheck = false;
        bool shouldRetry = false;
        bool shouldSurface = false;
        int retryDelayMs = 0;
        const bool transient = isTransientPollError(error);

        {
            QMutexLocker locker(&m_mutex);
            m_changesRequestInFlight = false;
            if (m_pendingCheckRequested) {
                runDeferredCheck = true;
                m_pendingCheckRequested = false;
            }

            if (transient) {
                m_consecutiveTransientFailures += 1;
                shouldRetry = (m_state == State::Running || m_waitingForToken);
                shouldSurface = m_consecutiveTransientFailures >= kTransientFailureSurfaceThreshold;
                retryDelayMs = transientRetryDelayMs(m_consecutiveTransientFailures);
            } else {
                m_consecutiveTransientFailures = 0;
            }
        }

        if (transient) {
            qWarning() << "RemoteChangeWatcher transient API error:" << operation << error
                       << "- retrying in" << retryDelayMs << "ms";

            if (shouldSurface) {
                emit this->error("API error in " + operation + ": " + error);
            }

            if (shouldRetry) {
                QTimer::singleShot(retryDelayMs, this, &RemoteChangeWatcher::checkNow);
            }
            return;
        }

        emit this->error("API error in " + operation + ": " + error);
        qWarning() << "RemoteChangeWatcher API error:" << operation << error;

        if (runDeferredCheck && operation.contains("changes", Qt::CaseInsensitive)) {
            QTimer::singleShot(0, this, &RemoteChangeWatcher::checkNow);
        }
    }
}

void RemoteChangeWatcher::processChange(const DriveChange& change) {
    if (!m_changeQueue) {
        return;
    }

    qDebug() << "Processing change:" << change.changeId << "fileId:" << change.fileId
             << "removed:" << change.removed;

    // Deduplication: Skip if we've recently queued this file ID
    {
        QMutexLocker locker(&m_mutex);
        QDateTime now = QDateTime::currentDateTime();

        // Clean up old entries from the dedup map
        QMutableHashIterator<QString, QDateTime> it(m_recentlyProcessedFileIds);
        while (it.hasNext()) {
            it.next();
            if (it.value().secsTo(now) > DEDUP_WINDOW_SECS) {
                it.remove();
            }
        }

        // Check if this file was recently processed
        if (m_recentlyProcessedFileIds.contains(change.fileId)) {
            qDebug() << "Skipping duplicate change for fileId:" << change.fileId << "(processed"
                     << m_recentlyProcessedFileIds[change.fileId].secsTo(now) << "seconds ago)";
            return;
        }

        // Mark as recently processed
        m_recentlyProcessedFileIds[change.fileId] = now;
    }

    if (!change.removed) {
        const QString nativeDocModeOverride =
            nativeDocModeOverrideForFile(m_syncDatabase, change.fileId);
        persistObservedNativeDocState(m_syncDatabase, change.file, nativeDocModeOverride);

        // Skip files that shouldn't be processed
        if (!shouldProcess(change.file, nativeDocModeOverride)) {
            qDebug() << "Skipping file:" << change.file.name;
            return;
        }
    }

    ChangeQueueItem item;
    item.origin = ChangeOrigin::Remote;
    item.fileId = change.fileId;
    item.detectedTime = QDateTime::currentDateTime();

    // Determine change type
    if (change.removed || change.file.trashed) {
        item.changeType = ChangeType::Delete;
        if (m_syncDatabase && !change.fileId.isEmpty()) {
            m_syncDatabase->deleteNativeDocState(change.fileId);
        }
        // For deletions, we must lookup the path from the sync database
        QString path = m_syncDatabase->getLocalPath(change.fileId);
        item.localPath = path;  // May be empty if we don't have it locally
        item.isDirectory = change.file.isFolder;
        item.modifiedTime =
            change.file.modifiedTime.isValid() ? change.file.modifiedTime : change.time;

        if (!item.localPath.isEmpty() && TrashPolicy::isTrashRelativePath(item.localPath)) {
            qDebug() << "Remote change dropped (trash path):" << change.fileId << item.localPath;
            return;
        }

    } else {
        // Resolve the file path
        QString path = resolvePath(change.file);
        if (path.isEmpty()) {
            qWarning() << "Remote change dropped (unresolved path):" << change.fileId;
            return;
        }
        item.localPath = path;
        item.modifiedTime =
            change.file.modifiedTime.isValid() ? change.file.modifiedTime : change.time;
        item.isDirectory = change.file.isFolder;
        item.remoteMd5 = change.file.md5Checksum;

        // Determine if this is a create or modify
        // For remote changes, we treat it as create if we don't have it locally
        // The change processor will make the final determination
        // For now, use modify as it covers both cases
        item.changeType = ChangeType::Modify;
    }

    m_changeQueue->enqueue(item);
    emit changeDetected(change.fileId);
}

QString RemoteChangeWatcher::resolvePath(const DriveFile& file) {
    const QString parentId = file.parentId();
    if (parentId.isEmpty()) {
        // Files without a parent ID are typically shared files or files not in
        // the user's My Drive hierarchy. Return empty to signal this.
        // Caller drops changes with empty paths before enqueue.
        return QString();
    }

    const QString rootId = m_driveClient ? m_driveClient->getRootFolderId() : QString();
    QString parentPath;
    QSet<QString> folderClaims;
    bool haveParentPath = false;

    {
        QMutexLocker locker(&m_mutex);
        for (auto it = m_folderIdToPath.constBegin(); it != m_folderIdToPath.constEnd(); ++it) {
            folderClaims.insert(it.value());
        }

        if (!rootId.isEmpty() && parentId == rootId) {
            haveParentPath = true;
        } else if (m_folderIdToPath.contains(parentId)) {
            parentPath = m_folderIdToPath.value(parentId);
            haveParentPath = true;
        }
    }

    if (!haveParentPath && m_syncDatabase) {
        parentPath = m_syncDatabase->getLocalPath(parentId);
        if (!parentPath.isEmpty()) {
            haveParentPath = true;
            QMutexLocker locker(&m_mutex);
            m_folderIdToPath.insert(parentId, parentPath);
            folderClaims.insert(parentPath);
        }
    }

    if (haveParentPath) {
        const QString nativeDocModeOverride = nativeDocModeOverrideForFile(m_syncDatabase, file.id);
        const QString resolvedPath = MirrorPathResolver::resolveRemoteLocalPath(
            parentPath, file.name, file.mimeType, nativeDocModeOverride, file.id, m_syncDatabase,
            m_settings, m_settings.syncFolder, &folderClaims);
        if (TrashPolicy::isTrashRelativePath(resolvedPath)) {
            return QString();
        }
        if (file.isFolder && !resolvedPath.isEmpty()) {
            QMutexLocker locker(&m_mutex);
            m_folderIdToPath.insert(file.id, resolvedPath);
        }
        return resolvedPath;
    }

    // Parent not in mapping - attempt to resolve via Drive API
    return resolvePathFromParents(file);
}

QString RemoteChangeWatcher::resolvePathFromParents(const DriveFile& file) {
    if (!m_driveClient) {
        return QString();
    }

    QString rootId = m_driveClient->getRootFolderId();
    if (rootId.isEmpty()) {
        qWarning() << "Remote path resolution failed (root ID unavailable):" << file.id;
        return QString();
    }

    QString parentId = file.parentId();
    if (parentId.isEmpty()) {
        qWarning() << "Remote path resolution failed (missing parent):" << file.id;
        return QString();
    }

    QList<DriveFile> pathChain;
    pathChain.prepend(file);
    QSet<QString> visited;

    while (!parentId.isEmpty()) {
        if (parentId == rootId) {
            break;
        }

        if (visited.contains(parentId)) {
            qWarning() << "Remote path resolution failed (loop detected):" << file.id;
            return QString();
        }
        visited.insert(parentId);

        {
            QMutexLocker locker(&m_mutex);
            if (m_folderIdToPath.contains(parentId)) {
                const QString parentPath = m_folderIdToPath.value(parentId);
                QSet<QString> folderClaims;
                for (auto it = m_folderIdToPath.constBegin(); it != m_folderIdToPath.constEnd();
                     ++it) {
                    folderClaims.insert(it.value());
                }

                QString currentPath = parentPath;
                for (const DriveFile& node : pathChain) {
                    const QString nativeDocModeOverride =
                        nativeDocModeOverrideForFile(m_syncDatabase, node.id);
                    currentPath = MirrorPathResolver::resolveRemoteLocalPath(
                        currentPath, node.name, node.mimeType, nativeDocModeOverride, node.id,
                        m_syncDatabase, m_settings, m_settings.syncFolder, &folderClaims);
                    if (TrashPolicy::isTrashRelativePath(currentPath)) {
                        return QString();
                    }
                    if (node.isFolder) {
                        m_folderIdToPath.insert(node.id, currentPath);
                        folderClaims.insert(currentPath);
                    }
                }
                return currentPath;
            }
        }

        if (m_syncDatabase) {
            const QString parentPath = m_syncDatabase->getLocalPath(parentId);
            if (!parentPath.isEmpty()) {
                QSet<QString> folderClaims;
                {
                    QMutexLocker locker(&m_mutex);
                    m_folderIdToPath.insert(parentId, parentPath);
                    for (auto it = m_folderIdToPath.constBegin(); it != m_folderIdToPath.constEnd();
                         ++it) {
                        folderClaims.insert(it.value());
                    }
                }

                QString currentPath = parentPath;
                for (const DriveFile& node : pathChain) {
                    const QString nativeDocModeOverride =
                        nativeDocModeOverrideForFile(m_syncDatabase, node.id);
                    currentPath = MirrorPathResolver::resolveRemoteLocalPath(
                        currentPath, node.name, node.mimeType, nativeDocModeOverride, node.id,
                        m_syncDatabase, m_settings, m_settings.syncFolder, &folderClaims);
                    if (TrashPolicy::isTrashRelativePath(currentPath)) {
                        return QString();
                    }
                    if (node.isFolder) {
                        QMutexLocker locker(&m_mutex);
                        m_folderIdToPath.insert(node.id, currentPath);
                        folderClaims.insert(currentPath);
                    }
                }
                return currentPath;
            }
        }

        DriveFile parentFile = m_driveClient->getFileMetadataBlocking(parentId);
        if (!parentFile.isValid()) {
            qWarning() << "Remote path resolution failed (parent fetch error):" << parentId;
            return QString();
        }

        pathChain.prepend(parentFile);
        parentId = parentFile.parentId();
    }

    if (parentId != rootId) {
        qWarning() << "Remote path resolution failed (orphan outside root):" << file.id;
        return QString();
    }

    QSet<QString> folderClaims;
    {
        QMutexLocker locker(&m_mutex);
        for (auto it = m_folderIdToPath.constBegin(); it != m_folderIdToPath.constEnd(); ++it) {
            folderClaims.insert(it.value());
        }
    }

    QString currentPath;
    for (const DriveFile& node : pathChain) {
        const QString nativeDocModeOverride = nativeDocModeOverrideForFile(m_syncDatabase, node.id);
        currentPath = MirrorPathResolver::resolveRemoteLocalPath(
            currentPath, node.name, node.mimeType, nativeDocModeOverride, node.id, m_syncDatabase,
            m_settings, m_settings.syncFolder, &folderClaims);
        if (TrashPolicy::isTrashRelativePath(currentPath)) {
            return QString();
        }
        if (node.isFolder) {
            QMutexLocker locker(&m_mutex);
            m_folderIdToPath.insert(node.id, currentPath);
            folderClaims.insert(currentPath);
        }
    }

    return currentPath;
}

bool RemoteChangeWatcher::shouldProcess(const DriveFile& file,
                                        const QString& nativeDocModeOverride) const {
    if (FileFilter::shouldSkipRemoteFile(file, m_settings)) {
        return false;
    }

    if (!isNativeDocMimeType(file.mimeType) || file.isFolder || file.isShortcut) {
        return true;
    }

    const NativeDocRepresentation representation = effectiveNativeDocRepresentation(
        file.mimeType, nativeDocModeOverride, nativeDocModeFromString(m_settings.nativeDocMode));
    return representation.visible;
}
