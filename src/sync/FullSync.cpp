/**
 * @file FullSync.cpp
 * @brief Implementation of the Full Sync system
 */

#include "FullSync.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <vector>

#include "ChangeProcessor.h"
#include "ChangeQueue.h"
#include "FileFilter.h"
#include "SyncDatabase.h"
#include "SyncSettings.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"

FullSync::FullSync(ChangeQueue* changeQueue, SyncDatabase* database, GoogleDriveClient* driveClient,
                   ChangeProcessor* changeProcessor, QObject* parent)
    : QObject(parent),
      m_changeQueue(changeQueue),
      m_database(database),
      m_driveClient(driveClient),
      m_changeProcessor(changeProcessor),
      m_state(State::Idle),
      m_mode(Mode::Full),
      m_cancelled(false),
      m_localFileCount(0),
      m_remoteFileCount(0),
      m_orphanCount(0),
      m_remoteTree(nullptr) {
    // Connect to Drive API signals
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::filesListed, this, &FullSync::onFilesListed);
        connect(m_driveClient, &GoogleDriveClient::error, this, &FullSync::onDriveError);
    }
}

FullSync::~FullSync() {
    cancel();
    delete m_remoteTree;
}

FullSync::State FullSync::state() const { return m_state.load(); }

void FullSync::setSyncFolder(const QString& path) {
    QMutexLocker locker(&m_mutex);
    m_syncFolder = QDir(path).absolutePath();
}

QString FullSync::syncFolder() const {
    QMutexLocker locker(&m_mutex);
    return m_syncFolder;
}

int FullSync::localFileCount() const {
    QMutexLocker locker(&m_mutex);
    return m_localFileCount;
}

bool FullSync::isRunning() const {
    State s = m_state.load();
    return s == State::ScanningLocal || s == State::FetchingRemote;
}

void FullSync::fullSync() { startInternal(Mode::Full); }

void FullSync::fullSyncLocal() { startInternal(Mode::LocalOnly); }

void FullSync::start() { fullSync(); }

void FullSync::startInternal(Mode mode) {
    {
        if (isRunning()) {
            qWarning() << "FullSync already running";
            return;
        }

        QMutexLocker locker(&m_mutex);

        if (m_syncFolder.isEmpty()) {
            locker.unlock();
            emit error("Cannot start: sync folder not set");
            return;
        }

        QDir dir(m_syncFolder);
        if (!dir.exists()) {
            locker.unlock();
            emit error("Cannot start: sync folder does not exist: " + m_syncFolder);
            return;
        }

        // Reset state
        m_cancelled = false;
        m_localFileCount = 0;
        m_remoteFileCount = 0;
        m_orphanCount = 0;
        m_allRemoteItems.clear();
        m_discoveredLocalPaths.clear();
        m_discoveredLocalPaths.clear();
        m_currentPageToken.clear();

        m_state.store(State::ScanningLocal);
        m_mode = mode;
        m_settings = SyncSettings::load();

        // get true root file ID:
        if (m_mode == Mode::Full && m_driveClient) {
            ROOT_FOLDER_ID = m_driveClient->getRootFolderId();
        } else {
            ROOT_FOLDER_ID.clear();
        }
    }

    qInfo() << "FullSync started for folder:" << m_syncFolder
            << (m_mode == Mode::LocalOnly ? "(local-only)" : "");
    emit stateChanged(State::ScanningLocal);

    // Start scanning local files (use singleShot to avoid blocking)
    QTimer::singleShot(0, this, &FullSync::scanLocalFiles);
}

void FullSync::cancel() {
    if (!isRunning()) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    m_cancelled = true;
    m_state.store(State::Idle);
    locker.unlock();

    qInfo() << "FullSync cancelled";
    emit stateChanged(State::Idle);
}

void FullSync::scanLocalFiles() {
    {
        QMutexLocker locker(&m_mutex);
        if (m_cancelled) {
            return;
        }
    }

    qInfo() << "Scanning local files in:" << m_syncFolder;
    emit progressUpdated("Scanning local files...", 0, 0);

    // Iterate through all files and directories in the sync folder
    QDirIterator it(m_syncFolder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    int count = 0;
    while (it.hasNext()) {
        {
            QMutexLocker locker(&m_mutex);
            if (m_cancelled) {
                return;
            }
        }

        QString path = it.next();
        QFileInfo info(path);

        // Skip files matching ignore patterns
        QString fileName = info.fileName();
        if (shouldIgnoreFile(fileName)) {
            continue;
        }

        queueLocalFile(path, info.isDir());
        ++count;

        // Emit progress every 100 files
        if (count % 100 == 0) {
            emit progressUpdated("Scanning local files...", count, 0);
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        m_localFileCount = count;
    }

    qInfo() << "Local scan complete:" << count << "files found";
    emit progressUpdated("Local scan complete", count, count);

    if (m_mode == Mode::LocalOnly) {
        finishSync();
        return;
    }

    // Now start fetching remote files
    fetchRemoteFiles();
}

void FullSync::fetchRemoteFiles() {
    {
        QMutexLocker locker(&m_mutex);
        if (m_cancelled) {
            return;
        }
        m_state.store(State::FetchingRemote);
    }

    emit stateChanged(State::FetchingRemote);
    emit progressUpdated("Fetching remote files...", 0, 0);

    qInfo() << "Fetching remote files from Google Drive";

    // Start listing files from root
    if (m_driveClient) {
        m_driveClient->listFiles("all");
    } else {
        finishSync();
    }
}

void FullSync::onFilesListed(const QList<DriveFile>& files, const QString& nextPageToken) {
    {
        QMutexLocker locker(&m_mutex);
        if (m_cancelled || m_state != State::FetchingRemote) {
            return;
        }
    }

    processRemoteFiles(files, nextPageToken);
}

void FullSync::processRemoteFiles(const QList<DriveFile>& files, const QString& nextPageToken) {
    for (const DriveFile& file : files) {
        {
            QMutexLocker locker(&m_mutex);
            if (m_cancelled) {
                return;
            }
        }

        // Skip files that shouldn't be synced
        if (shouldSkipRemoteFile(file)) {
            continue;
        }

        // Store remote files to generate structure after all are fetched
        m_allRemoteItems.append(file);
        ++m_remoteFileCount;
    }

    emit progressUpdated("Fetching remote files...", m_allRemoteItems.length(), 0);

    // Check if there are more pages
    if (!nextPageToken.isEmpty() && !m_cancelled) {
        // Continue listing from next page
        if (m_driveClient) {
            m_driveClient->listFiles("all", nextPageToken);
        }
    } else {
        // All remote files have been fetched
        buildRemoteFolderStructure();  // Will also queue remote files for processing
        finishSync();
    }
}

void FullSync::onDriveError(const QString& operation, const QString& errorMsg) {
    // Only handle errors if we're in the fetching state
    QMutexLocker locker(&m_mutex);
    if (m_state.load() != State::FetchingRemote) {
        return;
    }

    // Check if this is a listFiles error
    if (operation.contains("list", Qt::CaseInsensitive)) {
        m_state.store(State::Error);
        locker.unlock();

        qWarning() << "FullSync error during remote fetch:" << errorMsg;
        emit stateChanged(State::Error);
        emit error("Failed to fetch remote files: " + errorMsg);
    }
}

void FullSync::queueLocalFile(const QString& absolutePath, bool isDirectory) {
    if (!m_changeQueue) {
        return;
    }

    QString relativePath = getRelativePath(absolutePath);
    if (relativePath.isEmpty()) {
        return;
    }

    // Track this path to avoid duplicates when processing remote files
    m_discoveredLocalPaths.insert(relativePath);

    QFileInfo info(absolutePath);

    ChangeQueueItem item;
    item.changeType = ChangeType::Modify;  // Treat as create for initial sync
    item.origin = ChangeOrigin::Local;
    item.localPath = relativePath;
    item.detectedTime = QDateTime::currentDateTime();
    item.modifiedTime = info.lastModified();
    item.isDirectory = isDirectory;

    // Try to get file ID from database if we have synced this file before
    if (m_database) {
        item.fileId = m_database->getFileId(relativePath);
    }

    m_changeQueue->enqueue(item);
}

void FullSync::queueRemoteFile(const DriveFile& file, const QString& localPath) {
    if (!m_changeQueue) {
        return;
    }

    // Check if this file was already queued from local scan
    // The ChangeProcessor will handle conflict detection
    if (m_discoveredLocalPaths.contains(localPath)) {
        // File exists both locally and remotely - still queue it
        // The conflict resolver will determine what action (if any) is needed
        qDebug() << "File exists both locally and remotely:" << localPath;
    }

    ChangeQueueItem item;
    item.changeType = ChangeType::Modify;  // Treat as create for initial sync
    item.origin = ChangeOrigin::Remote;
    item.localPath = localPath;
    item.fileId = file.id;
    item.detectedTime = QDateTime::currentDateTime();
    item.modifiedTime = file.modifiedTime;
    item.isDirectory = file.isFolder;

    m_changeQueue->enqueue(item);
}

void FullSync::buildRemoteFolderStructure() {
    // Build a tree node struture

    // Create root node
    m_remoteTree = new FileTreeNode();
    m_remoteTree->name = "";
    m_remoteTree->relativePath = "";
    m_remoteTree->isFolder = true;
    m_remoteTree->fileId = ROOT_FOLDER_ID;

    unsigned long iterations = 0;
    std::vector<FileTreeNode*> currentBranchDepthParents;
    currentBranchDepthParents.push_back(
        m_remoteTree);  // list of parent IDs at current depth to reduce search time

    unsigned long maxIterations =
        m_allRemoteItems.size() * 10;  // arbitrary limit to avoid infinite loops
    while (!m_allRemoteItems.isEmpty() && iterations < maxIterations) {
        // store all items added this iteration to remove them after
        QList<qsizetype> itemsToRemove;
        for (qsizetype i = 0; i < m_allRemoteItems.size(); ++i) {
            const DriveFile& file = m_allRemoteItems[i];
            // Try all current depth to match parent
            for (unsigned long j = 0; j < currentBranchDepthParents.size(); j++) {
                if (file.parents.contains(currentBranchDepthParents[j]->fileId)) {
                    // Found parent - add to tree
                    FileTreeNode* parentNode = currentBranchDepthParents[j];
                    FileTreeNode* newNode = new FileTreeNode();
                    newNode->name = file.name;
                    newNode->isFolder = file.isFolder;
                    newNode->relativePath = parentNode->relativePath.isEmpty()
                                                ? file.name
                                                : parentNode->relativePath + "/" + file.name;
                    newNode->modifiedTime = file.modifiedTime;
                    newNode->fileId = file.id;
                    parentNode->children.insert(file.id, newNode);

                    // Queue this remote file for processing
                    queueRemoteFile(file, newNode->relativePath);

                    itemsToRemove.append(i);
                    break;
                }
            }
        }
        // Update current depth parents
        std::vector<FileTreeNode*> nextDepthParents;
        for (FileTreeNode* node : currentBranchDepthParents) {
            for (FileTreeNode* childNode : node->children) {
                // if (childNode->isFolder) {
                nextDepthParents.push_back(childNode);
                //}
            }
        }
        currentBranchDepthParents = nextDepthParents;
        iterations++;
        // Remove processed items
        // Remove from back to avoid invalidating indices
        std::sort(itemsToRemove.begin(), itemsToRemove.end(), std::greater<qsizetype>());
        for (qsizetype index : itemsToRemove) {
            m_allRemoteItems.removeAt(index);
        }

        if (iterations > 15 && currentBranchDepthParents.empty()) {
            qWarning() << "FullSync: Stuck building remote folder structure, remaining items:"
                       << m_allRemoteItems.size();
            break;
        }
    }

    m_orphanCount = m_allRemoteItems.size();

    // Clean up we are done with tree.
    delete m_remoteTree;
    m_remoteTree = nullptr;
}

QString FullSync::getRelativePath(const QString& absolutePath) const {
    if (absolutePath.startsWith(m_syncFolder)) {
        QString relative = absolutePath.mid(m_syncFolder.length());
        if (relative.startsWith('/')) {
            relative = relative.mid(1);
        }
        return relative;
    }
    // File is outside sync folder - return empty to indicate error
    qWarning() << "File outside sync folder:" << absolutePath;
    return QString();
}

void FullSync::finishSync() {
    int localCount, remoteCount;
    {
        QMutexLocker locker(&m_mutex);
        if (m_cancelled) {
            return;
        }
        m_state.store(State::Complete);
        localCount = m_localFileCount;
        remoteCount = m_remoteFileCount;

        // now clean up potentially long lists, we are done with it.
        m_allRemoteItems.clear();
        m_discoveredLocalPaths.clear();
    }

    // TODO: remaining remote count items are ignored orphans. Change what is reported here
    qInfo() << "FullSync complete: local=" << localCount << "remote=" << remoteCount;
    if (m_orphanCount > 0) {
        qInfo() << "FullSync ignored orphaned remote items:" << m_orphanCount;
    }

    emit stateChanged(State::Complete);
    emit progressUpdated("Full sync complete", localCount + remoteCount, localCount + remoteCount);
    emit completed(localCount, remoteCount);

    // Note: The ChangeProcessor remains in initial sync mode until all queued
    // items are processed. The initial sync mode causes the conflict resolver
    // to skip files that haven't changed since last sync, which is important
    // for performance on large drives. The ChangeProcessor should clear this
    // mode automatically when the queue is empty, or callers can connect to
    // the completed() signal and manually clear it via setInitialSync(false)
    // if they need more control.
}

bool FullSync::shouldIgnoreFile(const QString& fileName) const {
    // Check against each ignore pattern
    for (const QString& pattern : m_settings.ignorePatterns) {
        // Simple pattern matching
        if (pattern.startsWith("*.")) {
            // Extension match (e.g., "*.tmp")
            QString suffix = pattern.mid(1);  // Get ".tmp"
            if (fileName.endsWith(suffix, Qt::CaseInsensitive)) {
                return true;
            }
        } else if (pattern.endsWith("*")) {
            // Prefix match (e.g., ".*")
            QString prefix = pattern.left(pattern.length() - 1);
            if (fileName.startsWith(prefix)) {
                return true;
            }
        } else {
            // Exact match
            if (fileName == pattern) {
                return true;
            }
        }
    }
    return false;
}

bool FullSync::shouldSkipRemoteFile(const DriveFile& file) const {
    return FileFilter::shouldSkipRemoteFile(file, m_settings);
}