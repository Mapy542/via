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

namespace {
QString cacheKeyFor(const QString& fileId, const QString& exportMimeType = QString()) {
    return exportMimeType.isEmpty() ? fileId : fileId + QLatin1Char('|') + exportMimeType;
}

QString remoteFileIdFromCacheKey(const QString& cacheKey) {
    const int separator = cacheKey.indexOf(QLatin1Char('|'));
    return separator >= 0 ? cacheKey.left(separator) : cacheKey;
}

bool cacheKeyBelongsToFile(const QString& cacheKey, const QString& fileId) {
    return cacheKey == fileId || cacheKey.startsWith(fileId + QLatin1Char('|'));
}

bool hasUsableExportBytes(const QString& cachePath) {
    QFileInfo info(cachePath);
    return info.exists() && info.size() > 0;
}
}  // namespace

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

    if (!resetSnapshotDirectoryLocked()) {
        return false;
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
        qint64 sizeBefore = m_currentSize;
        evictLRU();
        if (m_currentSize == sizeBefore) {
            break;
        }
    }
}

qint64 FileCache::currentCacheSize() const {
    QMutexLocker locker(&m_mutex);
    return m_currentSize;
}

bool FileCache::isCached(const QString& fileId) const { return isCached(fileId, QString()); }

bool FileCache::isCached(const QString& fileId, const QString& exportMimeType) const {
    QMutexLocker locker(&m_mutex);

    const QString cacheKey = cacheKeyFor(fileId, exportMimeType);

    if (!m_cacheEntries.contains(cacheKey)) {
        return false;
    }

    // Verify file actually exists on disk
    const CacheEntry& entry = m_cacheEntries[cacheKey];
    if (exportMimeType.isEmpty()) {
        return QFile::exists(entry.cachePath);
    }

    return hasUsableExportBytes(entry.cachePath);
}

QString FileCache::getCachedPath(const QString& fileId, qint64 expectedSize) {
    const QString cacheKey = cacheKeyFor(fileId);

    // Check if already cached (with lock)
    {
        QMutexLocker locker(&m_mutex);

        if (m_cacheEntries.contains(cacheKey)) {
            const CacheEntry& entry = m_cacheEntries[cacheKey];
            if (QFile::exists(entry.cachePath)) {
                // Update access time
                m_cacheEntries[cacheKey].lastAccessed = QDateTime::currentDateTime();

                // Update in database
                if (m_database) {
                    m_database->updateCacheAccessTime(cacheKey);
                }

                qDebug() << "FileCache: Cache hit for" << fileId;
                return entry.cachePath;
            } else {
                // File missing from disk, remove stale entry
                m_cacheEntries.remove(cacheKey);
                if (m_database) {
                    m_database->evictFuseCacheEntry(cacheKey);
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
                recovered.cacheKey = cacheKey;
                recovered.cachePath = expectedPath;
                recovered.size = fi.size();
                recovered.lastAccessed = QDateTime::currentDateTime();
                recovered.downloadCompleted = fi.lastModified();
                m_cacheEntries[cacheKey] = recovered;
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
                qint64 sizeBefore = m_currentSize;
                evictLRU();
                if (m_currentSize == sizeBefore) {
                    break;
                }
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
        m_pendingDownloads[cacheKey] = false;  // Mark as in-progress
        m_pendingDownloadPaths[cachePath] = cacheKey;
        m_downloadErrors.remove(cacheKey);
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
        while (!m_pendingDownloads.value(cacheKey, true)) {
            // Wait with timeout to prevent indefinite blocking
            if (!m_downloadCondition.wait(&m_mutex, 30000)) {  // 30 second timeout
                qWarning() << "FileCache: Download timeout for" << fileId;
                m_pendingDownloads.remove(cacheKey);
                m_pendingDownloadPaths.remove(cachePath);
                return QString();
            }
        }

        // Check if download succeeded
        if (m_downloadErrors.contains(cacheKey)) {
            QString error = m_downloadErrors.take(cacheKey);
            m_pendingDownloads.remove(cacheKey);
            m_pendingDownloadPaths.remove(cachePath);
            qWarning() << "FileCache: Download failed for" << fileId << ":" << error;
            emit downloadFailed(fileId, error);
            return QString();
        }

        m_pendingDownloads.remove(cacheKey);
        m_pendingDownloadPaths.remove(cachePath);
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
    entry.cacheKey = cacheKey;
    entry.cachePath = cachePath;
    entry.size = downloadedFile.size();
    entry.lastAccessed = QDateTime::currentDateTime();
    entry.downloadCompleted = QDateTime::currentDateTime();

    {
        QMutexLocker locker(&m_mutex);
        m_cacheEntries[cacheKey] = entry;
        m_currentSize += entry.size;
    }

    // Save to database
    if (m_database) {
        m_database->recordFuseCacheEntry(cacheKey, cachePath, entry.size);
    }

    emit downloadCompleted(fileId, cachePath);
    emit cacheSizeChanged(m_currentSize);

    qInfo() << "FileCache: Downloaded and cached" << fileId << "(" << entry.size << "bytes)";

    return cachePath;
}

QString FileCache::getExportedPath(const QString& fileId, const QString& exportMimeType) {
    const QString cacheKey = cacheKeyFor(fileId, exportMimeType);
    const QString cachePath = generateCachePath(fileId, exportMimeType);

    if (!m_driveClient) {
        qWarning() << "FileCache: No GoogleDriveClient available for export";
        emit downloadFailedDetailed(
            fileId, QStringLiteral("No Google Drive client available for export"), 0);
        emit downloadFailed(fileId, QStringLiteral("No Google Drive client available for export"));
        return QString();
    }

    QFileInfo fileInfo(cachePath);
    QDir parentDir = fileInfo.dir();
    if (!parentDir.exists()) {
        parentDir.mkpath(".");
    }

    QList<PendingExportRequest> toStart;
    {
        QMutexLocker locker(&m_mutex);
        const QString readyPath = getReadyExportPathLocked(fileId, cacheKey, cachePath);
        if (!readyPath.isEmpty()) {
            qDebug() << "FileCache: Cache hit (export) for" << fileId;
            return readyPath;
        }

        if (m_pendingExportRequests.contains(cacheKey)) {
            const int queuedIndex = m_queuedExportKeys.indexOf(cacheKey);
            if (queuedIndex >= 0) {
                m_queuedExportKeys.removeAt(queuedIndex);
                m_activeExportKeys.insert(cacheKey);
                toStart.append(m_pendingExportRequests.value(cacheKey));
            }
        } else {
            PendingExportRequest request;
            request.fileId = fileId;
            request.exportMimeType = exportMimeType;
            request.cacheKey = cacheKey;
            request.cachePath = cachePath;

            qDebug() << "FileCache: Cache miss (export) for" << fileId << ", exporting as"
                     << exportMimeType;

            m_pendingDownloads[cacheKey] = false;
            m_pendingDownloadPaths[cachePath] = cacheKey;
            m_downloadErrors.remove(cacheKey);
            m_pendingExportRequests[cacheKey] = request;
            m_activeExportKeys.insert(cacheKey);
            toStart.append(request);
        }
    }

    startExportRequests(toStart);

    // Wait for export to complete (same mechanism as download)
    QString error;
    QList<PendingExportRequest> nextToStart;
    {
        QMutexLocker locker(&m_mutex);
        while (m_pendingDownloads.contains(cacheKey) && !m_pendingDownloads.value(cacheKey, true)) {
            if (!m_downloadCondition.wait(&m_mutex, 30000)) {
                qWarning() << "FileCache: Export timeout for" << fileId;
                error = QStringLiteral("Export timed out after 30 seconds");
                m_downloadErrors[cacheKey] = error;
                m_pendingDownloads.remove(cacheKey);
                m_pendingDownloadPaths.remove(cachePath);
                m_activeExportKeys.remove(cacheKey);
                m_pendingExportRequests.remove(cacheKey);
                m_queuedExportKeys.removeAll(cacheKey);
                nextToStart = collectQueuedExportsToStartLocked();
                break;
            }
        }

        if (m_downloadErrors.contains(cacheKey)) {
            error = m_downloadErrors.take(cacheKey);
            qWarning() << "FileCache: Export failed for" << fileId << ":" << error;
        }
    }

    startExportRequests(nextToStart);

    if (!error.isEmpty()) {
        if (error == QStringLiteral("Export timed out after 30 seconds")) {
            emit downloadFailedDetailed(fileId, error, 0);
            emit downloadFailed(fileId, error);
        }
        return QString();
    }

    {
        QMutexLocker locker(&m_mutex);
        return getReadyExportPathLocked(fileId, cacheKey, cachePath);
    }
}

void FileCache::queueExportedPath(const QString& fileId, const QString& exportMimeType) {
    if (exportMimeType.isEmpty() || !m_driveClient) {
        return;
    }

    const QString cacheKey = cacheKeyFor(fileId, exportMimeType);
    const QString cachePath = generateCachePath(fileId, exportMimeType);

    QFileInfo fileInfo(cachePath);
    QDir parentDir = fileInfo.dir();
    if (!parentDir.exists()) {
        parentDir.mkpath(".");
    }

    QList<PendingExportRequest> toStart;
    {
        QMutexLocker locker(&m_mutex);
        const QString readyPath = getReadyExportPathLocked(fileId, cacheKey, cachePath);
        if (!readyPath.isEmpty()) {
            return;
        }

        if (m_pendingExportRequests.contains(cacheKey)) {
            return;
        }

        PendingExportRequest request;
        request.fileId = fileId;
        request.exportMimeType = exportMimeType;
        request.cacheKey = cacheKey;
        request.cachePath = cachePath;

        m_pendingDownloads[cacheKey] = false;
        m_pendingDownloadPaths[cachePath] = cacheKey;
        m_downloadErrors.remove(cacheKey);
        m_pendingExportRequests[cacheKey] = request;

        if (m_activeExportKeys.size() < MAX_BACKGROUND_EXPORTS) {
            m_activeExportKeys.insert(cacheKey);
            toStart.append(request);
        } else {
            m_queuedExportKeys.append(cacheKey);
        }
    }

    startExportRequests(toStart);
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

    QList<QString> cacheKeys;
    for (auto it = m_cacheEntries.constBegin(); it != m_cacheEntries.constEnd(); ++it) {
        if (cacheKeyBelongsToFile(it.key(), fileId)) {
            cacheKeys.append(it.key());
        }
    }

    if (cacheKeys.isEmpty()) {
        return;
    }

    // C1 fix: Never delete a dirty file — local modifications would be lost.
    // The file will be uploaded by DirtySyncWorker, then re-downloaded on next access.
    if (m_dirtyFiles.contains(fileId)) {
        qWarning() << "FileCache: Skipping invalidation of dirty file" << fileId
                   << "— local changes pending upload";
        return;
    }

    // Fix 2: Never invalidate a file with open FUSE handles — readers/writers
    // would get I/O errors on their next system call.
    if (m_openHandleCounts.value(fileId, 0) > 0) {
        qWarning() << "FileCache: Skipping invalidation of file" << fileId
                   << "— open FUSE handles exist";
        return;
    }

    for (const QString& cacheKey : cacheKeys) {
        CacheEntry entry = m_cacheEntries.take(cacheKey);

        if (QFile::exists(entry.cachePath)) {
            QFile::remove(entry.cachePath);
        }

        m_currentSize -= entry.size;
        if (m_currentSize < 0) {
            m_currentSize = 0;
        }

        if (m_database) {
            m_database->evictFuseCacheEntry(cacheKey);
        }
    }

    emit fileEvicted(fileId);
    emit cacheSizeChanged(m_currentSize);
}

void FileCache::removeFromCache(const QString& fileId) {
    // Same as invalidate, but also clear dirty status
    QMutexLocker locker(&m_mutex);

    qDebug() << "FileCache: Removing" << fileId << "from cache";

    // Remove cache entry
    QList<QString> cacheKeys;
    for (auto it = m_cacheEntries.constBegin(); it != m_cacheEntries.constEnd(); ++it) {
        if (cacheKeyBelongsToFile(it.key(), fileId)) {
            cacheKeys.append(it.key());
        }
    }

    for (const QString& cacheKey : cacheKeys) {
        CacheEntry entry = m_cacheEntries.take(cacheKey);

        if (QFile::exists(entry.cachePath)) {
            QFile::remove(entry.cachePath);
        }

        m_currentSize -= entry.size;
        if (m_currentSize < 0) {
            m_currentSize = 0;
        }

        if (m_database) {
            m_database->evictFuseCacheEntry(cacheKey);
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
        if (m_dirtyFiles.contains(it.second.fileId)) {
            qWarning() << "FileCache: Skipping delete of dirty file in clearCache:"
                       << it.second.fileId;
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
    entry.generation = m_dirtyFiles.contains(fileId) ? m_dirtyFiles[fileId].generation + 1 : 1;
    entry.uploadedGeneration = 0;

    m_dirtyFiles[fileId] = entry;

    // Record in database
    if (m_database) {
        m_database->markFuseDirty(fileId, path);
    }

    emit fileDirty(fileId, path);
}

bool FileCache::clearDirty(const QString& fileId, quint64 expectedGeneration) {
    QMutexLocker locker(&m_mutex);

    return clearDirtyLocked(fileId, expectedGeneration);
}

UploadSnapshotResult FileCache::createUploadSnapshot(const QString& fileId,
                                                     quint64 expectedGeneration) {
    QMutexLocker locker(&m_mutex);

    UploadSnapshotResult result;

    auto dirtyIt = m_dirtyFiles.find(fileId);
    if (dirtyIt == m_dirtyFiles.end()) {
        result.status = UploadSnapshotStatus::StaleGeneration;
        return result;
    }

    if (expectedGeneration != 0 && dirtyIt->generation != expectedGeneration) {
        result.status = UploadSnapshotStatus::StaleGeneration;
        return result;
    }

    if (expectedGeneration != 0 && dirtyIt->uploadedGeneration >= expectedGeneration) {
        result.status = UploadSnapshotStatus::AlreadyUploaded;
        return result;
    }

    if (m_openWritableHandleCounts.value(fileId, 0) > 0) {
        result.status = UploadSnapshotStatus::BlockedByWriter;
        return result;
    }

    const quint64 snapshotGeneration =
        expectedGeneration != 0 ? expectedGeneration : dirtyIt->generation;
    const QString sourcePath = getContentPathLocked(fileId);
    if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
        result.status = UploadSnapshotStatus::MissingContent;
        return result;
    }

    const QString snapshotPath = generateUploadSnapshotPath(fileId, snapshotGeneration);
    if (!QDir().mkpath(QFileInfo(snapshotPath).path())) {
        qWarning() << "FileCache: Could not create snapshot directory for" << fileId;
        result.status = UploadSnapshotStatus::Failed;
        return result;
    }

    if (QFile::exists(snapshotPath) && !QFile::remove(snapshotPath)) {
        qWarning() << "FileCache: Could not remove stale upload snapshot for" << fileId;
        result.status = UploadSnapshotStatus::Failed;
        return result;
    }

    const QString pendingPath = generateDirtyPath(fileId);
    const bool sourceIsPending = (sourcePath == pendingPath);

    if (sourceIsPending) {
        if (!QFile::rename(pendingPath, snapshotPath)) {
            qWarning() << "FileCache: Could not move pending file into snapshot for" << fileId;
            result.status = UploadSnapshotStatus::Failed;
            return result;
        }

        if (!QFile::copy(snapshotPath, pendingPath)) {
            qWarning() << "FileCache: Could not restore pending file after snapshot for" << fileId;
            if (!QFile::rename(snapshotPath, pendingPath)) {
                qCritical() << "FileCache: Snapshot restore failed for" << fileId
                            << "- pending content left only at" << snapshotPath;
            }
            result.status = UploadSnapshotStatus::Failed;
            return result;
        }
    } else {
        if (!QFile::copy(sourcePath, snapshotPath)) {
            qWarning() << "FileCache: Could not copy upload snapshot for" << fileId;
            result.status = UploadSnapshotStatus::Failed;
            return result;
        }
    }

    result.status = UploadSnapshotStatus::Ready;
    result.snapshotPath = snapshotPath;
    return result;
}

void FileCache::cleanupUploadSnapshot(const QString& snapshotPath) {
    if (snapshotPath.isEmpty()) {
        return;
    }

    if (QFile::exists(snapshotPath) && !QFile::remove(snapshotPath)) {
        qWarning() << "FileCache: Could not remove upload snapshot" << snapshotPath;
    }
}

bool FileCache::finalizeUploadedGeneration(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    return maybeFinalizeUploadedGenerationLocked(fileId);
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
        qint64 sizeBefore = m_currentSize;
        evictLRU();
        // If evictLRU could not evict anything (all remaining entries are
        // dirty or have open handles), break to avoid an infinite loop.
        if (m_currentSize == sizeBefore) {
            break;
        }
        available = m_maxCacheSize - m_currentSize;
    }

    return available >= bytesNeeded;
}

bool FileCache::recordCacheEntry(const QString& fileId, const QString& localPath, qint64 size) {
    QMutexLocker locker(&m_mutex);

    // Check if we need to evict first
    if (m_currentSize + size > m_maxCacheSize) {
        while (m_currentSize + size > m_maxCacheSize && !m_cacheEntries.isEmpty()) {
            qint64 sizeBefore = m_currentSize;
            evictLRU();
            if (m_currentSize == sizeBefore) {
                break;
            }
        }
    }

    CacheEntry entry;
    entry.fileId = fileId;
    entry.cacheKey = fileId;
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
// Self-Upload Tracking (Fix 1)
// ============================================================================

void FileCache::markRecentlyUploaded(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    m_recentlyUploaded.insert(fileId);
}

bool FileCache::consumeRecentlyUploaded(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    return m_recentlyUploaded.remove(fileId);
}

// ============================================================================
// Open-Handle Tracking (Fix 2)
// ============================================================================

void FileCache::addOpenHandle(const QString& fileId, bool writable) {
    QMutexLocker locker(&m_mutex);
    m_openHandleCounts[fileId]++;
    if (writable) {
        m_openWritableHandleCounts[fileId]++;
    }
}

void FileCache::removeOpenHandle(const QString& fileId, bool writable) {
    QMutexLocker locker(&m_mutex);
    auto it = m_openHandleCounts.find(fileId);
    if (it != m_openHandleCounts.end()) {
        if (--(*it) <= 0) {
            m_openHandleCounts.erase(it);
        }
    }

    if (writable) {
        auto writableIt = m_openWritableHandleCounts.find(fileId);
        if (writableIt != m_openWritableHandleCounts.end()) {
            if (--(*writableIt) <= 0) {
                m_openWritableHandleCounts.erase(writableIt);
            }
        }
    }

    maybeFinalizeUploadedGenerationLocked(fileId);
}

bool FileCache::hasOpenHandles(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    return m_openHandleCounts.value(fileId, 0) > 0;
}

bool FileCache::hasOpenWritableHandles(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    return m_openWritableHandleCounts.value(fileId, 0) > 0;
}

void FileCache::markUploadedGeneration(const QString& fileId, quint64 generation) {
    QMutexLocker locker(&m_mutex);

    auto it = m_dirtyFiles.find(fileId);
    if (it == m_dirtyFiles.end()) {
        return;
    }

    if (generation >= it->uploadedGeneration) {
        it->uploadedGeneration = generation;
    }
}

bool FileCache::hasUploadedGeneration(const QString& fileId, quint64 generation) const {
    QMutexLocker locker(&m_mutex);

    auto it = m_dirtyFiles.constFind(fileId);
    if (it == m_dirtyFiles.constEnd()) {
        return false;
    }

    return generation != 0 && it->uploadedGeneration >= generation;
}

// ============================================================================
// Private Slots
// ============================================================================

void FileCache::onFileDownloaded(const QString& fileId, const QString& localPath) {
    QList<PendingExportRequest> toStart;
    QString error;
    QString completedPath;
    qint64 cacheSizeAfter = 0;
    bool emitCompleted = false;
    bool emitCacheSize = false;

    {
        QMutexLocker locker(&m_mutex);

        QString cacheKey =
            localPath.isEmpty() ? QString() : m_pendingDownloadPaths.value(localPath);
        if (!cacheKey.isEmpty() && m_pendingExportRequests.contains(cacheKey)) {
            const PendingExportRequest request = m_pendingExportRequests.value(cacheKey);
            m_activeExportKeys.remove(cacheKey);

            QFileInfo exportedFile(localPath);
            if (!exportedFile.exists()) {
                error = QStringLiteral("Export completed but no file was written");
            } else if (exportedFile.size() <= 0) {
                qWarning() << "FileCache: Export for" << request.fileId
                           << "produced a zero-byte file, discarding";
                QFile::remove(localPath);
                error = QStringLiteral("Export produced an empty file");
            } else {
                emitCacheSize =
                    recordCacheEntryLocked(cacheKey, request.fileId, localPath, exportedFile.size(),
                                           QDateTime::currentDateTime());
                completedPath = localPath;
                cacheSizeAfter = m_currentSize;
                emitCompleted = true;
            }

            if (!error.isEmpty()) {
                m_downloadErrors[cacheKey] = error;
            }

            m_pendingDownloads.remove(cacheKey);
            m_pendingDownloadPaths.remove(localPath);
            m_pendingExportRequests.remove(cacheKey);
            toStart = collectQueuedExportsToStartLocked();
            m_downloadCondition.wakeAll();
        } else {
            if (cacheKey.isEmpty() && m_pendingDownloads.contains(fileId)) {
                cacheKey = fileId;
            }

            // Mark download as completed successfully
            if (!cacheKey.isEmpty() && m_pendingDownloads.contains(cacheKey)) {
                m_pendingDownloads[cacheKey] = true;  // Mark as completed
                m_downloadCondition.wakeAll();
            }
            return;
        }
    }

    if (!error.isEmpty()) {
        qWarning() << "FileCache: Export failed for" << fileId << ":" << error;
        emit downloadFailedDetailed(fileId, error, 0);
        emit downloadFailed(fileId, error);
    } else if (emitCompleted) {
        emit downloadCompleted(fileId, completedPath);
        if (emitCacheSize) {
            emit cacheSizeChanged(cacheSizeAfter);
        }
        qInfo() << "FileCache: Exported and cached" << fileId << "("
                << QFileInfo(completedPath).size() << "bytes)";
    }

    startExportRequests(toStart);
}

void FileCache::onDownloadError(const QString& operation, const QString& errorMsg, int httpStatus,
                                const QString& fileId, const QString& localPath) {
    // Check if this is a download or export error
    if (!operation.startsWith("download") && !operation.startsWith("export")) {
        return;
    }

    QList<PendingExportRequest> toStart;
    bool handledExport = false;

    {
        QMutexLocker locker(&m_mutex);

        QString cacheKey =
            localPath.isEmpty() ? QString() : m_pendingDownloadPaths.value(localPath);
        if (!cacheKey.isEmpty() && m_pendingExportRequests.contains(cacheKey)) {
            m_downloadErrors[cacheKey] = errorMsg;
            m_pendingDownloads.remove(cacheKey);
            m_pendingDownloadPaths.remove(localPath);
            m_activeExportKeys.remove(cacheKey);
            m_pendingExportRequests.remove(cacheKey);
            toStart = collectQueuedExportsToStartLocked();
            m_downloadCondition.wakeAll();
            handledExport = true;
        } else if (operation.startsWith("export")) {
            return;
        } else {
            if (cacheKey.isEmpty() && !fileId.isEmpty() && m_pendingDownloads.contains(fileId)) {
                cacheKey = fileId;
            }

            // The errorDetailed signal provides fileId/localPath via tagReply(),
            // so use the local path first to disambiguate exported representations.
            if (!cacheKey.isEmpty() && m_pendingDownloads.contains(cacheKey)) {
                m_downloadErrors[cacheKey] = errorMsg;
                m_pendingDownloads[cacheKey] = true;  // Mark as completed (with error)
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
                    qWarning() << "FileCache: Download error without file ID association:"
                               << operation << errorMsg;

                    for (auto it = m_pendingDownloads.begin(); it != m_pendingDownloads.end();
                         ++it) {
                        if (!it.value()) {  // Still in progress
                            m_downloadErrors[it.key()] = errorMsg;
                            it.value() = true;  // Mark as completed (with error)
                        }
                    }
                }
            }

            m_downloadCondition.wakeAll();
        }
    }

    if (handledExport) {
        qWarning() << "FileCache: Export failed for" << fileId << ":" << errorMsg;
        emit downloadFailedDetailed(fileId, errorMsg, httpStatus);
        emit downloadFailed(fileId, errorMsg);
        startExportRequests(toStart);
    }
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

QString FileCache::generateCachePath(const QString& fileId, const QString& exportMimeType) const {
    // For exports, hash fileId + exportMimeType so different representations
    // get distinct on-disk paths even if the in-memory map is keyed by fileId.
    QByteArray combined = (fileId + "|" + exportMimeType).toUtf8();
    QByteArray hash = QCryptographicHash::hash(combined, QCryptographicHash::Sha256).toHex();

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

QString FileCache::generateUploadSnapshotPath(const QString& fileId, quint64 generation) const {
    QByteArray hash = QCryptographicHash::hash(fileId.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString subDir = QString::fromLatin1(hash.left(2));
    return m_dirtyDirectory + "/snapshots/" + subDir + "/" + QString::fromLatin1(hash) + "." +
           QString::number(generation);
}

QString FileCache::getReadyExportPathLocked(const QString& fileId, const QString& cacheKey,
                                            const QString& cachePath) {
    if (m_cacheEntries.contains(cacheKey)) {
        const CacheEntry entry = m_cacheEntries.value(cacheKey);
        if (hasUsableExportBytes(entry.cachePath)) {
            m_cacheEntries[cacheKey].lastAccessed = QDateTime::currentDateTime();
            if (m_database) {
                m_database->updateCacheAccessTime(cacheKey);
            }
            return entry.cachePath;
        }

        QFile::remove(entry.cachePath);
        m_currentSize -= entry.size;
        m_cacheEntries.remove(cacheKey);
        if (m_database) {
            m_database->evictFuseCacheEntry(cacheKey);
        }
    }

    if (!hasUsableExportBytes(cachePath)) {
        return QString();
    }

    QFileInfo exportedFile(cachePath);
    recordCacheEntryLocked(cacheKey, fileId, cachePath, exportedFile.size(),
                           exportedFile.lastModified());
    return cachePath;
}

bool FileCache::recordCacheEntryLocked(const QString& cacheKey, const QString& fileId,
                                       const QString& cachePath, qint64 size,
                                       const QDateTime& completedAt) {
    const bool hadEntry = m_cacheEntries.contains(cacheKey);
    const qint64 oldSize = hadEntry ? m_cacheEntries.value(cacheKey).size : 0;
    if (hadEntry) {
        m_currentSize -= oldSize;
    }

    CacheEntry entry;
    entry.fileId = fileId;
    entry.cacheKey = cacheKey;
    entry.cachePath = cachePath;
    entry.size = size;
    entry.lastAccessed = QDateTime::currentDateTime();
    entry.downloadCompleted = completedAt;

    m_cacheEntries[cacheKey] = entry;
    m_currentSize += size;
    if (m_database) {
        m_database->recordFuseCacheEntry(cacheKey, cachePath, size);
    }

    return !hadEntry || oldSize != size;
}

QList<PendingExportRequest> FileCache::collectQueuedExportsToStartLocked() {
    QList<PendingExportRequest> requests;

    while (m_activeExportKeys.size() < MAX_BACKGROUND_EXPORTS && !m_queuedExportKeys.isEmpty()) {
        const QString cacheKey = m_queuedExportKeys.takeFirst();
        if (!m_pendingExportRequests.contains(cacheKey)) {
            continue;
        }

        m_activeExportKeys.insert(cacheKey);
        requests.append(m_pendingExportRequests.value(cacheKey));
    }

    return requests;
}

void FileCache::startExportRequests(const QList<PendingExportRequest>& requests) {
    if (requests.isEmpty() || !m_driveClient) {
        return;
    }

    for (const PendingExportRequest& request : requests) {
        emit downloadStarted(request.fileId);
        QMetaObject::invokeMethod(
            m_driveClient,
            [driveClient = m_driveClient, request]() {
                driveClient->exportFile(request.fileId, request.exportMimeType, request.cachePath);
            },
            Qt::QueuedConnection);
    }
}

QString FileCache::getContentPath(const QString& fileId) const {
    return getContentPath(fileId, QString());
}

QString FileCache::getContentPath(const QString& fileId, const QString& exportMimeType) const {
    QMutexLocker locker(&m_mutex);

    return getContentPathLocked(fileId, exportMimeType);
}

QString FileCache::getContentPathLocked(const QString& fileId,
                                        const QString& exportMimeType) const {
    const QString cacheKey = cacheKeyFor(fileId, exportMimeType);

    // If dirty, the authoritative content is in the pending store (post-release)
    // or still in the cache dir (write handle still open).
    if (exportMimeType.isEmpty() && m_dirtyFiles.contains(fileId)) {
        QString pendingPath = generateDirtyPath(fileId);
        if (QFile::exists(pendingPath)) {
            return pendingPath;
        }
        // Fall through: file is dirty but not yet moved (still open for writing)
    }

    // For clean files, or dirty files whose handle is still open, return the
    // deterministic cache path (which may or may not exist on disk right now).
    if (m_cacheEntries.contains(cacheKey)) {
        return m_cacheEntries[cacheKey].cachePath;
    }

    return exportMimeType.isEmpty() ? generateCachePath(fileId)
                                    : generateCachePath(fileId, exportMimeType);
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

bool FileCache::clearDirtyLocked(const QString& fileId, quint64 expectedGeneration) {
    auto dirtyIt = m_dirtyFiles.find(fileId);
    if (dirtyIt == m_dirtyFiles.end()) {
        return false;
    }

    if (expectedGeneration != 0 && dirtyIt->generation != expectedGeneration) {
        qInfo() << "FileCache: Keeping dirty state for" << fileId
                << "- newer local writes landed during upload";
        return false;
    }

    if (m_openWritableHandleCounts.value(fileId, 0) > 0) {
        qInfo() << "FileCache: Keeping dirty state for" << fileId
                << "- writable FUSE handles still exist for this generation";
        return false;
    }

    qDebug() << "FileCache: Clearing dirty flag for" << fileId;

    if (!recycleAuthoritativeCopyToCacheLocked(fileId)) {
        return false;
    }

    m_dirtyFiles.erase(dirtyIt);

    if (m_database) {
        m_database->clearFuseDirty(fileId);
    }

    return true;
}

bool FileCache::maybeFinalizeUploadedGenerationLocked(const QString& fileId) {
    auto dirtyIt = m_dirtyFiles.find(fileId);
    if (dirtyIt == m_dirtyFiles.end()) {
        return false;
    }

    if (dirtyIt->uploadedGeneration < dirtyIt->generation) {
        return false;
    }

    if (m_openWritableHandleCounts.value(fileId, 0) > 0) {
        return false;
    }

    return clearDirtyLocked(fileId, dirtyIt->generation);
}

bool FileCache::recycleAuthoritativeCopyToCacheLocked(const QString& fileId) {
    // Recycle the pending-store file back into the LRU cache so the content
    // remains available for immediate reads. Without this there is a window
    // where no on-disk content exists — stat() would report the stale remote
    // size and reads would require a full re-download from Drive.
    const QString pendingPath = generateDirtyPath(fileId);
    const QString cachePath = generateCachePath(fileId);
    if (QFile::exists(pendingPath)) {
        QFileInfo pendingInfo(pendingPath);

        if (!QDir().mkpath(QFileInfo(cachePath).path())) {
            qWarning()
                << "FileCache: Could not create cache directory while clearing dirty state for"
                << fileId;
            return false;
        }

        if (QFile::exists(cachePath) && !QFile::remove(cachePath)) {
            qWarning() << "FileCache: Could not replace stale cache copy for" << fileId;
            return false;
        }

        bool recycled = QFile::rename(pendingPath, cachePath);
        if (!recycled) {
            recycled = QFile::copy(pendingPath, cachePath);
            if (recycled) {
                QFile::remove(pendingPath);
            }
        }

        if (!recycled) {
            qWarning() << "FileCache: Could not recycle pending file for" << fileId
                       << "- keeping dirty state so the pending content remains authoritative";
            return false;
        }

        qDebug() << "FileCache: Recycled pending file back into cache for" << fileId << "("
                 << pendingInfo.size() << "bytes)";
    }

    QFileInfo localInfo(cachePath);
    if (localInfo.exists()) {
        if (m_cacheEntries.contains(fileId)) {
            m_currentSize -= m_cacheEntries[fileId].size;
            if (m_currentSize < 0) {
                m_currentSize = 0;
            }
        }

        CacheEntry entry;
        entry.fileId = fileId;
        entry.cacheKey = fileId;
        entry.cachePath = cachePath;
        entry.size = localInfo.size();
        entry.lastAccessed = QDateTime::currentDateTime();
        entry.downloadCompleted = QDateTime::currentDateTime();
        m_cacheEntries[fileId] = entry;
        m_currentSize += entry.size;

        if (m_database) {
            m_database->recordFuseCacheEntry(fileId, cachePath, entry.size);
        }
    }

    return true;
}

bool FileCache::resetSnapshotDirectoryLocked() {
    const QString snapshotRoot = m_dirtyDirectory + "/snapshots";
    QDir snapshotDir(snapshotRoot);
    if (snapshotDir.exists() && !snapshotDir.removeRecursively()) {
        qWarning() << "FileCache: Failed to reset upload snapshot directory:" << snapshotRoot;
        return false;
    }

    if (!QDir().mkpath(snapshotRoot)) {
        qWarning() << "FileCache: Failed to create upload snapshot directory:" << snapshotRoot;
        return false;
    }

    return true;
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
            entry.fileId = remoteFileIdFromCacheKey(dbEntry.fileId);
            entry.cacheKey = dbEntry.fileId;
            entry.cachePath = dbEntry.cachePath;
            entry.size = dbEntry.size;
            entry.lastAccessed = dbEntry.lastAccessed;
            entry.downloadCompleted = dbEntry.downloadCompleted;

            m_cacheEntries[entry.cacheKey] = entry;
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
        entry.generation = 1;
        entry.uploadedGeneration = 0;

        m_dirtyFiles[entry.fileId] = entry;
    }

    qDebug() << "FileCache: Loaded" << m_cacheEntries.size() << "cache entries from database";
    qDebug() << "FileCache: Loaded" << m_dirtyFiles.size() << "dirty file entries from database";
}

void FileCache::evictLRU() {
    // Find least recently used entry that is NOT dirty
    QString lruFileId;
    QDateTime lruTime;

    for (auto it = m_cacheEntries.constBegin(); it != m_cacheEntries.constEnd(); ++it) {
        // Skip dirty files - they must not be evicted
        if (m_dirtyFiles.contains(it.value().fileId)) {
            continue;
        }

        // Fix 2: Skip files with open FUSE handles
        if (m_openHandleCounts.value(it.value().fileId, 0) > 0) {
            continue;
        }

        if (lruFileId.isEmpty() || it.value().lastAccessed < lruTime) {
            lruTime = it.value().lastAccessed;
            lruFileId = it.key();
        }
    }

    if (lruFileId.isEmpty()) {
        qWarning() << "FileCache: Cannot evict - all cached files are dirty or have open handles";
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
