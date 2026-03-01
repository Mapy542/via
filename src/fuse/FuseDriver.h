/**
 * @file FuseDriver.h
 * @brief FUSE virtual filesystem driver for Google Drive
 *
 * Implements a FUSE filesystem that provides on-demand access to Google Drive
 * files through a virtual filesystem mount point. Files are downloaded when
 * accessed and cached locally for performance.
 *
 * Architecture (from FUSE Procedure Flow Chart):
 * - FuseDriver: Main driver, handles FUSE callbacks and coordinates components
 * - MetadataCache: File/folder hierarchy and path-to-fileId mapping (separate class)
 * - FileCache: LRU cache manager for file contents
 * - DirtySyncWorker: Background thread for uploading modified files
 * - MetadataRefreshWorker: Background thread for polling remote changes
 *
 * Threading Model:
 * - FUSE Thread: Handles FUSE kernel callbacks (runs in FUSE context)
 * - Main Thread: Qt event loop processes API requests
 * - Dirty Sync Thread: Periodic upload of modified files (default: 5s interval)
 * - Metadata Refresh Thread: Polls for remote changes (default: 30s interval)
 *
 * Database Integration:
 * Uses SyncDatabase with FUSE-specific tables (isolated from Mirror Sync):
 * - fuse_metadata: File/folder metadata cache
 * - fuse_cache_entries: Cached file tracking for LRU eviction
 * - fuse_dirty_files: Modified files pending upload
 * - fuse_sync_state: Change tokens and other state
 */

#ifndef FUSEDRIVER_H
#define FUSEDRIVER_H

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>

// Forward declarations
class GoogleDriveClient;
class SyncDatabase;
class FileCache;
class MetadataCache;

// Forward declaration for internal worker classes
class DirtySyncWorker;
class MetadataRefreshWorker;

/**
 * @brief Interface for metadata cache operations
 *
 * Defines the interface that FuseDriver uses to interact with metadata.
 * This allows for dependency injection and testing.
 * Implemented by the internal MetadataCache component.
 */
class IMetadataProvider {
   public:
    virtual ~IMetadataProvider() = default;

    /**
     * @brief Get metadata by file path
     * @param path Path in FUSE filesystem (e.g., "/folder/file.txt")
     * @param stbuf Output stat structure to fill
     * @return true if found and stbuf filled, false otherwise
     */
    virtual bool getStatByPath(const QString& path, struct stat* stbuf) const = 0;

    /**
     * @brief Get file ID by path
     * @param path Path in FUSE filesystem
     * @return Google Drive file ID, or empty string if not found
     */
    virtual QString getFileIdByPath(const QString& path) const = 0;

    /**
     * @brief Get children of a directory
     * @param path Directory path
     * @return List of child names (not full paths)
     */
    virtual QStringList getChildNames(const QString& path) const = 0;

    /**
     * @brief Check if path exists and is a directory
     * @param path Path to check
     * @return true if exists and is directory
     */
    virtual bool isDirectory(const QString& path) const = 0;

    /**
     * @brief Check if directory is empty
     * @param path Directory path
     * @return true if directory has no children
     */
    virtual bool isDirectoryEmpty(const QString& path) const = 0;

    /**
     * @brief Refresh metadata for a path from remote
     * @param path Path to refresh
     * @return true if refresh successful
     */
    virtual bool refreshPath(const QString& path) = 0;

    /**
     * @brief Refresh root directory listing from remote
     * @return true if refresh successful
     */
    virtual bool refreshRoot() = 0;

    /**
     * @brief Create new file metadata after remote creation
     * @param path Path of new file
     * @param fileId Google Drive file ID
     * @param isFolder Whether this is a folder
     * @return true if successful
     */
    virtual bool createEntry(const QString& path, const QString& fileId, bool isFolder) = 0;

    /**
     * @brief Remove metadata entry
     * @param path Path to remove
     * @return true if successful
     */
    virtual bool removeEntry(const QString& path) = 0;

    /**
     * @brief Update path after rename/move
     * @param oldPath Original path
     * @param newPath New path
     * @return true if successful
     */
    virtual bool movePath(const QString& oldPath, const QString& newPath) = 0;
};

/**
 * @brief Interface for file content cache operations
 *
 * Defines the interface that FuseDriver uses to interact with file cache.
 * Implemented by FileCache class.
 */
class IFileCacheProvider {
   public:
    virtual ~IFileCacheProvider() = default;

    /**
     * @brief Check if file is cached
     * @param fileId Google Drive file ID
     * @return true if file is in cache
     */
    virtual bool isCached(const QString& fileId) const = 0;

    /**
     * @brief Get cached file path (downloads if not cached)
     *
     * This may BLOCK waiting for download to complete.
     *
     * @param fileId Google Drive file ID
     * @param expectedSize Expected file size (for cache eviction planning)
     * @return Local cache path, or empty string on failure
     */
    virtual QString getCachedPath(const QString& fileId, qint64 expectedSize = 0) = 0;

    /**
     * @brief Get cache path without downloading
     * @param fileId Google Drive file ID
     * @return Path where file would be cached
     */
    virtual QString getCachePathForFile(const QString& fileId) const = 0;

    /**
     * @brief Update last accessed time (for LRU tracking)
     * @param fileId Google Drive file ID
     */
    virtual void updateAccessTime(const QString& fileId) = 0;

    /**
     * @brief Mark file as dirty (needs upload)
     * @param fileId Google Drive file ID
     * @param path Logical path in FUSE filesystem
     */
    virtual void markDirty(const QString& fileId, const QString& path) = 0;

    /**
     * @brief Remove file from cache
     * @param fileId Google Drive file ID
     */
    virtual void removeFromCache(const QString& fileId) = 0;

    /**
     * @brief Record a new cache entry (for externally created files)
     * @param fileId Google Drive file ID
     * @param localPath Local path on disk
     * @param size File size
     * @return true if successful
     */
    virtual bool recordCacheEntry(const QString& fileId, const QString& localPath, qint64 size) = 0;
};

/**
 * @struct FuseOpenFile
 * @brief Represents an open file handle in FUSE
 *
 * Stores information about currently open files for read/write operations.
 * The file handle (fi->fh) in FUSE callbacks maps to an entry in m_openFiles.
 */
struct FuseOpenFile {
    QString fileId;     ///< Google Drive file ID
    QString path;       ///< Path in FUSE filesystem
    QString cachePath;  ///< Local cache path
    qint64 size;        ///< File size
    bool writable;      ///< Whether opened for writing
    bool dirty;         ///< Whether file has been modified
};

/**
 * @class FuseDriver
 * @brief Main FUSE filesystem driver
 *
 * Implements the FUSE callbacks to provide a virtual filesystem that presents
 * Google Drive files as local files. Coordinates between metadata cache,
 * file cache, and Google Drive API client.
 *
 * Usage:
 * @code
 * FuseDriver driver(driveClient, database);
 * driver.setMountPoint("/home/user/GoogleDriveFuse");
 * if (driver.mount()) {
 *     // Filesystem is now mounted and accessible
 * }
 * // Later...
 * driver.unmount();
 * @endcode
 *
 * Mount Process (from procedure chart):
 * 1. Create mount point directory if needed
 * 2. Initialize MetadataCache from database/API
 * 3. Initialize FileCache manager
 * 4. Start FUSE thread and register operation handlers
 * 5. Start background worker threads (DirtySyncWorker, MetadataRefreshWorker)
 * 6. Emit mounted() signal
 *
 * Unmount Process (from procedure chart):
 * 1. Flush all dirty files (upload pending changes)
 * 2. Stop background threads
 * 3. Clear in-memory caches
 * 4. Unmount FUSE filesystem
 * 5. Emit unmounted() signal
 */
class FuseDriver : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the FUSE driver
     * @param driveClient Pointer to Google Drive API client (shared with other components)
     * @param database Pointer to sync database (for FUSE-specific tables)
     * @param parent Parent QObject
     */
    explicit FuseDriver(GoogleDriveClient* driveClient, SyncDatabase* database,
                        QObject* parent = nullptr);

    ~FuseDriver() override;

    // ========================================================================
    // Static Utility Methods
    // ========================================================================

    /**
     * @brief Check if FUSE is available on this system
     * @return true if /dev/fuse exists and is accessible
     */
    static bool isFuseAvailable();

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Get the mount point path
     * @return Current mount point path
     */
    QString mountPoint() const;

    /**
     * @brief Set the mount point path
     *
     * Must be called before mount(). Cannot be changed while mounted.
     *
     * @param path New mount point path (must be absolute)
     */
    void setMountPoint(const QString& path);

    /**
     * @brief Set file cache directory path
     *
     * Must be called before mount().
     *
     * @param path Cache directory path
     */
    void setCacheDirectory(const QString& path);

    /**
     * @brief Set file cache maximum size in bytes
     *
     * Must be called before mount().
     *
     * @param bytes Max cache size in bytes
     */
    void setMaxCacheSizeBytes(qint64 bytes);

    /**
     * @brief Check if filesystem is currently mounted
     * @return true if mounted
     */
    bool isMounted() const;

    /**
     * @brief Get the file cache instance
     *
     * Used by background workers to access cache state.
     *
     * @return Pointer to FileCache, or nullptr if not initialized
     */
    FileCache* fileCache() const;

    /**
     * @brief Get the database instance
     * @return Pointer to SyncDatabase
     */
    SyncDatabase* database() const;

    /**
     * @brief Get the drive client instance
     * @return Pointer to GoogleDriveClient
     */
    GoogleDriveClient* driveClient() const;

   public slots:
    /**
     * @brief Mount the virtual filesystem
     *
     * Performs the mount process as described in class documentation.
     * This call returns immediately; actual mounting happens in FUSE thread.
     *
     * @return true if mount was initiated successfully
     */
    bool mount();

    /**
     * @brief Unmount the virtual filesystem
     *
     * Performs the unmount process as described in class documentation.
     * Blocks until unmount is complete.
     */
    void unmount();

    /**
     * @brief Refresh all metadata from remote
     *
     * Forces a refresh of the metadata cache. Called manually or by
     * MetadataRefreshWorker when changes are detected.
     */
    void refreshMetadata();

    /**
     * @brief Flush all dirty files
     *
     * Uploads all files marked as dirty. Called during unmount or
     * can be called manually.
     */
    void flushDirtyFiles();

   signals:
    /**
     * @brief Emitted when filesystem is mounted successfully
     */
    void mounted();

    /**
     * @brief Emitted when filesystem is unmounted
     */
    void unmounted();

    /**
     * @brief Emitted on mount error
     * @param error Error message
     */
    void mountError(const QString& error);

    /**
     * @brief Emitted when a file is accessed
     * @param path File path
     */
    void fileAccessed(const QString& path);

    /**
     * @brief Emitted when a file is modified
     * @param path File path
     */
    void fileModified(const QString& path);

    /**
     * @brief Emitted when metadata refresh completes
     */
    void metadataRefreshed();

    /**
     * @brief Emitted when dirty files are flushed
     * @param count Number of files uploaded
     */
    void dirtyFilesFlushed(int count);

   private:
    // ========================================================================
    // FUSE Operation Callbacks (static, required by FUSE API)
    // These are registered with the FUSE library and called from the FUSE
    // thread context. They use s_instance to access the FuseDriver object.
    // ========================================================================

    /**
     * @brief Get file/folder attributes (stat)
     *
     * Flow (from procedure chart):
     * 1. Check in-memory MetadataCache
     * 2. If not found, query FuseDatabase.fuse_metadata
     * 3. If not found, queue API call to get metadata
     * 4. Return stat structure
     */
    static int fuseGetattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi);

    /**
     * @brief List directory contents
     *
     * Flow (from procedure chart):
     * 1. Check MetadataCache for children
     * 2. If not cached, query API for children
     * 3. Fill buffer with child names
     */
    static int fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset,
                           struct fuse_file_info* fi, enum fuse_readdir_flags flags);

    /**
     * @brief Open a file
     *
     * Flow (from procedure chart):
     * 1. Check FileCache for file
     * 2. If not cached, download via GoogleDriveClient
     * 3. Record in fuse_cache_entries
     * 4. Check cache size, evict LRU if needed
     * 5. Create file handle
     */
    static int fuseOpen(const char* path, struct fuse_file_info* fi);

    /**
     * @brief Read file data
     *
     * Flow (from procedure chart):
     * 1. Read from cached file
     * 2. Update fuse_cache_entries.last_accessed
     */
    static int fuseRead(const char* path, char* buf, size_t size, off_t offset,
                        struct fuse_file_info* fi);

    /**
     * @brief Write file data
     *
     * Flow (from procedure chart):
     * 1. Write to cached file
     * 2. Mark file as dirty
     * 3. Record in fuse_dirty_files
     * 4. Upload is deferred to DirtySyncWorker
     */
    static int fuseWrite(const char* path, const char* buf, size_t size, off_t offset,
                         struct fuse_file_info* fi);

    /**
     * @brief Release (close) a file
     *
     * Cleanup file handle. Dirty files are uploaded by DirtySyncWorker.
     */
    static int fuseRelease(const char* path, struct fuse_file_info* fi);

    /**
     * @brief Create new directory
     *
     * Flow (from procedure chart):
     * 1. Queue API call to create folder
     * 2. Store folder ID in MetadataCache
     * 3. Save to fuse_metadata
     */
    static int fuseMkdir(const char* path, mode_t mode);

    /**
     * @brief Remove directory
     *
     * Flow (from procedure chart):
     * 1. Check if directory is empty
     * 2. Queue API call to delete folder
     * 3. Remove from fuse_metadata
     */
    static int fuseRmdir(const char* path);

    /**
     * @brief Delete file
     *
     * Flow (from procedure chart):
     * 1. Remove from cache if present
     * 2. Delete from fuse_cache_entries
     * 3. Queue API call to delete file
     * 4. Remove from fuse_metadata
     */
    static int fuseUnlink(const char* path);

    /**
     * @brief Rename/move file
     *
     * Flow (from procedure chart):
     * 1. Determine if rename, move, or both
     * 2. Queue appropriate API call
     * 3. Update fuse_metadata
     */
    static int fuseRename(const char* from, const char* to, unsigned int flags);

    /**
     * @brief Truncate file
     *
     * Flow (from procedure chart):
     * 1. Truncate cached file
     * 2. Mark as dirty for upload
     */
    static int fuseTruncate(const char* path, off_t size, struct fuse_file_info* fi);

    /**
     * @brief Create new file
     *
     * Flow (from procedure chart):
     * 1. Create empty file in cache
     * 2. Queue API call to create remote file
     * 3. Store file ID in MetadataCache
     * 4. Save to fuse_metadata
     */
    static int fuseCreate(const char* path, mode_t mode, struct fuse_file_info* fi);

    /**
     * @brief FUSE init callback
     */
    static void* fuseInit(struct fuse_conn_info* conn, struct fuse_config* cfg);

    /**
     * @brief FUSE destroy callback
     */
    static void fuseDestroy(void* private_data);

    /**
     * @brief Report filesystem statistics (M5 stub)
     */
    static int fuseStatfs(const char* path, struct statvfs* stbuf);

    /**
     * @brief Change file permissions (M5 no-op stub)
     */
    static int fuseChmod(const char* path, mode_t mode, struct fuse_file_info* fi);

    /**
     * @brief Change file ownership (M5 no-op stub)
     */
    static int fuseChown(const char* path, uid_t uid, gid_t gid, struct fuse_file_info* fi);

    /**
     * @brief Set file timestamps (M5 no-op stub)
     */
    static int fuseUtimens(const char* path, const struct timespec tv[2],
                           struct fuse_file_info* fi);

    // ========================================================================
    // Internal Helper Methods
    // ========================================================================

    /**
     * @brief Initialize the metadata cache
     * @return true if successful
     */
    bool initializeMetadataCache();

    /**
     * @brief Initialize the file cache
     * @return true if successful
     */
    bool initializeFileCache();

    /**
     * @brief Start background worker threads
     */
    void startBackgroundWorkers();

    /**
     * @brief Stop background worker threads
     */
    void stopBackgroundWorkers();

    /**
     * @brief Get parent directory path
     * @param path Full path
     * @return Parent path
     */
    static QString getParentPath(const QString& path);

    /**
     * @brief Get filename from path
     * @param path Full path
     * @return Filename component
     */
    static QString getFileName(const QString& path);

    /**
     * @brief Convert FUSE path to internal format
     * @param fusePath Path from FUSE (starts with /)
     * @return Internal path format
     */
    static QString normalizePath(const char* fusePath);

    /**
     * @brief Register a new open file handle
     * @param openFile Open file info
     * @return File handle ID
     */
    uint64_t registerOpenFile(const FuseOpenFile& openFile);

    /**
     * @brief Get open file by handle
     * @param fh File handle from FUSE
     * @return Pointer to FuseOpenFile or nullptr if not found
     */
    FuseOpenFile* getOpenFile(uint64_t fh);

    /**
     * @brief Unregister an open file handle
     * @param fh File handle to unregister
     */
    void unregisterOpenFile(uint64_t fh);

    // ========================================================================
    // Member Variables
    // ========================================================================

    // External dependencies (not owned)
    GoogleDriveClient* m_driveClient;  ///< Google Drive API client
    SyncDatabase* m_database;          ///< Sync database

    // Owned components
    MetadataCache* m_metadataCache;  ///< Metadata cache
    FileCache* m_fileCache;          ///< File content cache

    // Configuration
    QString m_mountPoint;        ///< Mount point path
    QString m_cacheDirectory;    ///< Cache directory path override
    qint64 m_maxCacheSizeBytes;  ///< Max cache size override

    // State
    bool m_mounted;  ///< Whether filesystem is mounted

    // FUSE thread and handles
    QThread* m_fuseThread;           ///< FUSE event loop thread
    struct fuse* m_fuse;             ///< FUSE handle
    struct fuse_session* m_session;  ///< FUSE session

    // Background workers
    QThread* m_dirtySyncThread;                      ///< Dirty sync worker thread
    QThread* m_metadataRefreshThread;                ///< Metadata refresh worker thread
    DirtySyncWorker* m_dirtySyncWorker;              ///< Dirty file upload worker
    MetadataRefreshWorker* m_metadataRefreshWorker;  ///< Metadata refresh worker

    // Open file handles
    QMap<uint64_t, FuseOpenFile> m_openFiles;  ///< Map of open file handles
    uint64_t m_nextFileHandle;                 ///< Next file handle to assign
    mutable QMutex m_openFilesMutex;           ///< Mutex for m_openFiles access

    // Static instance for FUSE callbacks
    static FuseDriver* s_instance;  ///< Singleton instance for callbacks
};

#endif  // FUSEDRIVER_H
