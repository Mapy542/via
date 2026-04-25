/**
 * @file FileCache.h
 * @brief LRU file cache manager for FUSE filesystem
 *
 * Manages on-demand file caching for the FUSE virtual filesystem.
 * Files are downloaded when accessed and cached locally with LRU eviction.
 * Dirty (locally modified) files are tracked for later upload.
 *
 * Key responsibilities (from FUSE Procedure Flow Chart):
 * - Download files on-demand when opened
 * - Track cache entries in FuseDatabase.fuse_cache_entries
 * - Implement LRU eviction when cache exceeds size limit
 * - Mark files as dirty when written to
 * - Track dirty files in FuseDatabase.fuse_dirty_files
 * - Coordinate with MetadataCache for file metadata
 * - Queue downloads/uploads through SyncQueue
 */

#ifndef FILECACHE_H
#define FILECACHE_H

#include <QDateTime>
#include <QDir>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QWaitCondition>

class SyncDatabase;
class GoogleDriveClient;

/**
 * @struct CacheEntry
 * @brief Information about a cached file
 *
 * Maps to FuseDatabase.fuse_cache_entries schema:
 * - file_id TEXT PRIMARY KEY
 * - cache_path TEXT NOT NULL
 * - size INTEGER NOT NULL
 * - last_accessed TEXT NOT NULL
 * - download_completed TEXT NOT NULL
 */
struct CacheEntry {
    QString fileId;               ///< Google Drive file ID
    QString cacheKey;             ///< Unique cache identity (raw fileId or fileId|exportMimeType)
    QString cachePath;            ///< Local cache path
    qint64 size;                  ///< File size in bytes
    QDateTime lastAccessed;       ///< Last access time (for LRU)
    QDateTime downloadCompleted;  ///< When download finished
};

/**
 * @struct DirtyFileEntry
 * @brief Information about a dirty (modified) file pending upload
 *
 * Maps to FuseDatabase.fuse_dirty_files schema:
 * - file_id TEXT PRIMARY KEY
 * - path TEXT NOT NULL
 * - marked_dirty_at TEXT NOT NULL
 * - last_upload_attempt TEXT
 * - upload_failed INTEGER DEFAULT 0
 */
struct DirtyFileEntry {
    QString fileId;                  ///< Google Drive file ID
    QString path;                    ///< Logical path in FUSE filesystem
    QDateTime markedDirtyAt;         ///< When file was marked dirty
    QDateTime lastUploadAttempt;     ///< Last upload attempt time
    bool uploadFailed;               ///< Whether last upload failed
    quint64 generation = 0;          ///< In-memory version to detect stale uploads
    quint64 uploadedGeneration = 0;  ///< Last local generation already uploaded to Drive
};

enum class UploadSnapshotStatus {
    Ready,
    AlreadyUploaded,
    BlockedByWriter,
    StaleGeneration,
    MissingContent,
    Failed
};

struct UploadSnapshotResult {
    UploadSnapshotStatus status = UploadSnapshotStatus::Failed;
    QString snapshotPath;
};

struct PendingExportRequest {
    QString fileId;
    QString exportMimeType;
    QString cacheKey;
    QString cachePath;
};

Q_DECLARE_METATYPE(CacheEntry)
Q_DECLARE_METATYPE(DirtyFileEntry)

/**
 * @class FileCache
 * @brief LRU cache manager for FUSE file contents
 *
 * The FileCache provides on-demand file caching for the FUSE filesystem.
 * It downloads files when they are first opened and stores them locally
 * in ~/.cache/Via/files/<fileId>.
 *
 * Thread Safety:
 * - All public methods are thread-safe using internal mutex
 * - Download operations can block waiting for network completion
 * - Cache eviction runs automatically when space is needed
 *
 * Integration Points (from FUSE Procedure Flow Chart):
 * - FuseDriver: calls open/read/write which use this cache
 * - SyncDatabase: stores cache entries and dirty files
 * - GoogleDriveClient: downloads file contents
 * - DirtySyncWorker: queries dirty files for upload
 * - MetadataRefreshWorker: calls invalidate() for stale entries
 */
class FileCache : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the file cache
     * @param database Pointer to sync database (for fuse_cache_entries table)
     * @param driveClient Pointer to Google Drive API client
     * @param parent Parent QObject
     */
    explicit FileCache(SyncDatabase* database, GoogleDriveClient* driveClient,
                       QObject* parent = nullptr);

    ~FileCache() override;

    /**
     * @brief Initialize the cache
     *
     * Creates cache directory if needed and loads cache index from database.
     * Must be called before using other methods.
     *
     * @return true if initialization successful
     */
    bool initialize();

    // ========================================================================
    // Cache & Pending-Store Configuration
    // ========================================================================

    /**
     * @brief Get the cache directory path
     * @return Cache directory path (default: ~/.cache/Via/files/)
     */
    QString cacheDirectory() const;

    /**
     * @brief Set the cache directory path
     * @param path New cache directory path
     */
    void setCacheDirectory(const QString& path);

    /**
     * @brief Get the pending-uploads directory path
     *
     * Dirty files are moved here on file-handle close so they are stored
     * in persistent XDG_DATA_HOME (~/.local/share/via/pending/) rather
     * than the evictable XDG_CACHE_HOME.
     *
     * @return Pending directory path
     */
    QString dirtyDirectory() const;

    /**
     * @brief Set the pending-uploads directory path
     * @param path New pending directory path
     */
    void setDirtyDirectory(const QString& path);

    /**
     * @brief Get maximum cache size
     * @return Maximum size in bytes (default: 10GB as per procedure chart)
     */
    qint64 maxCacheSize() const;

    /**
     * @brief Set maximum cache size
     *
     * If current size exceeds new max, LRU eviction runs automatically.
     *
     * @param bytes Maximum size in bytes
     */
    void setMaxCacheSize(qint64 bytes);

    /**
     * @brief Get current cache size
     * @return Current size in bytes
     */
    qint64 currentCacheSize() const;

    // ========================================================================
    // Cache Operations (from FUSE Procedure Flow Chart: open flow)
    // ========================================================================

    /**
     * @brief Check if a file is cached and fresh
     *
     * Used by fuse_open to determine if download is needed.
     * Checks: file exists in cache, download completed, not stale.
     *
     * @param fileId Google Drive file ID
     * @return true if file is in cache and fresh
     */
    bool isCached(const QString& fileId) const;

    /**
     * @brief Check if a specific exported representation is cached and fresh
     * @param fileId Google Drive file ID
     * @param exportMimeType Target MIME type for the export
     * @return true if the exported representation is in cache and fresh
     */
    bool isCached(const QString& fileId, const QString& exportMimeType) const;

    /**
     * @brief Get cached file path (downloads if not cached)
     *
     * This is the main entry point from fuse_open. It:
     * 1. Checks if file is in cache
     * 2. If not cached, downloads via GoogleDriveClient
     * 3. Records entry in fuse_cache_entries table
     * 4. Triggers LRU eviction if cache size exceeded
     * 5. Updates last_accessed time
     *
     * This method may BLOCK waiting for download to complete.
     *
     * @param fileId Google Drive file ID
     * @param expectedSize Expected file size (for cache eviction)
     * @return Local cache path, or empty string on failure
     */
    QString getCachedPath(const QString& fileId, qint64 expectedSize = 0);

    /**
     * @brief Export a Google-native doc and return cached path
     *
     * Like getCachedPath but uses the Drive export endpoint instead of
     * direct download. The exported file is stored in the normal LRU cache.
     * This method may BLOCK waiting for export to complete.
     *
     * @param fileId Google Drive file ID
     * @param exportMimeType Target MIME type for the export
     * @return Local cache path, or empty string on failure
     */
    QString getExportedPath(const QString& fileId, const QString& exportMimeType);

    /**
     * @brief Queue a background export for a Google-native doc
     *
     * Starts immediately when a prefetch slot is available, otherwise queues
     * the request behind existing background exports. Repeated calls for the
     * same representation are deduplicated.
     *
     * @param fileId Google Drive file ID
     * @param exportMimeType Target MIME type for the export
     */
    void queueExportedPath(const QString& fileId, const QString& exportMimeType);

    /**
     * @brief Get cache path without downloading
     *
     * Returns the path where a file would be cached, without triggering
     * a download. Used for write operations where file is created locally.
     *
     * @param fileId Google Drive file ID
     * @return Cache path for the file
     */
    QString getCachePathForFile(const QString& fileId) const;

    /**
     * @brief Get the deterministic path in the pending store for a file
     *
     * Returns where a dirty file would be stored in the pending store
     * (XDG_DATA_HOME/via/pending/...). Does not check whether the file
     * exists. Useful for tests and for callers that manage the pending
     * store directly.
     *
     * @param fileId Google Drive file ID
     * @return Pending-store path for the file
     */
    QString getDirtyPathForFile(const QString& fileId) const;

    /**
     * @brief Get the authoritative on-disk path for a file's current content
     *
     * Returns the pending-store path when the file is dirty (i.e. has been
     * moved out of the LRU cache into persistent storage after the write
     * handle was released). Falls back to the deterministic cache path when
     * the file is not dirty or has not yet been moved.
     *
     * Use this everywhere content needs to be read or uploaded — it is
     * the single source of truth for "where are the bytes right now?".
     *
     * @param fileId Google Drive file ID
     * @return Absolute path to the file's content, may not exist on disk
     */
    QString getContentPath(const QString& fileId) const;

    /**
     * @brief Get the authoritative on-disk path for an exported representation
     * @param fileId Google Drive file ID
     * @param exportMimeType Target MIME type for the export
     * @return Absolute path to the exported content, may not exist on disk yet
     */
    QString getContentPath(const QString& fileId, const QString& exportMimeType) const;

    /**
     * @brief Move dirty file content from LRU cache into persistent store
     *
     * Renames the cached file to the pending-uploads directory
     * (~/.local/share/via/pending/) and removes the cache entry so that
     * LRU eviction and clearCache() cannot touch it.  The dirty-files
     * record is preserved.
     *
     * Called from FuseDriver::fuseRelease() when a dirty file handle is
     * closed.  Stable fd-based FuseOpenFile handles remain valid across the
     * rename, so callers only need the returned path for path-based metadata
     * or logging decisions.
     *
     * @param fileId Google Drive file ID
     * @return New on-disk path on success, or empty string on failure
     */
    QString moveToDirtyStore(const QString& fileId);

    /**
     * @brief Update last accessed time for a cached file
     *
     * Called from fuse_read to update LRU tracking.
     * Updates both in-memory and database (fuse_cache_entries.last_accessed).
     *
     * @param fileId Google Drive file ID
     */
    void updateAccessTime(const QString& fileId);

    /**
     * @brief Invalidate a cache entry (mark as stale)
     *
     * Called from MetadataRefreshWorker when remote file has changed.
     * The file is removed from cache and will be re-downloaded on next access.
     *
     * @param fileId Google Drive file ID
     */
    void invalidate(const QString& fileId);

    /**
     * @brief Remove a file from cache
     *
     * Removes both the cached file on disk and the database entry.
     * Used when file is deleted via fuse_unlink.
     *
     * @param fileId Google Drive file ID
     */
    void removeFromCache(const QString& fileId);

    /**
     * @brief Clear the entire cache
     *
     * Removes all cached files and database entries.
     * Used for cleanup or settings reset.
     */
    void clearCache();

    // ========================================================================
    // Dirty File Tracking (from FUSE Procedure Flow Chart: write flow)
    // ========================================================================

    /**
     * @brief Mark a cached file as dirty (needs upload)
     *
     * Called from fuse_write when file is modified.
     * Records entry in fuse_dirty_files table.
     *
     * @param fileId Google Drive file ID
     * @param path Logical path in FUSE filesystem
     */
    void markDirty(const QString& fileId, const QString& path);

    /**
     * @brief Clear dirty flag for a file
     *
     * Called after successful upload by DirtySyncWorker.
     * Removes entry from fuse_dirty_files table.
     *
     * @param fileId Google Drive file ID
     */
    bool clearDirty(const QString& fileId, quint64 expectedGeneration = 0);

    /**
     * @brief Mark a dirty file upload as failed
     *
     * Called by DirtySyncWorker when upload fails.
     * Updates fuse_dirty_files.upload_failed flag.
     *
     * @param fileId Google Drive file ID
     */
    void markUploadFailed(const QString& fileId);

    /**
     * @brief Check if a file is dirty
     * @param fileId Google Drive file ID
     * @return true if file has local modifications pending upload
     */
    bool isDirty(const QString& fileId) const;

    /**
     * @brief Get list of all dirty files
     *
     * Used by DirtySyncWorker to find files needing upload.
     *
     * @return List of dirty file entries
     */
    QList<DirtyFileEntry> getDirtyFiles() const;

    /**
     * @brief Create an immutable local snapshot for uploading a dirty file
     *
     * For dirty files already staged in the persistent pending store, this
     * renames the current pending inode into a snapshot path and then copies
     * the bytes back to the pending path. Existing read handles remain pinned
     * to the snapshot inode while future opens and writes see the fresh copy.
     *
     * @param fileId Google Drive file ID
     * @param expectedGeneration Dirty generation the caller intends to upload
     * @return Snapshot result describing whether a snapshot is ready, blocked,
     *         already uploaded, stale, or failed.
     */
    UploadSnapshotResult createUploadSnapshot(const QString& fileId, quint64 expectedGeneration);

    /**
     * @brief Remove a previously created upload snapshot path
     * @param snapshotPath Absolute path to the snapshot file
     */
    void cleanupUploadSnapshot(const QString& snapshotPath);

    /**
     * @brief Finalize a generation that was already uploaded
     *
     * Clears the dirty state once no writable handles remain. Read-only
     * handles are safe because they survive local renames.
     *
     * @param fileId Google Drive file ID
     * @return true if the dirty entry was finalized and cleared
     */
    bool finalizeUploadedGeneration(const QString& fileId);

    // ========================================================================
    // Cache Management
    // ========================================================================

    // ========================================================================
    // Self-Upload Tracking (prevents MetadataRefreshWorker from
    // invalidating files that we just uploaded ourselves)
    // ========================================================================

    /**
     * @brief Record that a file was just uploaded by DirtySyncWorker
     * @param fileId Google Drive file ID
     */
    void markRecentlyUploaded(const QString& fileId);

    /**
     * @brief Check and consume a recently-uploaded marker
     *
     * Returns true if the file was recently uploaded by us (and removes
     * the marker so it is consumed only once).
     *
     * @param fileId Google Drive file ID
     * @return true if the file was uploaded by us and should not be invalidated
     */
    bool consumeRecentlyUploaded(const QString& fileId);

    // ========================================================================
    // Open-Handle Tracking (prevents eviction / invalidation of files
    // that are currently opened by a FUSE client)
    // ========================================================================

    /**
     * @brief Increment the open-handle reference count for a file
     * @param fileId Google Drive file ID
     * @param writable Whether the handle can still mutate the local file
     */
    void addOpenHandle(const QString& fileId, bool writable = false);

    /**
     * @brief Decrement the open-handle reference count for a file
     * @param fileId Google Drive file ID
     * @param writable Whether the handle could mutate the local file
     */
    void removeOpenHandle(const QString& fileId, bool writable = false);

    /**
     * @brief Check whether a file has any open FUSE handles
     * @param fileId Google Drive file ID
     * @return true if at least one handle is open
     */
    bool hasOpenHandles(const QString& fileId) const;

    /**
     * @brief Check whether a file has any open writable FUSE handles
     * @param fileId Google Drive file ID
     * @return true if at least one writable handle is open
     */
    bool hasOpenWritableHandles(const QString& fileId) const;

    /**
     * @brief Record that a specific dirty generation was uploaded
     * @param fileId Google Drive file ID
     * @param generation Local dirty generation uploaded to Drive
     */
    void markUploadedGeneration(const QString& fileId, quint64 generation);

    /**
     * @brief Check whether a dirty generation was already uploaded
     * @param fileId Google Drive file ID
     * @param generation Local dirty generation
     * @return true if that generation was already uploaded to Drive
     */
    bool hasUploadedGeneration(const QString& fileId, quint64 generation) const;

    // ========================================================================
    // Cache Management
    // ========================================================================

    /**
     * @brief Evict files to free specified space
     *
     * Uses LRU eviction policy. Does not evict dirty files.
     * Updates fuse_cache_entries table.
     *
     * @param bytesNeeded Space needed in bytes
     * @return true if enough space was freed
     */
    bool evictToFreeSpace(qint64 bytesNeeded);

    /**
     * @brief Add a file to cache (after external download)
     *
     * Records a file that was downloaded externally (e.g., for new file creation).
     * Creates entry in fuse_cache_entries table.
     *
     * @param fileId Google Drive file ID
     * @param localPath Path to the file on disk
     * @param size File size in bytes
     * @return true if successfully added
     */
    bool recordCacheEntry(const QString& fileId, const QString& localPath, qint64 size);

   signals:
    /**
     * @brief Emitted when a file download starts
     * @param fileId File being downloaded
     */
    void downloadStarted(const QString& fileId);

    /**
     * @brief Emitted when a file download completes
     * @param fileId Downloaded file ID
     * @param cachePath Local cache path
     */
    void downloadCompleted(const QString& fileId, const QString& cachePath);

    /**
     * @brief Emitted when a file download fails
     * @param fileId File ID
     * @param error Error message
     */
    void downloadFailed(const QString& fileId, const QString& error);

    /**
     * @brief Emitted when a download/export fails with HTTP status detail
     * @param fileId File ID
     * @param error Error message
     * @param httpStatus HTTP status code when available, otherwise 0
     */
    void downloadFailedDetailed(const QString& fileId, const QString& error, int httpStatus);

    /**
     * @brief Emitted when a file is evicted from cache
     * @param fileId Evicted file ID
     */
    void fileEvicted(const QString& fileId);

    /**
     * @brief Emitted when cache size changes significantly
     * @param newSize New cache size in bytes
     */
    void cacheSizeChanged(qint64 newSize);

    /**
     * @brief Emitted when a file is marked dirty
     * @param fileId Dirty file ID
     * @param path File path
     */
    void fileDirty(const QString& fileId, const QString& path);

   private slots:
    /**
     * @brief Handle file download completion from GoogleDriveClient
     * @param fileId Downloaded file ID
     * @param localPath Local path where file was saved
     */
    void onFileDownloaded(const QString& fileId, const QString& localPath);

    /**
     * @brief Handle download error from GoogleDriveClient (via errorDetailed signal)
     * @param operation Operation that failed
     * @param errorMsg Error message
     * @param httpStatus HTTP status code (0 if unavailable)
     * @param fileId File ID associated with the request (if known)
     * @param localPath Local path associated with the request (if known)
     */
    void onDownloadError(const QString& operation, const QString& errorMsg, int httpStatus,
                         const QString& fileId, const QString& localPath);

   private:
    // Internal helpers
    QString generateCachePath(const QString& fileId) const;
    QString generateCachePath(const QString& fileId, const QString& exportMimeType) const;
    QString generateDirtyPath(const QString& fileId) const;
    QString generateUploadSnapshotPath(const QString& fileId, quint64 generation) const;
    QString getContentPathLocked(const QString& fileId,
                                 const QString& exportMimeType = QString()) const;
    QString getReadyExportPathLocked(const QString& fileId, const QString& cacheKey,
                                     const QString& cachePath);
    bool recordCacheEntryLocked(const QString& cacheKey, const QString& fileId,
                                const QString& cachePath, qint64 size,
                                const QDateTime& completedAt);
    QList<PendingExportRequest> collectQueuedExportsToStartLocked();
    void startExportRequests(const QList<PendingExportRequest>& requests);
    bool clearDirtyLocked(const QString& fileId, quint64 expectedGeneration);
    bool maybeFinalizeUploadedGenerationLocked(const QString& fileId);
    bool recycleAuthoritativeCopyToCacheLocked(const QString& fileId);
    bool resetSnapshotDirectoryLocked();
    void loadCacheFromDatabase();
    void evictLRU();
    void updateCacheSizeFromDisk();

    // Dependencies
    SyncDatabase* m_database;
    GoogleDriveClient* m_driveClient;

    // Cache state
    QString m_cacheDirectory;
    QString m_dirtyDirectory;
    qint64 m_maxCacheSize;
    qint64 m_currentSize;

    // In-memory cache index (mirrors fuse_cache_entries table)
    QMap<QString, CacheEntry> m_cacheEntries;

    // In-memory dirty files (mirrors fuse_dirty_files table)
    QMap<QString, DirtyFileEntry> m_dirtyFiles;

    // Self-upload tracking — fileIds uploaded by DirtySyncWorker that
    // MetadataRefreshWorker should not invalidate (Fix 1)
    QSet<QString> m_recentlyUploaded;

    // Open-handle reference counts — files with open FUSE handles
    // must not be evicted or invalidated (Fix 2)
    QMap<QString, int> m_openHandleCounts;

    // Writable-handle reference counts — files with open writable FUSE
    // handles must not be snapshotted for upload because the pending file
    // can still be mutated after one writer closes.
    QMap<QString, int> m_openWritableHandleCounts;

    // Thread safety
    mutable QMutex m_mutex;

    // Download synchronization
    QMap<QString, bool> m_pendingDownloads;         // cacheKey -> completed
    QMap<QString, QString> m_pendingDownloadPaths;  // localPath -> cacheKey
    QMap<QString, QString> m_downloadErrors;        // cacheKey -> error
    QWaitCondition m_downloadCondition;

    // Native-doc export scheduling
    QMap<QString, PendingExportRequest> m_pendingExportRequests;
    QSet<QString> m_activeExportKeys;
    QList<QString> m_queuedExportKeys;

    // Default 10GB cache size (as specified in procedure chart)
    static constexpr qint64 DEFAULT_MAX_CACHE_SIZE = 10LL * 1024 * 1024 * 1024;
    static constexpr int MAX_BACKGROUND_EXPORTS = 2;
};

#endif  // FILECACHE_H
