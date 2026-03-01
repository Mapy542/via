/**
 * @file LocalChangeWatcher.cpp
 * @brief Implementation of local filesystem change watcher
 */

#include "LocalChangeWatcher.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QMutexLocker>
#include <QRegularExpression>

#include "ChangeQueue.h"
#include "utils/FileInUseChecker.h"

const int LocalChangeWatcher::DEBOUNCE_DELAY_MS;
const int LocalChangeWatcher::MOVE_DETECTION_WINDOW_MS;

namespace {
QString computeFileMd5(const QString& absolutePath) {
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
}  // namespace

LocalChangeWatcher::LocalChangeWatcher(ChangeQueue* changeQueue, QObject* parent)
    : QObject(parent),
      m_changeQueue(changeQueue),
      m_watcher(new QFileSystemWatcher(this)),
      m_debounceTimer(new QTimer(this)),
      m_state(State::Stopped),
      m_mutex(QRecursiveMutex()) {
    // Configure debounce timer
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(DEBOUNCE_DELAY_MS);

    // Connect signals
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
            &LocalChangeWatcher::onDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &LocalChangeWatcher::onFileChanged);
    connect(m_debounceTimer, &QTimer::timeout, this, &LocalChangeWatcher::processDebounceQueue);

    // Default ignore patterns
    setIgnorePatterns({"*.tmp", "*.swp", "*.bak", "*~", ".*.swp", ".git/*", ".git", ".DS_Store",
                       "Thumbs.db", "*.part", "*.partial"});
}

LocalChangeWatcher::~LocalChangeWatcher() { stop(); }

void LocalChangeWatcher::setSyncFolder(const QString& path) {
    QMutexLocker locker(&m_mutex);
    m_syncFolder = QDir(path).absolutePath();
}

QString LocalChangeWatcher::syncFolder() const {
    QMutexLocker locker(&m_mutex);
    return m_syncFolder;
}

void LocalChangeWatcher::setIgnorePatterns(const QStringList& patterns) {
    QMutexLocker locker(&m_mutex);
    m_ignorePatterns = patterns;
    m_compiledIgnorePatterns.clear();
    m_compiledIgnorePatterns.reserve(patterns.size());
    for (const QString& pattern : patterns) {
        m_compiledIgnorePatterns.append(
            QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern)));
    }
}

QStringList LocalChangeWatcher::ignorePatterns() const {
    QMutexLocker locker(&m_mutex);
    return m_ignorePatterns;
}

LocalChangeWatcher::State LocalChangeWatcher::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

void LocalChangeWatcher::start() {
    QMutexLocker locker(&m_mutex);

    if (m_syncFolder.isEmpty()) {
        emit error("Cannot start: sync folder not set");
        return;
    }

    QDir dir(m_syncFolder);
    if (!dir.exists()) {
        emit error("Cannot start: sync folder does not exist: " + m_syncFolder);
        return;
    }

    // Clear previous state
    m_fileState.clear();
    m_watchedDirs.clear();
    m_pendingChanges.clear();

    // Remove all existing watches
    QStringList currentPaths = m_watcher->directories() + m_watcher->files();
    if (!currentPaths.isEmpty()) {
        m_watcher->removePaths(currentPaths);
    }

    // Add sync folder and scan recursively
    if (!m_watcher->addPath(m_syncFolder)) {
        emit error("Failed to watch sync folder: " + m_syncFolder);
        return;
    }

    m_watchedDirs.insert(m_syncFolder);
    locker.unlock();

    // Scan directory (outside lock to avoid deadlock)
    scanDirectory(m_syncFolder);
    addDirectoryRecursive(m_syncFolder);

    locker.relock();
    m_state = State::Running;
    locker.unlock();

    emit stateChanged(State::Running);
    qInfo() << "LocalChangeWatcher started, monitoring:" << m_syncFolder;
}

void LocalChangeWatcher::stop() {
    QMutexLocker locker(&m_mutex);

    // Stop timers
    m_debounceTimer->stop();

    // Remove all watches
    QStringList paths = m_watcher->directories() + m_watcher->files();
    if (!paths.isEmpty()) {
        m_watcher->removePaths(paths);
    }

    // Clear state
    m_fileState.clear();
    m_watchedDirs.clear();
    m_pendingChanges.clear();

    m_state = State::Stopped;
    locker.unlock();

    emit stateChanged(State::Stopped);
    qInfo() << "LocalChangeWatcher stopped";
}

void LocalChangeWatcher::pause() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running) {
        return;
    }

    m_state = State::Paused;
    locker.unlock();

    emit stateChanged(State::Paused);
    qDebug() << "LocalChangeWatcher paused";
}

void LocalChangeWatcher::resume() {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Paused) {
        return;
    }

    m_state = State::Running;

    // Make a copy of watched dirs while holding lock
    QStringList dirsToScan = m_watchedDirs.values();
    locker.unlock();

    // Rescan directories to catch any changes while paused
    for (const QString& dir : dirsToScan) {
        scanDirectory(dir);
    }

    emit stateChanged(State::Running);
    qDebug() << "LocalChangeWatcher resumed";
}

void LocalChangeWatcher::watchDirectory(const QString& path) {
    QMutexLocker locker(&m_mutex);

    if (m_state != State::Running) {
        qDebug() << "LocalChangeWatcher not running, skipping watch request for:" << path;
        return;
    }

    if (shouldIgnore(path)) {
        qDebug() << "Path matches ignore pattern, not watching:" << path;
        return;
    }

    QDir dir(path);
    if (!dir.exists()) {
        qWarning() << "Cannot watch non-existent directory:" << path;
        return;
    }

    QString absolutePath = dir.absolutePath();

    // Add the directory itself if not already watched
    if (!m_watchedDirs.contains(absolutePath)) {
        if (m_watcher->addPath(absolutePath)) {
            m_watchedDirs.insert(absolutePath);
            qInfo() << "LocalChangeWatcher now watching directory:" << absolutePath;

            // Unlock before scanning to avoid deadlock - scanDirectory and addDirectoryRecursive
            // acquire their own locks as needed. This pattern matches the start() method.
            locker.unlock();
            scanDirectory(absolutePath);
            addDirectoryRecursive(absolutePath);
        } else {
            qWarning() << "Failed to add directory to watcher:" << absolutePath;
        }
    } else {
        qDebug() << "Directory already being watched:" << absolutePath;
    }
}

void LocalChangeWatcher::unwatchDirectory(const QString& path) {
    QMutexLocker locker(&m_mutex);

    QString absolutePath = QDir(path).absolutePath();

    // Remove the directory and all subdirectories from watch
    if (m_watchedDirs.contains(absolutePath)) {
        m_watcher->removePath(absolutePath);
        m_watchedDirs.remove(absolutePath);
        qInfo() << "LocalChangeWatcher stopped watching directory:" << absolutePath;
    }

    // Remove all subdirectories recursively
    removeDirectoryRecursive(absolutePath);
}

void LocalChangeWatcher::addDirectoryRecursive(const QString& path) {
    QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        QString subDir = it.next();

        if (shouldIgnore(subDir)) {
            continue;
        }

        QMutexLocker locker(&m_mutex);
        if (!m_watchedDirs.contains(subDir)) {
            if (m_watcher->addPath(subDir)) {
                m_watchedDirs.insert(subDir);
                locker.unlock();
                scanDirectory(subDir);
            }
        }
    }
}

void LocalChangeWatcher::removeDirectoryRecursive(const QString& path) {
    QMutexLocker locker(&m_mutex);

    // Remove subdirectories from watch list
    // The parent directory itself should be removed by the caller
    QList<QString> dirsToRemove;
    for (const QString& dir : m_watchedDirs) {
        // Only match subdirectories (path is within directory, but not the directory itself)
        if (dir != path && isPathWithinDirectory(dir, path)) {
            dirsToRemove.append(dir);
        }
    }

    for (const QString& dir : dirsToRemove) {
        m_watcher->removePath(dir);
        m_watchedDirs.remove(dir);
    }

    // Remove tracked files in this directory and all subdirectories
    QList<QString> filesToRemove;
    for (auto it = m_fileState.begin(); it != m_fileState.end(); ++it) {
        if (isPathWithinDirectory(it.key(), path)) {
            filesToRemove.append(it.key());
        }
    }
    for (const QString& file : filesToRemove) {
        m_fileState.remove(file);
    }
}

void LocalChangeWatcher::scanDirectory(const QString& path) {
    QDir dir(path);
    if (!dir.exists()) {
        return;
    }

    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

    QMutexLocker locker(&m_mutex);
    for (const QFileInfo& info : entries) {
        QString filePath = info.absoluteFilePath();

        if (shouldIgnore(filePath)) {
            continue;
        }

        m_fileState[filePath] = info;
    }
}

bool LocalChangeWatcher::shouldIgnore(const QString& path) const {
    QString fileName = QFileInfo(path).fileName();

    for (const QRegularExpression& regex : m_compiledIgnorePatterns) {
        if (regex.match(fileName).hasMatch()) {
            return true;
        }
        if (regex.match(path).hasMatch()) {
            return true;
        }
    }

    return false;
}

bool LocalChangeWatcher::isPathWithinDirectory(const QString& path, const QString& directory) {
    // Check for exact match or if path is a subdirectory/file within directory
    if (path == directory) {
        return true;
    }
    // Ensure we're matching directory boundaries, not partial names
    // e.g., /home/user/test should NOT match /home/user/testing
    QString dirWithSep = directory + "/";
    return path.startsWith(dirWithSep);
}

QString LocalChangeWatcher::getRelativePath(const QString& absolutePath) const {
    if (absolutePath.startsWith(m_syncFolder)) {
        QString relative = absolutePath.mid(m_syncFolder.length());
        if (relative.startsWith('/')) {
            relative = relative.mid(1);
        }
        return relative;
    }
    return absolutePath;
}

void LocalChangeWatcher::queueChange(ChangeType type, const QString& absolutePath,
                                     const QString& oldPath) {
    if (!m_changeQueue) {
        return;
    }

    QString relativePath = getRelativePath(absolutePath);
    QFileInfo info(absolutePath);

    // Skip files that are currently open for writing by another process.
    // They will be re-detected once the write completes and the mtime changes.
    if ((type == ChangeType::Create || type == ChangeType::Modify) && info.exists() &&
        info.isFile() && FileInUseChecker::isFileOpenForWriting(absolutePath)) {
        qDebug() << "LocalChangeWatcher: Skipping file open for writing:" << absolutePath;
        return;
    }

    ChangeQueueItem item;
    item.changeType = type;
    item.origin = ChangeOrigin::Local;
    item.localPath = relativePath;
    item.detectedTime = QDateTime::currentDateTime();
    item.modifiedTime = info.exists() ? info.lastModified() : QDateTime();
    item.isDirectory = info.isDir();
    if (info.exists() && info.isFile()) {
        item.localContentHash = computeFileMd5(absolutePath);
    }

    // For moves/renames, set the destination
    if (type == ChangeType::Move || type == ChangeType::Rename) {
        item.moveDestination = relativePath;
        item.localPath = getRelativePath(oldPath);
        if (type == ChangeType::Rename) {
            item.renameTo = info.fileName();
        }
    }

    m_changeQueue->enqueue(item);
    emit changeDetected(absolutePath);
}

void LocalChangeWatcher::queueDelete(const QString& absolutePath, bool isDirectory,
                                     const QDateTime& modifiedTime) {
    if (!m_changeQueue) {
        return;
    }

    ChangeQueueItem item;
    item.changeType = ChangeType::Delete;
    item.origin = ChangeOrigin::Local;
    item.localPath = getRelativePath(absolutePath);
    item.detectedTime = QDateTime::currentDateTime();
    item.modifiedTime = modifiedTime;
    item.isDirectory = isDirectory;

    m_changeQueue->enqueue(item);
    emit changeDetected(absolutePath);
}

void LocalChangeWatcher::detectMoveOrRename(const QString& deletedPath,
                                            const QString& createdPath) {
    QFileInfo deletedInfo(deletedPath);
    QFileInfo createdInfo(createdPath);

    // Check if this is a rename (same directory, different name)
    if (deletedInfo.dir().absolutePath() == createdInfo.dir().absolutePath()) {
        queueChange(ChangeType::Rename, createdPath, deletedPath);
    } else {
        // This is a move
        queueChange(ChangeType::Move, createdPath, deletedPath);
    }
}

void LocalChangeWatcher::onDirectoryChanged(const QString& path) {
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Running) {
        return;
    }
    locker.unlock();

    QDir dir(path);
    if (!dir.exists()) {
        // Directory was deleted
        QList<QFileInfo> deletedEntries;
        {
            QMutexLocker locker2(&m_mutex);
            for (auto it = m_fileState.begin(); it != m_fileState.end(); ++it) {
                if (isPathWithinDirectory(it.key(), path)) {
                    deletedEntries.append(it.value());
                }
            }
            m_watchedDirs.remove(path);
            removeDirectoryRecursive(path);
        }

        queueDelete(path, true);
        for (const QFileInfo& info : deletedEntries) {
            if (info.absoluteFilePath() == path) {
                continue;
            }
            queueDelete(info.absoluteFilePath(), info.isDir(), info.lastModified());
        }
        return;
    }

    // Get current directory contents
    QFileInfoList currentEntries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    QSet<QString> currentPaths;

    for (const QFileInfo& info : currentEntries) {
        QString filePath = info.absoluteFilePath();

        if (shouldIgnore(filePath)) {
            continue;
        }

        currentPaths.insert(filePath);

        QMutexLocker locker2(&m_mutex);
        if (!m_fileState.contains(filePath)) {
            // New file or directory
            m_fileState[filePath] = info;
            locker2.unlock();

            if (info.isDir()) {
                // New directory - add to watch
                if (!m_watchedDirs.contains(filePath)) {
                    if (m_watcher->addPath(filePath)) {
                        QMutexLocker locker3(&m_mutex);
                        m_watchedDirs.insert(filePath);
                        locker3.unlock();
                        addDirectoryRecursive(filePath);
                    }
                }
            }

            // Store for potential move detection
            PendingChange pending;
            pending.path = filePath;
            pending.isDelete = false;
            pending.isDirectory = info.isDir();
            pending.size = info.size();
            pending.modTime = info.lastModified();
            pending.detectedAt = QDateTime::currentDateTime();
            locker2.relock();
            m_pendingChanges.append(pending);
            locker2.unlock();

            // Start debounce timer for move detection
            if (!m_debounceTimer->isActive()) {
                m_debounceTimer->start();
            }
        } else {
            // Check if modified
            QFileInfo& oldInfo = m_fileState[filePath];
            if (info.lastModified() != oldInfo.lastModified() || info.size() != oldInfo.size()) {
                m_fileState[filePath] = info;
                locker2.unlock();

                if (info.isFile()) {
                    queueChange(ChangeType::Modify, filePath);
                }
            }
        }
    }

    // Check for deleted files
    QMutexLocker locker2(&m_mutex);
    QList<QString> toRemove;
    for (auto it = m_fileState.begin(); it != m_fileState.end(); ++it) {
        // Only check direct children of this directory
        if (isPathWithinDirectory(it.key(), path) && it.key() != path &&
            !currentPaths.contains(it.key())) {
            // Check if direct child (no "/" in the path relative to parent)
            QString relativePath = it.key().mid(path.length() + 1);
            if (!relativePath.contains('/')) {
                toRemove.append(it.key());

                // Store for potential move detection
                PendingChange pending;
                pending.path = it.key();
                pending.isDelete = true;
                pending.isDirectory = it.value().isDir();
                pending.size = it.value().size();
                pending.modTime = it.value().lastModified();
                pending.detectedAt = QDateTime::currentDateTime();
                m_pendingChanges.append(pending);
            }
        }
    }

    for (const QString& key : toRemove) {
        m_fileState.remove(key);
    }
    locker2.unlock();

    // Start debounce timer for move detection
    if (!toRemove.isEmpty() && !m_debounceTimer->isActive()) {
        m_debounceTimer->start();
    }
}

void LocalChangeWatcher::onFileChanged(const QString& path) {
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Running) {
        return;
    }
    locker.unlock();

    QFileInfo info(path);

    if (!info.exists()) {
        // File was deleted
        bool wasDirectory = false;
        QDateTime lastModified;
        {
            QMutexLocker locker2(&m_mutex);
            if (m_fileState.contains(path)) {
                const QFileInfo& oldInfo = m_fileState[path];
                wasDirectory = oldInfo.isDir();
                lastModified = oldInfo.lastModified();
            }
            m_fileState.remove(path);
        }
        queueDelete(path, wasDirectory, lastModified);
    } else {
        // File was modified
        QMutexLocker locker2(&m_mutex);
        m_fileState[path] = info;
        locker2.unlock();
        queueChange(ChangeType::Modify, path);
    }
}

void LocalChangeWatcher::processDebounceQueue() {
    QMutexLocker locker(&m_mutex);

    QDateTime now = QDateTime::currentDateTime();
    QList<PendingChange> creates;
    QList<PendingChange> deletes;

    // Separate creates and deletes
    for (const PendingChange& change : m_pendingChanges) {
        if (change.isDelete) {
            deletes.append(change);
        } else {
            creates.append(change);
        }
    }

    // Try to match deletes with creates for move detection
    QSet<int> matchedCreates;
    QSet<int> matchedDeletes;

    for (int d = 0; d < deletes.size(); ++d) {
        const PendingChange& del = deletes[d];

        for (int c = 0; c < creates.size(); ++c) {
            if (matchedCreates.contains(c)) {
                continue;
            }

            const PendingChange& create = creates[c];

            // Check if this could be a move (same size, similar mod time, same type)
            if (del.isDirectory == create.isDirectory && del.size == create.size &&
                qAbs(del.detectedAt.msecsTo(create.detectedAt)) < MOVE_DETECTION_WINDOW_MS) {
                // This looks like a move
                matchedCreates.insert(c);
                matchedDeletes.insert(d);

                locker.unlock();
                detectMoveOrRename(del.path, create.path);
                locker.relock();
                break;
            }
        }
    }

    // Process unmatched deletes as actual deletions
    for (int d = 0; d < deletes.size(); ++d) {
        if (!matchedDeletes.contains(d)) {
            locker.unlock();
            queueDelete(deletes[d].path, deletes[d].isDirectory, deletes[d].modTime);
            locker.relock();
        }
    }

    // Process unmatched creates as actual creations
    for (int c = 0; c < creates.size(); ++c) {
        if (!matchedCreates.contains(c)) {
            locker.unlock();
            queueChange(ChangeType::Create, creates[c].path);
            locker.relock();
        }
    }

    m_pendingChanges.clear();
}
