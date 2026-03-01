/**
 * @file MetadataCache.h
 * @brief In-memory metadata cache for FUSE filesystem operations
 *
 * Provides fast access to file/folder metadata for FUSE operations like
 * getattr and readdir. Caches Google Drive file metadata and provides
 * path-to-fileId mappings.
 */

#ifndef METADATACACHE_H
#define METADATACACHE_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QReadWriteLock>
#include <QString>

class SyncDatabase;
class GoogleDriveClient;

/**
 * @struct FuseFileMetadata
 * @brief Metadata for a file or folder in the FUSE filesystem
 *
 * This structure holds all metadata needed for FUSE operations
 * including stat attributes and Google Drive specific information.
 */
struct FuseFileMetadata {
    QString fileId;           ///< Google Drive file ID
    QString path;             ///< Full path relative to mount point
    QString name;             ///< File/folder name
    QString parentId;         ///< Parent folder's Google Drive file ID
    bool isFolder = false;    ///< Whether this is a folder
    qint64 size = 0;          ///< File size in bytes (0 for folders)
    QString mimeType;         ///< MIME type
    QDateTime createdTime;    ///< Creation timestamp
    QDateTime modifiedTime;   ///< Last modification timestamp
    QDateTime cachedAt;       ///< When this metadata was cached
    QDateTime lastAccessed;   ///< When this entry was last accessed

    /**
     * @brief Check if this metadata entry is valid
     * @return true if the entry has required fields (fileId and path)
     */
    bool isValid() const { return !fileId.isEmpty() && !path.isEmpty(); }

    /**
     * @brief Check if the cached metadata is stale
     * @param maxAgeSeconds Maximum age in seconds before considered stale
     * @return true if the metadata is older than maxAgeSeconds
     */
    bool isStale(int maxAgeSeconds) const {
        return cachedAt.secsTo(QDateTime::currentDateTime()) > maxAgeSeconds;
    }
};

/**
 * @class MetadataCache
 * @brief In-memory cache for FUSE file and folder metadata
 *
 * This class provides:
 * - Fast path-to-fileId lookups for FUSE operations
 * - Caching of file/folder attributes (size, times, permissions)
 * - Directory listing support for readdir operations
 * - Integration with SyncDatabase for persistence
 * - Background refresh of stale metadata via GoogleDriveClient
 *
 * Thread Safety:
 * - All public methods are thread-safe using read-write locks
 * - Write operations (add/update/remove) acquire exclusive locks
 * - Read operations can proceed concurrently
 */
class MetadataCache : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the metadata cache
     * @param database Pointer to the sync database for persistence
     * @param driveClient Pointer to the Google Drive client for API calls
     * @param parent Parent QObject
     */
    explicit MetadataCache(SyncDatabase* database, GoogleDriveClient* driveClient,
                           QObject* parent = nullptr);

    ~MetadataCache() override;

    /**
     * @brief Initialize the cache from database
     * @return true if initialization successful
     */
    bool initialize();

    // ========================================
    // Path-based lookups (primary interface for FUSE)
    // ========================================

    /**
     * @brief Get metadata by path
     * @param path File path relative to mount point (e.g., "/Documents/file.txt")
     * @return Metadata if found, or invalid FuseFileMetadata if not in cache
     *
     * This is the primary lookup method used by fuse_getattr.
     * Returns immediately from cache. If path not in cache, returns invalid metadata
     * and caller should use getOrFetchMetadata() for blocking fetch.
     */
    FuseFileMetadata getMetadataByPath(const QString& path) const;

    /**
     * @brief Get metadata by path, fetching from API if not cached
     * @param path File path relative to mount point
     * @param[out] fetched Set to true if metadata was fetched from API
     * @return Metadata (may be invalid if fetch failed)
     *
     * This method will:
     * 1. Check in-memory cache
     * 2. Check database cache
     * 3. Attempt API fetch if not found (BLOCKING call)
     *
     * Use for FUSE operations that can tolerate blocking on cache miss.
     */
    FuseFileMetadata getOrFetchMetadataByPath(const QString& path, bool* fetched = nullptr);

    /**
     * @brief Check if a path exists in the cache
     * @param path File path relative to mount point
     * @return true if the path is in the cache
     */
    bool hasPath(const QString& path) const;

    // ========================================
    // FileId-based lookups
    // ========================================

    /**
     * @brief Get metadata by Google Drive file ID
     * @param fileId Google Drive file ID
     * @return Metadata if found, or invalid FuseFileMetadata if not in cache
     */
    FuseFileMetadata getMetadataByFileId(const QString& fileId) const;

    /**
     * @brief Get file ID for a given path
     * @param path File path relative to mount point
     * @return Google Drive file ID or empty string if not found
     */
    QString getFileIdByPath(const QString& path) const;

    /**
     * @brief Get path for a given file ID
     * @param fileId Google Drive file ID
     * @return File path or empty string if not found
     */
    QString getPathByFileId(const QString& fileId) const;

    // ========================================
    // Directory operations (for readdir)
    // ========================================

    /**
     * @brief Get children of a directory
     * @param parentPath Path of the parent directory
     * @return List of child metadata entries (may be empty)
     *
     * Used by fuse_readdir to list directory contents.
     * Returns only immediate children, not recursive.
     */
    QList<FuseFileMetadata> getChildren(const QString& parentPath) const;

    /**
     * @brief Get children of a directory, fetching from API if needed
     * @param parentPath Path of the parent directory
     * @param[out] fetched Set to true if children were fetched from API
     * @return List of child metadata entries
     *
     * This method will attempt to fetch children from the API if not cached.
     * BLOCKING call - use for FUSE readdir when cache is stale.
     */
    QList<FuseFileMetadata> getOrFetchChildren(const QString& parentPath, bool* fetched = nullptr);

    /**
     * @brief Check if a directory's children are cached
     * @param parentPath Path of the parent directory
     * @return true if children are cached and not stale
     */
    bool hasChildrenCached(const QString& parentPath) const;

    // ========================================
    // Cache modification
    // ========================================

    /**
     * @brief Add or update metadata in cache
     * @param metadata The metadata to store
     *
     * Updates both in-memory cache and database.
     */
    void setMetadata(const FuseFileMetadata& metadata);

    /**
     * @brief Add or update multiple metadata entries
     * @param metadataList List of metadata entries to store
     *
     * More efficient than calling setMetadata in a loop.
     */
    void setMetadataBatch(const QList<FuseFileMetadata>& metadataList);

    /**
     * @brief Remove metadata from cache
     * @param path File path to remove
     *
     * Removes from both in-memory cache and database.
     */
    void removeByPath(const QString& path);

    /**
     * @brief Remove metadata from cache by file ID
     * @param fileId Google Drive file ID to remove
     */
    void removeByFileId(const QString& fileId);

    /**
     * @brief Update the path of an existing entry (for renames/moves)
     * @param oldPath Previous path
     * @param newPath New path
     * @return true if update successful
     */
    bool updatePath(const QString& oldPath, const QString& newPath);

    /**
     * @brief Update the parent ID of an existing entry (for moves)
     * @param fileId File ID to update
     * @param newParentId New parent folder ID
     * @return true if update successful
     */
    bool updateParentId(const QString& fileId, const QString& newParentId);

    /**
     * @brief Mark metadata as accessed (update lastAccessed time)
     * @param path File path
     */
    void markAccessed(const QString& path);

    // ========================================
    // Cache invalidation
    // ========================================

    /**
     * @brief Invalidate a cache entry (mark as stale)
     * @param path File path to invalidate
     *
     * The entry remains in cache but will be refreshed on next access.
     */
    void invalidate(const QString& path);

    /**
     * @brief Invalidate a cache entry by file ID
     * @param fileId Google Drive file ID to invalidate
     */
    void invalidateByFileId(const QString& fileId);

    /**
     * @brief Invalidate all children of a directory
     * @param parentPath Directory path
     */
    void invalidateChildren(const QString& parentPath);

    /**
     * @brief Clear all cached metadata
     *
     * Clears in-memory cache only. Database entries are preserved.
     */
    void clearCache();

    /**
     * @brief Clear all metadata including database entries
     */
    void clearAll();

    // ========================================
    // Configuration
    // ========================================

    /**
     * @brief Set the maximum age for cached metadata before refresh
     * @param seconds Maximum age in seconds (default: 300 = 5 minutes)
     */
    void setMaxCacheAge(int seconds);

    /**
     * @brief Get the current max cache age setting
     * @return Maximum cache age in seconds
     */
    int maxCacheAge() const;

    /**
     * @brief Get the root folder ID (My Drive root)
     * @return Root folder ID or empty string if not set
     */
    QString rootFolderId() const;

    /**
     * @brief Set the root folder ID
     * @param fileId Root folder ID from Google Drive
     */
    void setRootFolderId(const QString& fileId);

    // ========================================
    // Statistics
    // ========================================

    /**
     * @brief Get the number of entries in the in-memory cache
     * @return Number of cached entries
     */
    int cacheSize() const;

    /**
     * @brief Get cache hit/miss statistics
     * @param[out] hits Number of cache hits
     * @param[out] misses Number of cache misses
     */
    void getStatistics(qint64* hits, qint64* misses) const;

    /**
     * @brief Reset cache statistics
     */
    void resetStatistics();

   signals:
    /**
     * @brief Emitted when metadata is updated
     * @param path Path of the updated entry
     */
    void metadataUpdated(const QString& path);

    /**
     * @brief Emitted when metadata is removed
     * @param path Path of the removed entry
     */
    void metadataRemoved(const QString& path);

    /**
     * @brief Emitted when metadata fetch from API completes
     * @param path Path that was fetched
     * @param success true if fetch was successful
     */
    void metadataFetched(const QString& path, bool success);

    /**
     * @brief Emitted when cache is cleared
     */
    void cacheCleared();

    /**
     * @brief Emitted on cache error
     * @param error Error message
     */
    void cacheError(const QString& error);

   private slots:
    /**
     * @brief Handle API response for file metadata
     * @param fileId File ID that was queried
     * @param metadata Retrieved metadata (or empty on error)
     */
    void onApiMetadataReceived(const QString& fileId, const FuseFileMetadata& metadata);

    /**
     * @brief Handle API response for directory listing
     * @param parentId Parent folder ID
     * @param children List of child metadata entries
     */
    void onApiChildrenReceived(const QString& parentId, const QList<FuseFileMetadata>& children);

   private:
    /**
     * @brief Load metadata from database into in-memory cache
     */
    void loadFromDatabase();

    /**
     * @brief Save metadata to database
     * @param metadata Metadata to persist
     */
    void saveToDatabase(const FuseFileMetadata& metadata);

    /**
     * @brief Remove metadata from database
     * @param fileId File ID to remove
     */
    void removeFromDatabase(const QString& fileId);

    /**
     * @brief Build path from file ID by traversing parent chain
     * @param fileId File ID to resolve
     * @return Full path or empty string if cannot resolve
     */
    QString buildPathFromParents(const QString& fileId) const;

    /**
     * @brief Request metadata from API (asynchronous)
     * @param fileId File ID to fetch
     */
    void requestMetadataFromApi(const QString& fileId);

    /**
     * @brief Request children from API (asynchronous)
     * @param parentId Parent folder ID
     */
    void requestChildrenFromApi(const QString& parentId);

    /**
     * @brief Get the parent path from a full path
     * @param path Full file path
     * @return Parent directory path (always "/" for root-level files)
     */
    static QString getParentPath(const QString& path);

    SyncDatabase* m_database;
    GoogleDriveClient* m_driveClient;

    // In-memory caches with thread-safe access
    QHash<QString, FuseFileMetadata> m_pathToMetadata;   ///< Path -> Metadata
    QHash<QString, QString> m_fileIdToPath;              ///< FileId -> Path
    QHash<QString, QList<QString>> m_parentToChildren;   ///< ParentPath -> Child paths
    QHash<QString, QDateTime> m_childrenCacheTime;       ///< When children were cached

    // Root folder ID for path resolution
    QString m_rootFolderId;

    // Configuration
    int m_maxCacheAgeSeconds;  ///< Default: 300 (5 minutes)

    // Statistics
    mutable qint64 m_cacheHits;
    mutable qint64 m_cacheMisses;

    // Thread safety
    mutable QReadWriteLock m_lock;

    // Constants
    static const int DEFAULT_MAX_CACHE_AGE_SECONDS = 300;  // 5 minutes
    static const char* DB_CONNECTION_NAME;  // Database connection name
};

#endif  // METADATACACHE_H
