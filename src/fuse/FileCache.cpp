/**
 * @file FileCache.cpp
 * @brief Implementation of LRU file cache manager for FUSE filesystem
 */

#include "FileCache.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStandardPaths>

#include "api/GoogleDriveClient.h"
#include "sync/SyncDatabase.h"

FileCache::FileCache(SyncDatabase* database, GoogleDriveClient* driveClient, QObject* parent)
    : QObject(parent),
      m_database(database),
      m_driveClient(driveClient),
      m_maxCacheSize(DEFAULT_MAX_CACHE_SIZE),
      m_currentSize(0) {
    // Set default cache directory (evictable, under XDG_CACHE_HOME)
    m_cacheDirectory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/Via/files";

    // Set default pending-uploads directory (persistent, under XDG_DATA_HOME).
    // Dirty files are moved here after their write handle is closed so that
    // OS cache-cleaning tools cannot delete unsaved user data.
    m_dirtyDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Via/pending";

    // Connect to GoogleDriveClient signals for download completion
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::fileDownloaded, this,
                &FileCache::onFileDownloaded);
        connect(m_driveClient, &GoogleDriveClient::errorDetailed, this,
                &FileCache::onDownloadError);
    }
}

FileCache::~FileCache() {
    // Note: We don't clear cache on destruction - it persists across sessions
    qInfo() << "FileCache destroyed, cache preserved at:" << m_cacheDirectory;
}

bool FileCache::initialize() {
    QMutexLocker locker(&m_mutex);

    // Create cache directory if it doesn't exist
    QDir dir(m_cacheDirectory);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qWarning() << "FileCache: Failed to create cache directory:" << m_cacheDirectory;
            return false;
        }
    }

    // Create pending-uploads directory (persistent XDG_DATA_HOME store).
    // This must succeed — without it dirty files cannot be moved to safety.
    QDir dirtyDir(m_dirtyDirectory);
    if (!dirtyDir.exists()) {
        if (!dirtyDir.mkpath(".")) {
            qWarning() << "FileCache: Failed to create pending-uploads directory:"
                       << m_dirtyDirectory;
            return false;
        }
    }

    // Load cache state from database
    loadCacheFromDatabase();

    // Calculate actual cache size from disk
    updateCacheSizeFromDisk();

    qInfo() << "FileCache initialized at:" << m_cacheDirectory;
    qInfo() << "Pending-uploads store at:" << m_dirtyDirectory;
    qInfo() << "Current cache size:" << (m_currentSize / 1024 / 1024) << "MB";
    qInfo() << "Max cache size:" << (m_maxCacheSize / 1024 / 1024 / 1024) << "GB";
    qInfo() << "Cached files:" << m_cacheEntries.size();
    qInfo() << "Dirty files:" << m_dirtyFiles.size();

    return true;
}

QString FileCache::cacheDirectory() const {
    QMutexLocker locker(&m_mutex);
    return m_cacheDirectory;
}

void FileCache::setCacheDirectory(const QString& path) {
    QMutexLocker locker(&m_mutex);
    m_cacheDirectory = path;
}

QString FileCache::dirtyDirectory() const {
    QMutexLocker locker(&m_mutex);
    return m_dirtyDirectory;
}

void FileCache::setDirtyDirectory(const QString& path) {
    QMutexLocker locker(&m_mutex);
    m_dirtyDirectory = path;
}

qint64 FileCache::maxCacheSize() const {
    QMutexLocker locker(&m_mutex);
    return m_maxCacheSize;
}

void FileCache::setMaxCacheSize(qint64 bytes) {
    QMutexLocker locker(&m_mutex);
    m_maxCacheSize = bytes;

    // Trigger eviction if current size exceeds new max
    while (m_currentSize > m_maxCacheSize && !m_cacheEntries.isEmpty()) {
        evictLRU();
    }
}

qint64 FileCache::currentCacheSize() const {
    QMutexLocker locker(&m_mutex);
    return m_currentSize;
}

bool FileCache::isCached(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);

    if (!m_cacheEntries.contains(fileId)) {
        return false;
    }

    // Verify file actually exists on disk
    const CacheEntry& entry = m_cacheEntries[fileId];
    return QFile::exists(entry.cachePath);
}

QString FileCache::getCachedPath(const QString& fileId, qint64 expectedSize) {
    // Check if already cached (with lock)
    {
        QMutexLocker locker(&m_mutex);

        if (m_cacheEntries.contains(fileId)) {
            const CacheEntry& entry = m_cacheEntries[fileId];
            if (QFile::exists(entry.cachePath)) {
                // Update access time
                m_cacheEntries[fileId].lastAccessed = QDateTime::currentDateTime();

                // Update in database
                if (m_database) {
                    m_database->updateCacheAccessTime(fileId);
                }

                qDebug() << "FileCache: Cache hit for" << fileId;
                return entry.cachePath;
            } else {
                // File missing from disk, remove stale entry
                m_cacheEntries.remove(fileId);
                if (m_database) {
                    m_database->evictFuseCacheEntry(fileId);
                }
            }
        }

        // Dirty guard: if the file has unsynced local modifications, do NOT
        // download from remote — that would silently overwrite the user's changes.
        //
        // Check the pending store first (file was already released and moved),
        // then fall back to the cache path (file is still open / not yet moved).
        if (m_dirtyFiles.contains(fileId)) {
            // Primary: persistent pending-uploads store (post-release)
            QString pendingPath = generateDirtyPath(fileId);
            if (QFile::exists(pendingPath)) {
                qDebug() << "FileCache: Serving dirty file from pending store for" << fileId;
                return pendingPath;
            }
            // Secondary: cache dir (write handle still open, file not moved yet)
            QString expectedPath = generateCachePath(fileId);
            if (QFile::exists(expectedPath)) {
                QFileInfo fi(expectedPath);
                CacheEntry recovered;
                recovered.fileId = fileId;
                recovered.cachePath = expectedPath;
                recovered.size = fi.size();
                recovered.lastAccessed = QDateTime::currentDateTime();
                recovered.downloadCompleted = fi.lastModified();
                m_cacheEntries[fileId] = recovered;
                m_currentSize += recovered.size;
                qWarning() << "FileCache: Rehydrated dirty cache entry for" << fileId;
                return expectedPath;
            }
            // Dirty file absent from both stores — local changes are lost; fall through
            // to re-download so the file handle at least remains valid.
            qCritical() << "FileCache: Dirty file" << fileId
                        << "is missing from both pending store and cache"
                        << "— local changes may be lost; falling back to remote download";
        }
    }

    // Not cached - need to download
    qDebug() << "FileCache: Cache miss for" << fileId << ", downloading...";

    // Ensure we have space for the file
    if (expectedSize > 0) {
        QMutexLocker locker(&m_mutex);
        if (m_currentSize + expectedSize > m_maxCacheSize) {
            // Need to evict to make space
            while (m_currentSize + expectedSize > m_maxCacheSize && !m_cacheEntries.isEmpty()) {
                evictLRU();
            }
        }
    }

    // Generate cache path for this file
    QString cachePath = generateCachePath(fileId);

    // Ensure parent directory exists
    QFileInfo fileInfo(cachePath);
    QDir parentDir = fileInfo.dir();
    if (!parentDir.exists()) {
        parentDir.mkpath(".");
    }

    // Initiate download
    {
        QMutexLocker locker(&m_mutex);
        m_pendingDownloads[fileId] = false;  // Mark as in-progress
        m_downloadErrors.remove(fileId);
    }

    emit downloadStarted(fileId);

    // Request download from GoogleDriveClient.
    // Must invoke on main thread since QNetworkAccessManager lives there.
    // QueuedConnection: the request is dispatched without blocking the calling
    // thread until the main thread is free.  The FUSE thread waits for the
    // download result via m_downloadCondition below.
    if (m_driveClient) {
        QMetaObject::invokeMethod(
            m_driveClient,
            [driveClient = m_driveClient, fileId, cachePath]() {
                driveClient->downloadFile(fileId, cachePath);
            },
            Qt::QueuedConnection);
    } else {
        qWarning() << "FileCache: No GoogleDriveClient available for download";
        return QString();
    }

    // Wait for download to complete
    // This is a blocking operation as required by FUSE open semantics
    {
        QMutexLocker locker(&m_mutex);
        while (!m_pendingDownloads.value(fileId, true)) {
            // Wait with timeout to prevent indefinite blocking
            if (!m_downloadCondition.wait(&m_mutex, 30000)) {  // 30 second timeout
                qWarning() << "FileCache: Download timeout for" << fileId;
                m_pendingDownloads.remove(fileId);
                return QString();
            }
        }

        // Check if download succeeded
        if (m_downloadErrors.contains(fileId)) {
            QString error = m_downloadErrors.take(fileId);
            m_pendingDownloads.remove(fileId);
            qWarning() << "FileCache: Download failed for" << fileId << ":" << error;
            emit downloadFailed(fileId, error);
            return QString();
        }

        m_pendingDownloads.remove(fileId);
    }

    // Verify file was downloaded
    if (!QFile::exists(cachePath)) {
        qWarning() << "FileCache: Downloaded file not found at" << cachePath;
        return QString();
    }

    // Record cache entry
    QFileInfo downloadedFile(cachePath);
    CacheEntry entry;
    entry.fileId = fileId;
    entry.cachePath = cachePath;
    entry.size = downloadedFile.size();
    entry.lastAccessed = QDateTime::currentDateTime();
    entry.downloadCompleted = QDateTime::currentDateTime();

    {
        QMutexLocker locker(&m_mutex);
        m_cacheEntries[fileId] = entry;
        m_currentSize += entry.size;
    }

    // Save to database
    if (m_database) {
        m_database->recordFuseCacheEntry(fileId, cachePath, entry.size);
    }

    emit downloadCompleted(fileId, cachePath);
    emit cacheSizeChanged(m_currentSize);

    qInfo() << "FileCache: Downloaded and cached" << fileId << "(" << entry.size << "bytes)";

    return cachePath;
}

QString FileCache::getCachePathForFile(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    return generateCachePath(fileId);
}

QString FileCache::getDirtyPathForFile(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    return generateDirtyPath(fileId);
}

void FileCache::updateAccessTime(const QString& fileId) {
    QMutexLocker locker(&m_mutex);

    if (m_cacheEntries.contains(fileId)) {
        m_cacheEntries[fileId].lastAccessed = QDateTime::currentDateTime();

        // Update in database
        if (m_database) {
            m_database->updateCacheAccessTime(fileId);
        }
    }
}

void FileCache::invalidate(const QString& fileId) {
    QMutexLocker locker(&m_mutex);

    qDebug() << "FileCache: Invalidating cache entry for" << fileId;

    if (!m_cacheEntries.contains(fileId)) {
        return;
    }

    // C1 fix: Never delete a dirty file — local modifications would be lost.
    // The file will be uploaded by DirtySyncWorker, then re-downloaded on next access.
    if (m_dirtyFiles.contains(fileId)) {
        qWarning() << "FileCache: Skipping invalidation of dirty file" << fileId
                   << "— local changes pending upload";
        return;
    }

    CacheEntry entry = m_cacheEntries.take(fileId);

    // Delete the cached file from disk
    if (QFile::exists(entry.cachePath)) {
        QFile::remove(entry.cachePath);
    }

    // Update size tracking
    m_currentSize -= entry.size;
    if (m_currentSize < 0) {
        m_currentSize = 0;
    }

    // Remove from database
    if (m_database) {
        m_database->evictFuseCacheEntry(fileId);
    }

    emit fileEvicted(fileId);
    emit cacheSizeChanged(m_currentSize);
}

void FileCache::removeFromCache(const QString& fileId) {
    // Same as invalidate, but also clear dirty status
    QMutexLocker locker(&m_mutex);

    qDebug() << "FileCache: Removing" << fileId << "from cache";

    // Remove cache entry
    if (m_cacheEntries.contains(fileId)) {
        CacheEntry entry = m_cacheEntries.take(fileId);

        if (QFile::exists(entry.cachePath)) {
            QFile::remove(entry.cachePath);
        }

        m_currentSize -= entry.size;
        if (m_currentSize < 0) {
            m_currentSize = 0;
        }

        if (m_database) {
            m_database->evictFuseCacheEntry(fileId);
        }
    }

    // Remove dirty entry and its pending-store file
    if (m_dirtyFiles.contains(fileId)) {
        QString pendingPath = generateDirtyPath(fileId);
        if (QFile::exists(pendingPath)) {
            QFile::remove(pendingPath);
        }
        m_dirtyFiles.remove(fileId);

        if (m_database) {
            m_database->clearFuseDirty(fileId);
        }
    }

    emit fileEvicted(fileId);
    emit cacheSizeChanged(m_currentSize);
}

void FileCache::clearCache() {
    QMutexLocker locker(&m_mutex);

    qInfo() << "FileCache: Clearing entire cache";

    // Delete all cached files, but never touch dirty files — their content
    // lives in the pending-uploads store (m_dirtyDirectory) and must survive.
    for (const auto& it : m_cacheEntries.asKeyValueRange()) {
        if (m_dirtyFiles.contains(it.first)) {
            qWarning() << "FileCache: Skipping delete of dirty file in clearCache:" << it.first;
            continue;
        }
        if (QFile::exists(it.second.cachePath)) {
            QFile::remove(it.second.cachePath);
        }
    }

    m_cacheEntries.clear();
    m_currentSize = 0;

    // Note: We don't clear dirty files - they still need to be uploaded
    // This is intentional to prevent data loss

    // Clear cache entries from database (not dirty files)
    if (m_database) {
        m_database->clearAllFuseCacheEntries();
    }

    emit cacheSizeChanged(0);
}

void FileCache::markDirty(const QString& fileId, const QString& path) {
    QMutexLocker locker(&m_mutex);

    qDebug() << "FileCache: Marking" << fileId << "as dirty";

    DirtyFileEntry entry;
    entry.fileId = fileId;
    entry.path = path;
    entry.markedDirtyAt = QDateTime::currentDateTime();
    entry.uploadFailed = false;

    m_dirtyFiles[fileId] = entry;

    // Record in database
    if (m_database) {
        m_database->markFuseDirty(fileId, path);
    }

    emit fileDirty(fileId, path);
}

void FileCache::clearDirty(const QString& fileId) {
    QMutexLocker locker(&m_mutex);

    qDebug() << "FileCache: Clearing dirty flag for" << fileId;

    // Remove the pending-store file — upload succeeded, so the canonical copy
    // is now on the remote and the local pending file is no longer needed.
    QString pendingPath = generateDirtyPath(fileId);
    if (QFile::exists(pendingPath)) {
        QFile::remove(pendingPath);
        qDebug() << "FileCache: Removed pending file" << pendingPath;
    }

    m_dirtyFiles.remove(fileId);

    if (m_database) {
        m_database->clearFuseDirty(fileId);
    }
}

void FileCache::markUploadFailed(const QString& fileId) {
    QMutexLocker locker(&m_mutex);

    if (m_dirtyFiles.contains(fileId)) {
        m_dirtyFiles[fileId].uploadFailed = true;
        m_dirtyFiles[fileId].lastUploadAttempt = QDateTime::currentDateTime();

        // Persist to database
        if (m_database) {
            m_database->markFuseUploadFailed(fileId);
        }
    }
}

bool FileCache::isDirty(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    return m_dirtyFiles.contains(fileId);
}

QList<DirtyFileEntry> FileCache::getDirtyFiles() const {
    QMutexLocker locker(&m_mutex);
    return m_dirtyFiles.values();
}

bool FileCache::evictToFreeSpace(qint64 bytesNeeded) {
    QMutexLocker locker(&m_mutex);

    qint64 available = m_maxCacheSize - m_currentSize;

    while (available < bytesNeeded && !m_cacheEntries.isEmpty()) {
        evictLRU();
        available = m_maxCacheSize - m_currentSize;
    }

    return available >= bytesNeeded;
}

bool FileCache::recordCacheEntry(const QString& fileId, const QString& localPath, qint64 size) {
    QMutexLocker locker(&m_mutex);

    // Check if we need to evict first
    if (m_currentSize + size > m_maxCacheSize) {
        while (m_currentSize + size > m_maxCacheSize && !m_cacheEntries.isEmpty()) {
            evictLRU();
        }
    }

    CacheEntry entry;
    entry.fileId = fileId;
    entry.cachePath = localPath;
    entry.size = size;
    entry.lastAccessed = QDateTime::currentDateTime();
    entry.downloadCompleted = QDateTime::currentDateTime();

    m_cacheEntries[fileId] = entry;
    m_currentSize += size;

    // Save to database
    if (m_database) {
        m_database->recordFuseCacheEntry(fileId, localPath, size);
    }

    emit cacheSizeChanged(m_currentSize);
    return true;
}

// ============================================================================
// Private Slots
// ============================================================================

void FileCache::onFileDownloaded(const QString& fileId, const QString& localPath) {
    QMutexLocker locker(&m_mutex);

    // localPath is provided for potential verification but not currently used
    // The actual cache path is tracked in m_cacheEntries
    (void)localPath;

    // Mark download as completed successfully
    if (m_pendingDownloads.contains(fileId)) {
        m_pendingDownloads[fileId] = true;  // Mark as completed
        m_downloadCondition.wakeAll();
    }
}

void FileCache::onDownloadError(const QString& operation, const QString& errorMsg, int httpStatus,
                                const QString& fileId, const QString& localPath) {
    Q_UNUSED(httpStatus)
    Q_UNUSED(localPath)

    // Check if this is a download error
    if (!operation.startsWith("download")) {
        return;
    }

    QMutexLocker locker(&m_mutex);

    // The errorDetailed signal provides the fileId directly via tagReply(),
    // so we can always identify which download failed.
    if (!fileId.isEmpty() && m_pendingDownloads.contains(fileId)) {
        m_downloadErrors[fileId] = errorMsg;
        m_pendingDownloads[fileId] = true;  // Mark as completed (with error)
    } else {
        // Fallback: try to extract fileId from operation string
        // (format "downloadFile:<fileId>")
        QString parsedId;
        if (operation.contains(':')) {
            parsedId = operation.mid(operation.indexOf(':') + 1);
        }

        if (!parsedId.isEmpty() && m_pendingDownloads.contains(parsedId)) {
            m_downloadErrors[parsedId] = errorMsg;
            m_pendingDownloads[parsedId] = true;
        } else {
            // Last resort: could not identify the failed file.
            // Only mark downloads as failed if we truly can't match.
            qWarning() << "FileCache: Download error without file ID association:" << operation
                       << errorMsg;

            for (auto it = m_pendingDownloads.begin(); it != m_pendingDownloads.end(); ++it) {
                if (!it.value()) {  // Still in progress
                    m_downloadErrors[it.key()] = errorMsg;
                    it.value() = true;  // Mark as completed (with error)
                }
            }
        }
    }

    m_downloadCondition.wakeAll();
}

// ============================================================================
// Private Helpers
// ============================================================================

QString FileCache::generateCachePath(const QString& fileId) const {
    // Use hash of file ID for cache path to avoid filesystem issues with IDs
    QByteArray hash = QCryptographicHash::hash(fileId.toUtf8(), QCryptographicHash::Sha256).toHex();

    // Use first 2 characters as subdirectory for better filesystem distribution
    QString subDir = QString::fromLatin1(hash.left(2));

    return m_cacheDirectory + "/" + subDir + "/" + QString::fromLatin1(hash);
}

QString FileCache::generateDirtyPath(const QString& fileId) const {
    // Same hashing scheme as the cache path but rooted in the persistent
    // pending-uploads store so that the OS cannot evict it.
    QByteArray hash = QCryptographicHash::hash(fileId.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString subDir = QString::fromLatin1(hash.left(2));
    return m_dirtyDirectory + "/" + subDir + "/" + QString::fromLatin1(hash);
}

QString FileCache::getContentPath(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);

    // If dirty, the authoritative content is in the pending store (post-release)
    // or still in the cache dir (write handle still open).
    if (m_dirtyFiles.contains(fileId)) {
        QString pendingPath = generateDirtyPath(fileId);
        if (QFile::exists(pendingPath)) {
            return pendingPath;
        }
        // Fall through: file is dirty but not yet moved (still open for writing)
    }

    // For clean files, or dirty files whose handle is still open, return the
    // deterministic cache path (which may or may not exist on disk right now).
    return generateCachePath(fileId);
}

QString FileCache::moveToDirtyStore(const QString& fileId) {
    QMutexLocker locker(&m_mutex);

    // Nothing to move if not in cache
    if (!m_cacheEntries.contains(fileId)) {
        // File may already be in the pending store (re-entrant call guard)
        QString pendingPath = generateDirtyPath(fileId);
        if (QFile::exists(pendingPath)) {
            qDebug() << "FileCache: File already in pending store for" << fileId;
            return pendingPath;
        }
        qWarning() << "FileCache: moveToDirtyStore called but no cache entry for" << fileId;
        return QString();
    }

    CacheEntry cacheEntry = m_cacheEntries[fileId];
    QString pendingPath = generateDirtyPath(fileId);

    // Ensure the pending-store subdirectory exists
    QFileInfo pendingInfo(pendingPath);
    QDir pendingSubDir = pendingInfo.dir();
    if (!pendingSubDir.exists()) {
        if (!pendingSubDir.mkpath(".")) {
            qWarning() << "FileCache: Could not create pending subdirectory for" << fileId;
            return QString();
        }
    }

    // Rename (atomic on same filesystem; falls back to copy+delete cross-fs)
    if (!QFile::rename(cacheEntry.cachePath, pendingPath)) {
        // Cross-filesystem: copy then remove
        if (!QFile::copy(cacheEntry.cachePath, pendingPath)) {
            qWarning() << "FileCache: Could not move dirty file to pending store for" << fileId;
            return QString();
        }
        QFile::remove(cacheEntry.cachePath);
    }

    // Remove from LRU cache accounting — it is no longer evictable
    m_currentSize -= cacheEntry.size;
    if (m_currentSize < 0) m_currentSize = 0;
    m_cacheEntries.remove(fileId);

    if (m_database) {
        m_database->evictFuseCacheEntry(fileId);
    }

    qDebug() << "FileCache: Moved dirty file to pending store" << pendingPath;
    return pendingPath;
}

void FileCache::loadCacheFromDatabase() {
    if (!m_database) {
        qWarning() << "FileCache: No database available, starting with empty cache";
        return;
    }

    // Load cache entries from fuse_cache_entries table
    QList<FuseCacheEntry> dbEntries = m_database->getFuseCacheEntries();
    for (const FuseCacheEntry& dbEntry : dbEntries) {
        // Only load entries where file still exists on disk
        if (QFile::exists(dbEntry.cachePath)) {
            CacheEntry entry;
            entry.fileId = dbEntry.fileId;
            entry.cachePath = dbEntry.cachePath;
            entry.size = dbEntry.size;
            entry.lastAccessed = dbEntry.lastAccessed;
            entry.downloadCompleted = dbEntry.downloadCompleted;

            m_cacheEntries[entry.fileId] = entry;
            m_currentSize += entry.size;
        } else {
            // File missing from disk, clean up database entry
            m_database->evictFuseCacheEntry(dbEntry.fileId);
        }
    }

    // Load dirty files from fuse_dirty_files table with complete data
    QList<FuseDirtyFile> dirtyFileEntries = m_database->getFuseDirtyFiles();
    for (const FuseDirtyFile& dbEntry : dirtyFileEntries) {
        DirtyFileEntry entry;
        entry.fileId = dbEntry.fileId;
        entry.path = dbEntry.path;
        entry.markedDirtyAt = dbEntry.markedDirtyAt;
        entry.lastUploadAttempt = dbEntry.lastUploadAttempt;
        entry.uploadFailed = dbEntry.uploadFailed;

        m_dirtyFiles[entry.fileId] = entry;
    }

    qDebug() << "FileCache: Loaded" << m_cacheEntries.size() << "cache entries from database";
    qDebug() << "FileCache: Loaded" << m_dirtyFiles.size() << "dirty file entries from database";
}

void FileCache::evictLRU() {
    // Find least recently used entry that is NOT dirty
    QString lruFileId;
    QDateTime lruTime = QDateTime::currentDateTime();

    for (auto it = m_cacheEntries.constBegin(); it != m_cacheEntries.constEnd(); ++it) {
        // Skip dirty files - they must not be evicted
        if (m_dirtyFiles.contains(it.key())) {
            continue;
        }

        if (it.value().lastAccessed < lruTime) {
            lruTime = it.value().lastAccessed;
            lruFileId = it.key();
        }
    }

    if (lruFileId.isEmpty()) {
        qWarning() << "FileCache: Cannot evict - all cached files are dirty";
        return;
    }

    // Evict the LRU entry
    CacheEntry entry = m_cacheEntries.take(lruFileId);

    // Delete file from disk
    if (QFile::exists(entry.cachePath)) {
        QFile::remove(entry.cachePath);
    }

    // Update size tracking
    m_currentSize -= entry.size;
    if (m_currentSize < 0) {
        m_currentSize = 0;
    }

    // Remove from database
    if (m_database) {
        m_database->evictFuseCacheEntry(lruFileId);
    }

    qDebug() << "FileCache: Evicted LRU file" << lruFileId << "(" << entry.size << "bytes)";

    // Note: Signal emission is deferred to avoid emitting while holding the mutex.
    // The caller (evictToFreeSpace, setMaxCacheSize, etc.) should emit signals
    // after releasing the lock if needed. For internal LRU eviction, we log
    // but don't emit per-file signals to avoid excessive signaling during bulk eviction.
}

void FileCache::updateCacheSizeFromDisk() {
    // Recalculate actual cache size from entries
    m_currentSize = 0;

    for (const CacheEntry& entry : m_cacheEntries) {
        if (QFile::exists(entry.cachePath)) {
            QFileInfo info(entry.cachePath);
            m_currentSize += info.size();
        }
    }
}
