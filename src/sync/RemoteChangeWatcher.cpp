/**
 * @file RemoteChangeWatcher.cpp
 * @brief Implementation of remote Google Drive change watcher
 */

#include "RemoteChangeWatcher.h"

#include <QDebug>
#include <QMutexLocker>
#include <QSet>

#include "ChangeQueue.h"
#include "FileFilter.h"
#include "SyncDatabase.h"
#include "SyncSettings.h"
#include "api/DriveChange.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"

const int RemoteChangeWatcher::DEFAULT_POLL_INTERVAL_MS;

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

RemoteChangeWatcher::~RemoteChangeWatcher() { stop(); }

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

    locker.unlock();

    emit stateChanged(State::Stopped);
    qInfo() << "RemoteChangeWatcher stopped";
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

void RemoteChangeWatcher::checkNow() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running) {
        return;
    }

    if (m_changeToken.isEmpty()) {
        qDebug() << "No change token, requesting start page token";
        m_waitingForToken = true;
        locker.unlock();
        m_driveClient->getStartPageToken();
        return;
    }

    if (m_changesRequestInFlight) {
        m_pendingCheckRequested = true;
        qDebug() << "Remote changes request already in flight; deferring check";
        return;
    }

    m_changesRequestInFlight = true;

    QString token = m_changeToken;
    locker.unlock();

    qDebug() << "Checking for remote changes with token:" << token;
    m_driveClient->listChanges(token);
}

void RemoteChangeWatcher::onPollingTimeout() { checkNow(); }

void RemoteChangeWatcher::onChangesReceived(const QList<DriveChange>& changes,
                                            const QString& newToken, bool hasMorePages) {
    qDebug() << "Received" << changes.count() << "remote changes, hasMorePages:" << hasMorePages;

    // Ignore if we're not running (e.g. in fuse-only mode, another component
    // triggered the shared GoogleDriveClient and this signal fired to us)
    {
        QMutexLocker locker(&m_mutex);
        if (m_state != State::Running) {
            return;
        }
    }

    // Mark the request as no longer in flight, but defer token update
    // until after processing to avoid advancing past unprocessed changes
    // if a crash occurs mid-processing.
    QMutexLocker locker(&m_mutex);
    const bool runDeferredCheck = m_pendingCheckRequested;
    const bool tokenUpdated = (m_changeToken != newToken);
    m_pendingCheckRequested = false;
    locker.unlock();

    // Process each change
    for (const DriveChange& change : changes) {
        processChange(change);
    }

    // Commit the new token only after all changes have been processed
    {
        QMutexLocker locker2(&m_mutex);
        m_changeToken = newToken;
    }

    emit changeTokenUpdated(newToken);

    // Mark request as completed after processing to allow deferred checks to run if needed
    m_changesRequestInFlight = false;

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
        {
            QMutexLocker locker(&m_mutex);
            m_changesRequestInFlight = false;
            if (m_pendingCheckRequested) {
                runDeferredCheck = true;
                m_pendingCheckRequested = false;
            }
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

    // Skip files that shouldn't be processed
    if (!change.removed && !shouldProcess(change.file)) {
        qDebug() << "Skipping file:" << change.file.name;
        return;
    }

    ChangeQueueItem item;
    item.origin = ChangeOrigin::Remote;
    item.fileId = change.fileId;
    item.detectedTime = QDateTime::currentDateTime();

    // Determine change type
    if (change.removed || change.file.trashed) {
        item.changeType = ChangeType::Delete;
        // For deletions, we must lookup the path from the sync database
        QString path = m_syncDatabase->getLocalPath(change.fileId);
        item.localPath = path;  // May be empty if we don't have it locally
        item.isDirectory = change.file.isFolder;
        item.modifiedTime =
            change.file.modifiedTime.isValid() ? change.file.modifiedTime : change.time;

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
    QMutexLocker locker(&m_mutex);

    QString parentId = file.parentId();
    if (parentId.isEmpty()) {
        // Files without a parent ID are typically shared files or files not in
        // the user's My Drive hierarchy. Return empty to signal this.
        // The change processor should skip files with empty paths.
        // TODO: Validate that change processor handles this correctly.
        return QString();
    }

    // Look up parent path in the folder ID to path mapping
    if (m_folderIdToPath.contains(parentId)) {
        QString parentPath = m_folderIdToPath.value(parentId);
        if (parentPath.isEmpty()) {
            // Parent is the root folder (empty path = My Drive root)
            return file.name;
        }
        return parentPath + "/" + file.name;
    }

    locker.unlock();

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

    QStringList pathParts;
    pathParts.append(file.name);
    QList<QPair<QString, QString>> parentChain;
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
                QString parentPath = m_folderIdToPath.value(parentId);
                QString tail = pathParts.join("/");
                if (parentPath.isEmpty()) {
                    return tail;
                }
                return parentPath + "/" + tail;
            }
        }

        DriveFile parentFile = m_driveClient->getFileMetadataBlocking(parentId);
        if (!parentFile.isValid()) {
            qWarning() << "Remote path resolution failed (parent fetch error):" << parentId;
            return QString();
        }

        parentChain.append(qMakePair(parentFile.id, parentFile.name));
        pathParts.prepend(parentFile.name);
        parentId = parentFile.parentId();
    }

    if (parentId != rootId) {
        qWarning() << "Remote path resolution failed (orphan outside root):" << file.id;
        return QString();
    }

    QString resolvedPath = pathParts.join("/");

    if (!parentChain.isEmpty()) {
        QString currentPath;
        QMutexLocker locker(&m_mutex);
        for (int i = parentChain.size() - 1; i >= 0; --i) {
            const auto& entry = parentChain[i];
            if (currentPath.isEmpty()) {
                currentPath = entry.second;
            } else {
                currentPath += "/" + entry.second;
            }
            m_folderIdToPath.insert(entry.first, currentPath);
        }
    }

    return resolvedPath;
}

bool RemoteChangeWatcher::shouldProcess(const DriveFile& file) const {
    return !FileFilter::shouldSkipRemoteFile(file, m_settings);
}
