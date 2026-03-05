/**
 * @file SyncActionThread.cpp
 * @brief Implementation of the Sync Action Thread
 */

#include "SyncActionThread.h"

// Hopefully windows and linux compatible...
#include <sys/types.h>
#include <utime.h>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTimer>
#include <QtGlobal>

#include "ChangeProcessor.h"
#include "LocalChangeWatcher.h"
#include "SyncActionQueue.h"
#include "SyncDatabase.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "utils/FileInUseChecker.h"

SyncActionThread::SyncActionThread(SyncActionQueue* actionQueue, SyncDatabase* database, GoogleDriveClient* driveClient,
                                   ChangeProcessor* changeProcessor, LocalChangeWatcher* localWatcher, QObject* parent)
    : QObject(parent),
      m_actionQueue(actionQueue),
      m_database(database),
      m_driveClient(driveClient),
      m_changeProcessor(changeProcessor),
      m_localWatcher(localWatcher),
      m_state(State::Stopped),
      m_processingActive(false) {
    // Connect to Sync Action Queue's itemsAvailable signal (Jobs Available Wakeup)
    if (m_actionQueue) {
        connect(m_actionQueue, &SyncActionQueue::itemsAvailable, this, &SyncActionThread::onItemsAvailable);
    }

    // Connect Drive API response signals
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::fileUploaded, this, &SyncActionThread::onFileUploaded);
        connect(m_driveClient, &GoogleDriveClient::fileUploadedDetailed, this,
                &SyncActionThread::onFileUploadedDetailed);
        connect(m_driveClient, &GoogleDriveClient::fileUpdated, this, &SyncActionThread::onFileUpdated);
        connect(m_driveClient, &GoogleDriveClient::fileDownloaded, this, &SyncActionThread::onFileDownloaded);
        connect(m_driveClient, &GoogleDriveClient::fileMoved, this, &SyncActionThread::onFileMoved);
        connect(m_driveClient, &GoogleDriveClient::fileMovedDetailed, this, &SyncActionThread::onFileMovedDetailed);
        connect(m_driveClient, &GoogleDriveClient::fileRenamed, this, &SyncActionThread::onFileRenamed);
        connect(m_driveClient, &GoogleDriveClient::fileRenamedDetailed, this, &SyncActionThread::onFileRenamedDetailed);
        connect(m_driveClient, &GoogleDriveClient::fileDeleted, this, &SyncActionThread::onFileDeleted);
        connect(m_driveClient, &GoogleDriveClient::folderCreatedDetailed, this,
                &SyncActionThread::onFolderCreatedDetailed);
        connect(m_driveClient, &GoogleDriveClient::error, this, &SyncActionThread::onDriveError);
        connect(m_driveClient, &GoogleDriveClient::errorDetailed, this, &SyncActionThread::onDriveErrorDetailed);
    }
}

SyncActionThread::~SyncActionThread() { stop(); }

SyncActionThread::State SyncActionThread::state() const {
    QMutexLocker locker(&m_stateMutex);
    return m_state;
}

void SyncActionThread::setSyncFolder(const QString& path) {
    m_syncFolder = path;
    qDebug() << "SyncActionThread sync folder set to:" << path;
}

QString SyncActionThread::syncFolder() const { return m_syncFolder; }

void SyncActionThread::start() {
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state == State::Running) {
            qDebug() << "SyncActionThread already running";
            return;
        }
        m_state = State::Running;
    }

    m_processingActive = true;
    emit stateChanged(State::Running);

    qInfo() << "SyncActionThread started";

    // Start processing immediately if there are items
    QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
}

void SyncActionThread::stop() {
    {
        QMutexLocker locker(&m_stateMutex);
        m_state = State::Stopped;
    }

    m_processingActive = false;

    // Wake up the queue in case we're waiting
    if (m_actionQueue) {
        m_actionQueue->wakeAll();
    }

    emit stateChanged(State::Stopped);

    qInfo() << "SyncActionThread stopped";
}

void SyncActionThread::clearInProgressActions() {
    {
        QMutexLocker locker(&m_driveActionsMutex);
        m_driveActionsInProgress.clear();
    }
    m_retryCounts.clear();
    m_lastTokenRefreshRequestMs = 0;
    qInfo() << "SyncActionThread: in-progress actions and retry state cleared (account sign-out)";
}

void SyncActionThread::pause() {
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state != State::Running) {
            return;
        }
        m_state = State::Paused;
    }

    m_processingActive = false;
    emit stateChanged(State::Paused);

    qInfo() << "SyncActionThread paused";
}

void SyncActionThread::resume() {
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state != State::Paused) {
            return;
        }
        m_state = State::Running;
    }

    m_processingActive = true;
    emit stateChanged(State::Running);

    qInfo() << "SyncActionThread resumed";

    // Resume processing
    QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
}

void SyncActionThread::onItemsAvailable() {
    // Jobs Available Wakeup Signal/Slot handler
    qDebug() << "SyncActionThread received items available signal";

    QMutexLocker locker(&m_stateMutex);
    if (m_state == State::Running && !m_processingActive) {
        m_processingActive = true;
        QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
    }
}

QString SyncActionThread::actionKeyForFileId(const QString& fileId) const {
    if (fileId.isEmpty()) {
        return QString();
    }
    return QString("id:%1").arg(fileId);
}

QString SyncActionThread::actionKeyForLocalPath(const QString& localPath) const {
    QString normalized = normalizeLocalPath(localPath);
    if (normalized.isEmpty()) {
        return QString();
    }
    return QString("path:%1").arg(normalized);
}

QString SyncActionThread::actionKeyForItem(const SyncActionItem& item) const {
    if (!item.fileId.isEmpty()) {
        return actionKeyForFileId(item.fileId);
    }
    return actionKeyForLocalPath(item.localPath);
}

QString SyncActionThread::normalizeLocalPath(const QString& path) const {
    if (path.isEmpty()) {
        return QString();
    }
    if (!QDir::isAbsolutePath(path)) {
        return path;
    }
    if (m_syncFolder.isEmpty()) {
        return path;
    }
    QString relative = QDir(m_syncFolder).relativeFilePath(path);
    if (relative.startsWith("..")) {
        return path;
    }
    return relative;
}

void SyncActionThread::trackActionInProgress(const SyncActionItem& item) {
    QString key = actionKeyForItem(item);
    if (key.isEmpty()) {
        return;
    }
    QMutexLocker locker(&m_driveActionsMutex);
    m_driveActionsInProgress.insert(key, item);
}

void SyncActionThread::untrackActionInProgress(const SyncActionItem& item) {
    QMutexLocker locker(&m_driveActionsMutex);
    QString fileKey = actionKeyForFileId(item.fileId);
    if (!fileKey.isEmpty()) {
        m_driveActionsInProgress.remove(fileKey);
    }
    QString pathKey = actionKeyForLocalPath(item.localPath);
    if (!pathKey.isEmpty()) {
        m_driveActionsInProgress.remove(pathKey);
    }
}

void SyncActionThread::processNextAction() {
    // Check if we should continue processing
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state != State::Running) {
            m_processingActive = false;
            return;
        }
    }

    // Check for items in the queue
    if (!m_actionQueue || m_actionQueue->isEmpty()) {
        m_processingActive = false;
        qDebug() << "SyncActionThread idle - waiting for itemsAvailable signal";
        return;
    }

    // Dequeue the next action
    SyncActionItem action = m_actionQueue->dequeue();
    if (!action.isValid()) {
        // Invalid item, continue to next
        QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
        return;
    }

    if (action.actionType == SyncActionType::Download && !action.localPath.isEmpty()) {
        action.localPath = resolveUniqueLocalPath(action.localPath, action.fileId, QString(), true);
    }

    qDebug() << "Processing action:" << static_cast<int>(action.actionType) << "path:" << action.localPath
             << "fileId:" << action.fileId;

    // Store current action for async response handling
    trackActionInProgress(action);

    // Mark file as "in operation" (with mutex) per flow chart
    if (m_changeProcessor && !action.localPath.isEmpty()) {
        m_changeProcessor->markFileInOperation(action.localPath);
    }

    // Create a new database entry if it doesn't exist
    if (m_database && !action.localPath.isEmpty()) {
        FileSyncState existingState = m_database->getFileState(action.localPath);
        if (existingState.fileId.isEmpty() && !action.fileId.isEmpty()) {
            FileSyncState newState;
            newState.localPath = action.localPath;
            newState.fileId = action.fileId;
            newState.isFolder = action.isFolder;
            newState.modifiedTimeAtSync = action.modifiedTime;
            m_database->saveFileState(newState);
        }
    }

    // Execute the action based on type
    switch (action.actionType) {
        case SyncActionType::Upload:
            executeUpload(action);
            break;
        case SyncActionType::Download:
            executeDownload(action);
            break;
        case SyncActionType::DeleteLocal:
            executeDeleteLocal(action);
            break;
        case SyncActionType::DeleteRemote:
            executeDeleteRemote(action);
            break;
        case SyncActionType::MoveLocal:
            executeMoveLocal(action);
            break;
        case SyncActionType::MoveRemote:
            executeMoveRemote(action);
            break;
        case SyncActionType::RenameLocal:
            executeRenameLocal(action);
            break;
        case SyncActionType::RenameRemote:
            executeRenameRemote(action);
            break;
    }

    // If in-progress actions map is under size limit, process next action immediately
    {
        QMutexLocker locker(&m_driveActionsMutex);
        if (m_driveActionsInProgress.size() < MAX_CONCURRENT_DRIVE_ACTIONS) {
            QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
        }
    }
}

void SyncActionThread::executeUpload(const SyncActionItem& item) {
    QString absolutePath = toAbsolutePath(item.localPath);

    QFileInfo fileInfo(absolutePath);
    if (!fileInfo.exists()) {
        failAction(item, "Local file does not exist: " + absolutePath);
        return;
    }

    // Check if the file is still being written to by another process.
    // If so, re-enqueue to the back of the queue so other ready items
    // can proceed, and this file gets re-checked on its next turn.
    if (!item.isFolder && fileInfo.isFile() && FileInUseChecker::isFileOpenForWriting(absolutePath)) {
        qInfo() << "Upload deferred — file is open for writing by another process:" << item.localPath;

        // Untrack so the item can be re-processed
        untrackActionInProgress(item);
        if (m_changeProcessor && !item.localPath.isEmpty()) {
            m_changeProcessor->unmarkFileInOperation(item.localPath);
        }

        // Re-enqueue after a short delay so we don't spin
        QTimer::singleShot(3000, this, [this, item]() {
            if (m_actionQueue) {
                m_actionQueue->enqueueIfNotDuplicate(item);
            }
        });

        // Continue processing other items immediately
        QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
        return;
    }

    // If folder, create remote folder instead of uploading
    if (item.isFolder) {
        QString existingFolderId = item.fileId;
        if (existingFolderId.isEmpty() && m_database) {
            existingFolderId = m_database->getFileId(item.localPath);
        }

        if (!existingFolderId.isEmpty()) {
            qInfo() << "Skipping remote folder create - mapping already exists:" << item.localPath
                    << "fileId:" << existingFolderId;
            updateDatabaseAfterAction(item, existingFolderId,
                                      item.modifiedTime.isValid() ? item.modifiedTime : QDateTime::currentDateTimeUtc(),
                                      QString(), QString());
            completeAction(item);
            return;
        }

        qInfo() << "Creating remote folder for local folder:" << item.localPath;
        QString parentPath = QFileInfo(item.localPath).path();
        QString parentId;
        if (!resolveRemoteParentId(parentPath, parentId)) {
            if (deferUntilRemoteParentReady(parentPath, item)) {
                return;
            }
            failAction(item, "Failed to resolve remote parent folder: " + parentPath);
            return;
        }
        m_driveClient->createFolder(fileInfo.fileName(), parentId, item.localPath);
        return;
    }

    qInfo() << "Uploading:" << item.localPath;

    if (!item.fileId.isEmpty()) {
        // Update existing file
        m_driveClient->updateFile(item.fileId, absolutePath);
    } else {
        // New file upload - need to determine parent folder
        QString parentPath = QFileInfo(item.localPath).path();
        QString parentId;
        if (!resolveRemoteParentId(parentPath, parentId)) {
            if (deferUntilRemoteParentReady(parentPath, item)) {
                return;
            }
            failAction(item, "Failed to resolve remote parent folder: " + parentPath);
            return;
        }

        m_driveClient->uploadFile(absolutePath, parentId, QString());
    }
}

void SyncActionThread::executeDownload(const SyncActionItem& item) {
    if (item.fileId.isEmpty()) {
        failAction(item, "Cannot download: no file ID provided");
        return;
    }

    QString absolutePath = toAbsolutePath(item.localPath);

    // If folder, create it instead of downloading
    if (item.isFolder) {
        QDir dir;
        if (!dir.mkpath(absolutePath)) {
            failAction(item, "Failed to create local folder: " + absolutePath);
            return;
        }
        // Notify local watcher to watch newly created directory
        if (m_localWatcher) {
            qDebug() << "Notifying local watcher to watch new directory:" << absolutePath;
            m_localWatcher->watchDirectory(absolutePath);
        }
        updateDatabaseAfterAction(item, item.fileId, item.modifiedTime);
        qInfo() << "Created local folder:" << item.localPath;
        completeAction(item);
        return;
    }

    // Ensure parent directory exists
    QFileInfo fileInfo(absolutePath);
    QDir parentDir = fileInfo.dir();
    if (!parentDir.exists()) {
        if (!parentDir.mkpath(".")) {
            failAction(item, "Failed to create directory: " + parentDir.path());
            return;
        }
        // Notify local watcher to watch newly created parent directory
        if (m_localWatcher) {
            qDebug() << "Notifying local watcher to watch new parent directory:" << parentDir.absolutePath();
            m_localWatcher->watchDirectory(parentDir.absolutePath());
        }
    }

    qInfo() << "Downloading:" << item.fileId << "to" << item.localPath;

    m_driveClient->downloadFile(item.fileId, absolutePath);
}

void SyncActionThread::executeDeleteLocal(const SyncActionItem& item) {
    qInfo() << "Deleting local:" << item.localPath;

    const bool success = deleteLocalPathRecursive(item.localPath, item.fileId);

    if (success) {
        completeAction(item);
    } else {
        failAction(item, "Failed to delete local file: " + toAbsolutePath(item.localPath));
    }
}

bool SyncActionThread::deleteLocalPathRecursive(const QString& localPath, const QString& fileId) {
    QString absolutePath = toAbsolutePath(localPath);

    qInfo() << "Deleting local:" << localPath;

    QFileInfo fileInfo(absolutePath);
    bool success = false;

    if (fileInfo.isDir()) {
        QDir dir(absolutePath);
        // If recursive delete files, build a list of all db entries to mark deleted
        QStringList recursiveDeleteChildren = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        // Delete children first
        for (const QString& child : recursiveDeleteChildren) {
            QString childRelPath = QDir(localPath).filePath(child);
            if (!deleteLocalPathRecursive(childRelPath)) {
                return false;
            }
        }

        // Notify local watcher to stop watching this directory before deletion
        if (m_localWatcher) {
            qDebug() << "Notifying local watcher to unwatch directory:" << absolutePath;
            m_localWatcher->unwatchDirectory(absolutePath);
        }

        success = dir.removeRecursively();
    } else {
        success = QFile::remove(absolutePath);
    }

    if (success) {
        // Mark in database as deleted
        if (m_database) {
            const QString effectiveFileId = fileId.isEmpty() ? m_database->getFileId(localPath) : fileId;
            m_database->markFileDeleted(localPath, effectiveFileId);
        }
        return true;
    } else {
        return false;
    }
}

void SyncActionThread::executeDeleteRemote(const SyncActionItem& item) {
    if (item.fileId.isEmpty()) {
        // Try to get file ID from database
        QString fileId;
        if (m_database) {
            fileId = m_database->getFileId(item.localPath);
        }

        if (fileId.isEmpty()) {
            failAction(item, "Cannot delete remote: no file ID available");
            return;
        }

        // Create a modified action with the file ID for tracking
        SyncActionItem modifiedItem = item;
        modifiedItem.fileId = fileId;
        untrackActionInProgress(item);
        trackActionInProgress(modifiedItem);

        qInfo() << "Deleting remote:" << fileId;
        m_driveClient->deleteFile(fileId);
    } else {
        qInfo() << "Deleting remote:" << item.fileId;
        m_driveClient->deleteFile(item.fileId);
    }
}

void SyncActionThread::executeMoveLocal(const SyncActionItem& item) {
    QString sourceAbsPath = toAbsolutePath(item.localPath);
    QString effectiveFileId = item.fileId;
    if (effectiveFileId.isEmpty() && m_database) {
        effectiveFileId = m_database->getFileId(item.localPath);
    }
    QString resolvedMoveDestination =
        resolveUniqueLocalPath(item.moveDestination, effectiveFileId, item.localPath, false);
    QString destAbsPath = toAbsolutePath(resolvedMoveDestination);

    qInfo() << "Moving local:" << item.localPath << "to" << resolvedMoveDestination;

    // Ensure destination directory exists
    QFileInfo destInfo(destAbsPath);
    QDir destDir = destInfo.dir();
    if (!destDir.exists()) {
        if (!destDir.mkpath(".")) {
            failAction(item, "Failed to create destination directory: " + destDir.path());
            return;
        }
        // Notify local watcher to watch newly created destination directory
        if (m_localWatcher) {
            qDebug() << "Notifying local watcher to watch new destination directory:" << destDir.absolutePath();
            m_localWatcher->watchDirectory(destDir.absolutePath());
        }
    }

    // Check if we're moving a directory
    QFileInfo sourceInfo(sourceAbsPath);
    bool isDirectory = sourceInfo.isDir();

    // Perform the move
    bool success = QFile::rename(sourceAbsPath, destAbsPath);

    if (success) {
        // If a directory was moved, update watcher to track the new location
        if (m_localWatcher && isDirectory) {
            qDebug() << "Directory moved, updating watcher: unwatch" << sourceAbsPath << "and watch" << destAbsPath;
            m_localWatcher->unwatchDirectory(sourceAbsPath);
            m_localWatcher->watchDirectory(destAbsPath);
        }

        // Update database with new path
        if (m_database) {
            QString dbFileId = effectiveFileId;
            if (dbFileId.isEmpty()) {
                dbFileId = m_database->getFileId(item.localPath);
            }
            if (!dbFileId.isEmpty()) {
                m_database->setLocalPath(dbFileId, resolvedMoveDestination);
            }
        }
        SyncActionItem completedItem = item;
        if (completedItem.fileId.isEmpty()) {
            completedItem.fileId = effectiveFileId;
        }
        completedItem.moveDestination = resolvedMoveDestination;
        completeAction(completedItem);
    } else {
        failAction(item, "Failed to move local file from " + sourceAbsPath + " to " + destAbsPath);
    }
}

void SyncActionThread::executeMoveRemote(const SyncActionItem& item) {
    if (item.fileId.isEmpty()) {
        failAction(item, "Cannot move remote: no file ID provided");
        return;
    }

    qInfo() << "Moving remote:" << item.fileId << "to" << item.moveDestination;

    // Get current parent ID
    QJsonArray parents = m_driveClient->getParentsByFileId(item.fileId);
    if (parents.isEmpty()) {
        failAction(item, "Failed to get current parent for remote file");
        return;
    }
    QString oldParentId = parents.first().toString();

    // Get new parent ID (moveDestination is a parent folder path for remote moves)
    QString newParentId;
    if (!resolveRemoteParentId(item.moveDestination, newParentId)) {
        if (deferUntilRemoteParentReady(item.moveDestination, item)) {
            return;
        }
        failAction(item, "Failed to resolve remote move destination: " + item.moveDestination);
        return;
    }

    m_driveClient->moveFile(item.fileId, newParentId, oldParentId);
}

void SyncActionThread::executeRenameLocal(const SyncActionItem& item) {
    QString absolutePath = toAbsolutePath(item.localPath);

    qInfo() << "Renaming local:" << item.localPath << "to" << item.renameTo;

    QFileInfo fileInfo(absolutePath);
    QString newRelPath = QFileInfo(item.localPath).dir().filePath(item.renameTo);
    QString effectiveFileId = item.fileId;
    if (effectiveFileId.isEmpty() && m_database) {
        effectiveFileId = m_database->getFileId(item.localPath);
    }
    QString resolvedRelPath = resolveUniqueLocalPath(newRelPath, effectiveFileId, item.localPath, false);
    QString newAbsPath = toAbsolutePath(resolvedRelPath);

    // Perform the rename
    bool success = QFile::rename(absolutePath, newAbsPath);

    if (success) {
        // Update database with new path
        if (m_database) {
            QString dbFileId = effectiveFileId;
            if (dbFileId.isEmpty()) {
                dbFileId = m_database->getFileId(item.localPath);
            }
            if (!dbFileId.isEmpty()) {
                m_database->setLocalPath(dbFileId, resolvedRelPath);
            }
        }
        SyncActionItem completedItem = item;
        if (completedItem.fileId.isEmpty()) {
            completedItem.fileId = effectiveFileId;
        }
        completedItem.renameTo = QFileInfo(resolvedRelPath).fileName();
        completeAction(completedItem);
    } else {
        failAction(item, "Failed to rename local file from " + absolutePath + " to " + newAbsPath);
    }
}

void SyncActionThread::executeRenameRemote(const SyncActionItem& item) {
    if (item.fileId.isEmpty()) {
        failAction(item, "Cannot rename remote: no file ID provided");
        return;
    }

    qInfo() << "Renaming remote:" << item.fileId << "to" << item.renameTo;

    // Use the renameFile method to update just the name metadata
    m_driveClient->renameFile(item.fileId, item.renameTo);
}

QString SyncActionThread::toAbsolutePath(const QString& relativePath) const {
    if (QDir::isAbsolutePath(relativePath)) {
        return relativePath;
    }
    return QDir(m_syncFolder).filePath(relativePath);
}

QString SyncActionThread::resolveUniqueLocalPath(const QString& desiredLocalPath, const QString& fileId,
                                                 const QString& currentLocalPath, bool reuseExistingMapping) const {
    QString normalizedPath = QDir::cleanPath(desiredLocalPath);
    if (normalizedPath.isEmpty()) {
        return normalizedPath;
    }

    if (reuseExistingMapping && m_database && !fileId.isEmpty()) {
        QString existingPath = m_database->getLocalPath(fileId);
        if (!existingPath.isEmpty()) {
            return existingPath;
        }
    }

    if (isPathAvailableForFileId(normalizedPath, fileId, currentLocalPath)) {
        return normalizedPath;
    }

    if (fileId.isEmpty()) {
        return normalizedPath;
    }

    for (int counter = 0; counter < 1000; ++counter) {
        QString candidate = buildDisambiguatedPath(normalizedPath, fileId, counter);
        if (isPathAvailableForFileId(candidate, fileId, currentLocalPath)) {
            return candidate;
        }
    }

    return normalizedPath;
}

QString SyncActionThread::buildDisambiguatedPath(const QString& desiredLocalPath, const QString& fileId,
                                                 int counter) const {
    QFileInfo info(desiredLocalPath);
    QString dirPath = info.path();
    QString baseName = info.completeBaseName();
    QString suffix = info.suffix();

    QString disambiguator = QString("__%1").arg(fileId);
    if (counter > 0) {
        disambiguator += QString("__%1").arg(counter);
    }

    QString fileName;
    if (suffix.isEmpty()) {
        fileName = QString("%1%2").arg(baseName, disambiguator);
    } else {
        fileName = QString("%1%2.%3").arg(baseName, disambiguator, suffix);
    }

    if (dirPath.isEmpty() || dirPath == ".") {
        return fileName;
    }

    return QDir(dirPath).filePath(fileName);
}

bool SyncActionThread::isPathAvailableForFileId(const QString& localPath, const QString& fileId,
                                                const QString& currentLocalPath) const {
    if (localPath == currentLocalPath) {
        return true;
    }

    if (m_database) {
        QString mappedId = m_database->getFileId(localPath);
        if (!mappedId.isEmpty()) {
            return mappedId == fileId;
        }
    }

    QString absolutePath = toAbsolutePath(localPath);
    if (QFileInfo::exists(absolutePath)) {
        return false;
    }

    return true;
}

bool SyncActionThread::resolveRemoteParentId(const QString& parentPath, QString& parentId, bool forceRefresh) {
    QString normalizedParent = QDir::cleanPath(parentPath);
    if (normalizedParent == ".") {
        normalizedParent.clear();
    }

    if (normalizedParent.isEmpty()) {
        parentId = "root";
        return true;
    }

    if (!forceRefresh && m_database) {
        QString dbParentId = m_database->getFileId(normalizedParent);
        if (!dbParentId.isEmpty()) {
            parentId = dbParentId;
            return true;
        }
    }

    if (!m_driveClient) {
        return false;
    }

    QString foundId = m_driveClient->getFolderIdByPath(normalizedParent);
    if (!foundId.isEmpty()) {
        parentId = foundId;
        if (m_database) {
            m_database->setFileId(normalizedParent, foundId);
            m_database->setLocalPath(foundId, normalizedParent);
        }
        return true;
    }

    return false;
}

bool SyncActionThread::deferUntilRemoteParentReady(const QString& parentPath, const SyncActionItem& item) {
    QString normalizedParent = QDir::cleanPath(parentPath);
    if (normalizedParent == ".") {
        normalizedParent.clear();
    }
    if (normalizedParent.isEmpty()) {
        return false;
    }

    if (!m_actionQueue) {
        failAction(item, "Cannot defer action: missing action queue");
        return true;
    }

    QFileInfo parentInfo(toAbsolutePath(normalizedParent));
    if (!parentInfo.exists() || !parentInfo.isDir()) {
        failAction(item, "Missing local parent folder: " + normalizedParent);
        return true;
    }

    bool parentInProgress = false;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        parentInProgress = m_driveActionsInProgress.contains(actionKeyForLocalPath(normalizedParent));
    }

    untrackActionInProgress(item);
    if (m_changeProcessor && !item.localPath.isEmpty()) {
        m_changeProcessor->unmarkFileInOperation(item.localPath);
    }

    if (!parentInProgress) {
        SyncActionItem parentAction;
        parentAction.actionType = SyncActionType::Upload;
        parentAction.localPath = normalizedParent;
        parentAction.isFolder = true;
        if (!m_actionQueue->enqueueIfNotDuplicate(parentAction)) {
            qDebug() << "Parent create already pending, suppressing duplicate:" << normalizedParent;
        }
    }

    if (!m_actionQueue->enqueueIfNotDuplicate(item)) {
        qDebug() << "Deferred action already pending, suppressing duplicate:" << item.localPath;
    }
    qInfo() << "Deferring action until remote parent exists:" << item.localPath << "parent:" << normalizedParent;
    QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
    return true;
}

bool SyncActionThread::scheduleRetry(const SyncActionItem& item, const QString& reason, int baseDelayMs) {
    QString key = actionKeyForItem(item);
    if (key.isEmpty() || !m_actionQueue) {
        return false;
    }

    int attempt = m_retryCounts.value(key, 0);
    if (attempt >= MAX_RETRY_ATTEMPTS) {
        return false;
    }

    int delayMs = baseDelayMs;
    for (int i = 0; i < attempt; ++i) {
        delayMs *= 2;
    }

    m_retryCounts.insert(key, attempt + 1);

    untrackActionInProgress(item);
    if (m_changeProcessor && !item.localPath.isEmpty()) {
        m_changeProcessor->unmarkFileInOperation(item.localPath);
    }

    qWarning() << "Retrying action after recoverable error:" << reason << "attempt" << (attempt + 1) << "of"
               << MAX_RETRY_ATTEMPTS << "delayMs" << delayMs << "path" << item.localPath << "fileId" << item.fileId;

    QTimer::singleShot(delayMs, this, [this, item]() {
        if (!m_actionQueue) {
            return;
        }
        if (!m_actionQueue->enqueueIfNotDuplicate(item)) {
            qDebug() << "Retry enqueue suppressed duplicate action:" << item.localPath << "fileId:" << item.fileId;
        }
    });

    QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
    return true;
}

void SyncActionThread::clearRetryState(const SyncActionItem& item) {
    QString key = actionKeyForItem(item);
    if (!key.isEmpty()) {
        m_retryCounts.remove(key);
    }
}

QString SyncActionThread::computeLocalFileMd5(const QString& localPath) const {
    if (localPath.isEmpty()) {
        return QString();
    }

    const QString absolutePath = toAbsolutePath(localPath);
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

void SyncActionThread::updateDatabaseAfterAction(const SyncActionItem& item, const QString& fileId,
                                                 const QDateTime& modifiedTime, const QString& remoteMd5,
                                                 const QString& localHash) {
    if (!m_database) {
        return;
    }

    // Update file ID mapping for newly created uploads/folders
    if (!item.localPath.isEmpty() && !fileId.isEmpty()) {
        m_database->setFileId(item.localPath, fileId);
        m_database->setLocalPath(fileId, item.localPath);
    }

    // Update modification time in database (per flow chart: "Update file last modified time
    // recorded at sync in DB")
    if (!item.localPath.isEmpty() && modifiedTime.isValid()) {
        m_database->setModifiedTimeAtSync(item.localPath, modifiedTime);
    }

    if (!item.localPath.isEmpty()) {
        QString localHashToStore = localHash;
        if (localHashToStore.isEmpty() && !item.isFolder) {
            localHashToStore = computeLocalFileMd5(item.localPath);
        }
        m_database->setContentHashesAtSync(item.localPath, remoteMd5, localHashToStore);
    }
}

void SyncActionThread::completeAction(const SyncActionItem& item) {
    qInfo() << "Action completed:" << static_cast<int>(item.actionType) << "path:" << item.localPath;

    // Update local metadata if applicable (per flow chart)
    if ((item.actionType == SyncActionType::Download || item.actionType == SyncActionType::MoveLocal ||
         item.actionType == SyncActionType::RenameLocal) &&
        item.modifiedTime.isValid()) {
        QString metadataPath = item.localPath;

        if (item.actionType == SyncActionType::MoveLocal) {
            if (m_database && !item.fileId.isEmpty()) {
                const QString mappedPath = m_database->getLocalPath(item.fileId);
                if (!mappedPath.isEmpty()) {
                    metadataPath = mappedPath;
                }
            }
            if ((metadataPath.isEmpty() || metadataPath == item.localPath) && !item.moveDestination.isEmpty()) {
                metadataPath = item.moveDestination;
            }
        } else if (item.actionType == SyncActionType::RenameLocal) {
            if (m_database && !item.fileId.isEmpty()) {
                const QString mappedPath = m_database->getLocalPath(item.fileId);
                if (!mappedPath.isEmpty()) {
                    metadataPath = mappedPath;
                }
            }
            if ((metadataPath.isEmpty() || metadataPath == item.localPath) && !item.renameTo.isEmpty()) {
                metadataPath = QFileInfo(item.localPath).dir().filePath(item.renameTo);
            }
        }

        updateLocalMetadata(metadataPath, item.modifiedTime);
    }

    // Unmark file from "in operation" (per flow chart)
    if (m_changeProcessor && !item.localPath.isEmpty()) {
        m_changeProcessor->unmarkFileInOperation(item.localPath);
    }

    // Clean up current action tracking
    clearRetryState(item);
    untrackActionInProgress(item);

    emit actionCompleted(item);

    // Continue processing next action
    QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
}

void SyncActionThread::updateLocalMetadata(const QString& localPath, const QDateTime& modifiedTime) {
    QString absolutePath = toAbsolutePath(localPath);

    QFileInfo fileInfo(absolutePath);
    if (!fileInfo.exists()) {
        qWarning() << "Cannot update metadata: local file does not exist:" << absolutePath;
        return;
    }

    // Update the file's modification time
    struct utimbuf newTimes;
    newTimes.actime = fileInfo.lastRead().toSecsSinceEpoch();  // access time
    newTimes.modtime = modifiedTime.toSecsSinceEpoch();        // modification time

    if (utime(absolutePath.toStdString().c_str(), &newTimes) != 0) {
        qWarning() << "Failed to update modification time for file:" << absolutePath;
        return;
    }

    // TODO: Add other platform-specific metadata updates if needed

    qInfo() << "Updated local file metadata for" << localPath << "to modified time" << modifiedTime.toString();
}

void SyncActionThread::failAction(const SyncActionItem& item, const QString& errorMsg) {
    qWarning() << "Action failed:" << static_cast<int>(item.actionType) << "path:" << item.localPath
               << "error:" << errorMsg;

    // Unmark file from "in operation"
    if (m_changeProcessor && !item.localPath.isEmpty()) {
        m_changeProcessor->unmarkFileInOperation(item.localPath);
    }

    clearRetryState(item);
    untrackActionInProgress(item);

    emit actionFailed(item, errorMsg);
    emit error(errorMsg);

    // Continue processing next action
    QTimer::singleShot(0, this, &SyncActionThread::processNextAction);
}

// Drive API response handlers

void SyncActionThread::onFileUploaded(const DriveFile& file) {
    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForFileId(file.id));
    }

    // Verify this response is for the current action
    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::Upload) {
        return;
    }

    qInfo() << "Upload completed:" << file.name << "fileId:" << file.id;

    // Update database with new file ID and modification time
    updateDatabaseAfterAction(currentAction, file.id, file.modifiedTime, file.md5Checksum,
                              currentAction.localContentHash);

    completeAction(currentAction);
}

void SyncActionThread::onFileUploadedDetailed(const DriveFile& file, const QString& localPath) {
    QString normalizedPath = normalizeLocalPath(localPath);
    if (normalizedPath.isEmpty()) {
        return;
    }

    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForLocalPath(normalizedPath));
    }

    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::Upload) {
        return;
    }

    currentAction.fileId = file.id;

    qInfo() << "Upload completed:" << file.name << "fileId:" << file.id << "localPath:" << normalizedPath;

    updateDatabaseAfterAction(currentAction, file.id, file.modifiedTime, file.md5Checksum,
                              currentAction.localContentHash);

    completeAction(currentAction);
}

void SyncActionThread::onFileUpdated(const DriveFile& file) {
    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForFileId(file.id));
    }

    // Verify this response is for the current action
    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::Upload) {
        return;
    }

    qInfo() << "Update completed:" << file.name << "fileId:" << file.id;

    // Update database with modification time
    updateDatabaseAfterAction(currentAction, file.id, file.modifiedTime, file.md5Checksum,
                              currentAction.localContentHash);

    completeAction(currentAction);
}

void SyncActionThread::onFileDownloaded(const QString& fileId, const QString& localPath) {
    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForFileId(fileId));
    }

    // Verify this response is for the current action
    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::Download ||
        currentAction.fileId != fileId) {
        return;
    }

    qInfo() << "Download completed:" << fileId << "to" << localPath;

    // Local metadata is set in completeAction

    // Get the actual modification time of the downloaded file
    QFileInfo fileInfo(localPath);
    QDateTime localModTime =
        currentAction.modifiedTime.isValid() ? currentAction.modifiedTime : fileInfo.lastModified();

    // Update database
    updateDatabaseAfterAction(currentAction, fileId, localModTime, currentAction.remoteMd5,
                              currentAction.localContentHash);

    // If a directory was downloaded, notify the local watcher to start watching it
    if (m_localWatcher && fileInfo.isDir()) {
        QString absolutePath = toAbsolutePath(currentAction.localPath);
        qDebug() << "Notifying local watcher to watch new directory:" << absolutePath;
        m_localWatcher->watchDirectory(absolutePath);
    }

    completeAction(currentAction);
}

void SyncActionThread::onFileMoved(const QString& fileId) {
    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForFileId(fileId));
    }

    // Verify this response is for the current action
    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::MoveRemote ||
        currentAction.fileId != fileId) {
        return;
    }

    qInfo() << "Remote move completed:" << fileId;

    // Update database path mapping
    if (m_database && !currentAction.localPath.isEmpty()) {
        QString newLocalPath;
        if (currentAction.moveDestination.isEmpty() || currentAction.moveDestination == ".") {
            newLocalPath = QFileInfo(currentAction.localPath).fileName();
        } else {
            newLocalPath = QDir(currentAction.moveDestination).filePath(QFileInfo(currentAction.localPath).fileName());
        }
        m_database->setLocalPath(fileId, QDir::cleanPath(newLocalPath));
    }

    completeAction(currentAction);
}

void SyncActionThread::onFileMovedDetailed(const DriveFile& file) {
    if (!m_database || file.id.isEmpty()) {
        return;
    }

    QString localPath = m_database->getLocalPath(file.id);
    if (!localPath.isEmpty() && file.modifiedTime.isValid()) {
        m_database->setModifiedTimeAtSync(localPath, file.modifiedTime);
    }
}

void SyncActionThread::onFileRenamed(const QString& fileId) {
    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForFileId(fileId));
    }

    // Verify this response is for the current action
    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::RenameRemote ||
        currentAction.fileId != fileId) {
        return;
    }

    qInfo() << "Remote rename completed:" << fileId;

    // Update database path mapping with new name
    if (m_database && !currentAction.localPath.isEmpty() && !currentAction.renameTo.isEmpty()) {
        QString newLocalPath = QFileInfo(currentAction.localPath).dir().filePath(currentAction.renameTo);
        m_database->setLocalPath(fileId, QDir::cleanPath(newLocalPath));
    }

    completeAction(currentAction);
}

void SyncActionThread::onFileRenamedDetailed(const DriveFile& file) {
    if (!m_database || file.id.isEmpty()) {
        return;
    }

    QString localPath = m_database->getLocalPath(file.id);
    if (!localPath.isEmpty() && file.modifiedTime.isValid()) {
        m_database->setModifiedTimeAtSync(localPath, file.modifiedTime);
    }
}

void SyncActionThread::onFileDeleted(const QString& fileId) {
    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForFileId(fileId));
    }

    // Verify this response is for the current action
    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::DeleteRemote) {
        return;
    }

    qInfo() << "Remote delete completed:" << fileId;

    // Remove from database
    if (m_database) {
        QString basePath = currentAction.localPath;
        if (basePath.isEmpty()) {
            basePath = m_database->getLocalPath(fileId);
        }

        if (!basePath.isEmpty()) {
            m_database->markFileDeleted(basePath, fileId);
        }

        FileSyncState baseState = m_database->getFileStateById(fileId);
        if (basePath.isEmpty() && !baseState.localPath.isEmpty()) {
            basePath = baseState.localPath;
        }
        bool shouldCascade = baseState.isFolder;
        if (!basePath.isEmpty()) {
            QList<FileSyncState> descendants = m_database->getFileStatesByPrefix(basePath);
            if (!descendants.isEmpty()) {
                shouldCascade = true;
            }
            if (shouldCascade) {
                for (const FileSyncState& state : descendants) {
                    m_database->markFileDeleted(state.localPath, state.fileId);
                }
            }
        }
    }

    completeAction(currentAction);
}

void SyncActionThread::onFolderCreatedDetailed(const DriveFile& folder, const QString& localPath) {
    QString normalizedPath = normalizeLocalPath(localPath);
    if (normalizedPath.isEmpty()) {
        return;
    }

    SyncActionItem currentAction;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        currentAction = m_driveActionsInProgress.value(actionKeyForLocalPath(normalizedPath));
    }

    if (!currentAction.isValid() || currentAction.actionType != SyncActionType::Upload) {
        return;
    }

    currentAction.fileId = folder.id;

    qInfo() << "Folder created:" << folder.name << "fileId:" << folder.id << "localPath:" << normalizedPath;

    updateDatabaseAfterAction(currentAction, folder.id, folder.modifiedTime, QString(), QString());

    completeAction(currentAction);
}

void SyncActionThread::onDriveError(const QString& operation, const QString& errorMsg) {
    // The legacy error signal lacks fileId/localPath, so we cannot reliably
    // match it to a specific in-progress action.  If there is exactly one
    // action in flight we can assume this error belongs to it and fail it;
    // otherwise log a warning so the issue is visible rather than silently
    // dropping it.
    SyncActionItem currentAction;
    bool found = false;
    {
        QMutexLocker locker(&m_driveActionsMutex);
        if (m_driveActionsInProgress.size() == 1) {
            currentAction = m_driveActionsInProgress.constBegin().value();
            found = true;
        }
    }

    if (found) {
        failAction(currentAction, operation + ": " + errorMsg);
    } else {
        qWarning() << "SyncActionThread::onDriveError (legacy signal) – cannot match to action:" << operation
                   << errorMsg << "actions in flight:" << m_driveActionsInProgress.size();
        emit error(operation + ": " + errorMsg);
    }
}

void SyncActionThread::onDriveErrorDetailed(const QString& operation, const QString& errorMsg, int httpStatus,
                                            const QString& fileId, const QString& localPath) {
    SyncActionItem currentAction;
    bool found = false;
    QString normalizedPath = normalizeLocalPath(localPath);

    {
        QMutexLocker locker(&m_driveActionsMutex);
        QString fileKey = actionKeyForFileId(fileId);
        QString pathKey = actionKeyForLocalPath(normalizedPath);
        if (!fileKey.isEmpty() && m_driveActionsInProgress.contains(fileKey)) {
            currentAction = m_driveActionsInProgress.value(fileKey);
            found = true;
        } else if (!pathKey.isEmpty() && m_driveActionsInProgress.contains(pathKey)) {
            currentAction = m_driveActionsInProgress.value(pathKey);
            found = true;
        } else if (!normalizedPath.isEmpty()) {
            for (auto it = m_driveActionsInProgress.constBegin(); it != m_driveActionsInProgress.constEnd(); ++it) {
                if (it.value().localPath == normalizedPath) {
                    currentAction = it.value();
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        qWarning() << "Drive error without matching action:" << operation << errorMsg;
        emit error(operation + ": " + errorMsg);
        return;
    }

    const QString errorLower = errorMsg.toLower();
    const bool isAuthFailure = (httpStatus == 401);
    const bool isStaleParent = (httpStatus == 404) && (operation == "createFolder" || operation == "uploadFile") &&
                               errorLower.contains("file not found");
    const bool isTransientStatus = (httpStatus >= 500 && httpStatus < 600);
    const bool isTransientMessage = errorLower.contains("goaway") || errorLower.contains("timed out") ||
                                    errorLower.contains("timeout") || errorLower.contains("connection reset") ||
                                    errorLower.contains("connection closed") || errorLower.contains("temporary") ||
                                    errorLower.contains("stream error") || errorLower.contains("remote host closed");
    const bool isTransientFailure = isTransientStatus || isTransientMessage;

    if (isStaleParent) {
        QString parentPath = QFileInfo(currentAction.localPath).path();
        QString refreshedParentId;
        if (resolveRemoteParentId(parentPath, refreshedParentId, true)) {
            if (scheduleRetry(currentAction, "stale remote parent mapping", 250)) {
                return;
            }
        } else if (deferUntilRemoteParentReady(parentPath, currentAction)) {
            return;
        }
    }

    if (isAuthFailure) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if ((nowMs - m_lastTokenRefreshRequestMs) >= TOKEN_REFRESH_COOLDOWN_MS) {
            m_lastTokenRefreshRequestMs = nowMs;
            emit tokenRefreshRequested();
        }
        if (scheduleRetry(currentAction, "authentication failure", 400)) {
            return;
        }
    }

    if (isTransientFailure) {
        if (scheduleRetry(currentAction, "transient Drive/network failure", 300)) {
            return;
        }
    }

    failAction(currentAction, operation + ": " + errorMsg);
}
