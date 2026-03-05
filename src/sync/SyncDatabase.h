/**
 * @file SyncDatabase.h
 * @brief SQLite database for tracking file sync state
 */

#ifndef SYNCDATABASE_H
#define SYNCDATABASE_H

#include <QAtomicInteger>
#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

/**
 * @struct FileSyncState
 * @brief Represents the sync state of a file
 */
struct FileSyncState {
    QString localPath;             ///< Local file path (relative)
    QString fileId;                ///< Google Drive file ID (canonical UID)
    QDateTime modifiedTimeAtSync;  ///< Remote modified time at last sync
    bool isFolder = false;         ///< Whether this is a folder
    QString remoteMd5AtSync;       ///< Remote md5Checksum baseline at last sync
    QString localHashAtSync;       ///< Local content hash baseline at last sync
};

/**
 * @struct ConflictVersion
 * @brief Represents a version captured for a conflict record
 */
struct ConflictVersion {
    int id = -1;
    QDateTime localModifiedTime;
    QDateTime remoteModifiedTime;
    QDateTime dbSyncTime;
    QDateTime detectedAt;
};

/**
 * @struct ConflictRecord
 * @brief Represents a persisted conflict record
 */
struct ConflictRecord {
    int id = -1;
    QString localPath;
    QString fileId;
    QString conflictPath;
    QDateTime detectedAt;
    bool resolved = false;
    QList<ConflictVersion> versions;
};

// ============================================================================
// FUSE-specific structures (from FUSE Procedure Flow Chart)
// These map to the fuse_* tables in the database
// ============================================================================

/**
 * @struct FuseMetadata
 * @brief File/folder metadata for FUSE operations
 *
 * Maps to fuse_metadata table schema
 */
struct FuseMetadata {
    QString fileId;          ///< Google Drive file ID (primary key)
    QString path;            ///< Logical path in FUSE filesystem
    QString name;            ///< File/folder name
    QString parentId;        ///< Parent folder ID
    bool isFolder;           ///< Whether this is a folder
    qint64 size;             ///< File size in bytes
    QString mimeType;        ///< MIME type
    QDateTime createdTime;   ///< Creation timestamp
    QDateTime modifiedTime;  ///< Last modification timestamp
    QDateTime cachedAt;      ///< When metadata was cached
    QDateTime lastAccessed;  ///< Last access time for LRU
};

/**
 * @struct FuseCacheEntry
 * @brief Cached file entry for FUSE file cache
 *
 * Maps to fuse_cache_entries table schema
 */
struct FuseCacheEntry {
    QString fileId;               ///< Google Drive file ID (primary key)
    QString cachePath;            ///< Local cache path
    qint64 size;                  ///< File size in bytes
    QDateTime lastAccessed;       ///< Last access time (for LRU)
    QDateTime downloadCompleted;  ///< When download finished
};

/**
 * @struct FuseDirtyFile
 * @brief Dirty file entry for FUSE dirty file tracking
 *
 * Maps to fuse_dirty_files table schema
 */
struct FuseDirtyFile {
    QString fileId;               ///< Google Drive file ID (primary key)
    QString path;                 ///< Logical path in FUSE filesystem
    QDateTime markedDirtyAt;      ///< When file was marked dirty
    QDateTime lastUploadAttempt;  ///< Last upload attempt time
    bool uploadFailed;            ///< Whether last upload failed
};

/**
 * @class SyncDatabase
 * @brief SQLite database for file sync tracking
 *
 * Tracks:
 * - File mappings (local path <-> Drive ID)
 * - File metadata (checksums, sizes, timestamps)
 * - Sync history
 */
class SyncDatabase : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the sync database
     * @param parent Parent object
     */
    explicit SyncDatabase(QObject* parent = nullptr);

    ~SyncDatabase() override;

    /**
     * @brief Initialize the database
     * @return true if initialization successful
     */
    bool initialize();

    /**
     * @brief Close the database connection
     */
    void close();

    /**
     * @brief Check if database is open
     * @return true if database is open
     */
    bool isOpen() const;

    /**
     * @brief Get the peak concurrent access count (thread-safety telemetry)
     * @return Peak number of threads that attempted concurrent database access
     */
    int peakConcurrentAccess() const { return m_concurrentAccessCount.loadRelaxed(); }

    // File operations

    /**
     * @brief Add or update a file record (fileId is required)
     * @param state File sync state
     */
    void saveFileState(const FileSyncState& state);

    /**
     * @brief Get file sync state
     * @param localPath Local file path (relative)
     * @return File sync state or empty if not found
     */
    FileSyncState getFileState(const QString& localPath) const;

    /**
     * @brief Get file sync state by Drive file ID
     * @param fileId Google Drive file ID
     * @return File sync state or empty if not found
     */
    FileSyncState getFileStateById(const QString& fileId) const;

    /**
     * @brief Get file ID from local path
     * @param localPath Local file path (relative)
     * @return Google Drive file ID or empty string
     */
    QString getFileId(const QString& localPath) const;

    /**
     * @brief Get local path from file ID
     * @param fileId Google Drive file ID
     * @return Local file path (relative) or empty string
     */
    QString getLocalPath(const QString& fileId) const;

    /**
     * @brief Set file ID for local path
     * @param localPath Local file path (relative)
     * @param fileId Google Drive file ID
     */
    void setFileId(const QString& localPath, const QString& fileId);

    void setLocalPath(const QString& fileId, const QString& localPath);

    /**
     * @brief Get modified time at last sync for a file
     * @param localPath Local file path (relative)
     * @return Modified time at last sync or invalid datetime
     */
    QDateTime getModifiedTimeAtSync(const QString& localPath) const;

    /**
     * @brief Set modified time at last sync for a file
     * @param localPath Local file path (relative)
     * @param time Modified time at last sync
     */
    void setModifiedTimeAtSync(const QString& localPath, const QDateTime& time);

    /**
     * @brief Get remote md5Checksum recorded at last sync
     * @param localPath Local file path (relative)
     * @return Remote MD5 hash at sync or empty string
     */
    QString getRemoteMd5AtSync(const QString& localPath) const;

    /**
     * @brief Set remote md5Checksum recorded at last sync
     * @param localPath Local file path (relative)
     * @param remoteMd5 Remote MD5 hash
     */
    void setRemoteMd5AtSync(const QString& localPath, const QString& remoteMd5);

    /**
     * @brief Get local content hash recorded at last sync
     * @param localPath Local file path (relative)
     * @return Local hash at sync or empty string
     */
    QString getLocalHashAtSync(const QString& localPath) const;

    /**
     * @brief Set local content hash recorded at last sync
     * @param localPath Local file path (relative)
     * @param localHash Local content hash
     */
    void setLocalHashAtSync(const QString& localPath, const QString& localHash);

    /**
     * @brief Set both remote and local content hashes recorded at last sync
     * @param localPath Local file path (relative)
     * @param remoteMd5 Remote MD5 hash
     * @param localHash Local content hash
     */
    void setContentHashesAtSync(const QString& localPath, const QString& remoteMd5, const QString& localHash);

    /**
     * @brief Get all synced files
     * @return List of all file sync states
     */
    QList<FileSyncState> getAllFiles() const;

    /**
     * @brief Get all file states under a path prefix (descendants only)
     * @param pathPrefix Base path to match (relative)
     * @return List of file sync states for descendants
     */
    QList<FileSyncState> getFileStatesByPrefix(const QString& pathPrefix) const;

    // Change token

    /**
     * @brief Get the stored change page token
     * @return Change page token or empty string
     */
    // TODO: Figure out why no key in settings table!! (Saved in setting?? change to db for final)
    QString getChangeToken() const;

    /**
     * @brief Set the change page token
     * @param token New change page token
     */
    void setChangeToken(const QString& token);

    // Statistics

    /**
     * @brief Get total number of synced files
     * @return Number of files
     */
    int fileCount() const;

    // Delete tracking

    /**
     * @brief Mark a file as deleted locally (prevents re-download from remote)
     * @param localPath Local file path (relative)
     * @param fileId Google Drive file ID
     */
    void markFileDeleted(const QString& localPath, const QString& fileId);

    /**
     * @brief Check if a file was deleted locally
     * @param localPath Local file path (relative)
     * @return true if file was deleted locally
     */
    bool wasFileDeleted(const QString& localPath) const;

    /**
     * @brief Clear the deleted status for a file (e.g., if user re-creates it)
     * @param localPath Local file path (relative)
     */
    void clearDeletedFile(const QString& localPath);

    /**
     * @brief Purge deleted file records older than the specified number of days
     * @param maxAgeDays Maximum age in days (default 31)
     * @return Number of records purged
     */
    int purgeOldDeletedRecords(int maxAgeDays = 31);

    // Conflict persistence

    /**
     * @brief Create or reuse an unresolved conflict record
     * @param localPath Local file path (relative)
     * @param fileId Google Drive file ID
     * @param conflictPath Optional conflict copy path
     * @return Conflict record ID or -1 on failure
     */
    int upsertConflictRecord(const QString& localPath, const QString& fileId, const QString& conflictPath = QString());

    /**
     * @brief Append a version entry to a conflict record
     * @param conflictId Conflict record ID
     * @param version Version data
     */
    void addConflictVersion(int conflictId, const ConflictVersion& version);

    /**
     * @brief Check if a conflict exists and is unresolved for a path
     * @param localPath Local file path (relative)
     * @return true if an unresolved conflict exists
     */
    bool hasUnresolvedConflict(const QString& localPath) const;

    /**
     * @brief Get all unresolved conflict records with versions
     * @return List of unresolved conflict records
     */
    QList<ConflictRecord> getUnresolvedConflicts();

    /**
     * @brief Mark conflicts as resolved by local path
     * @param localPath Local file path (relative)
     */
    void markConflictResolved(const QString& localPath);

    /**
     * @brief Mark a conflict as resolved by record ID
     * @param conflictId Conflict record ID
     */
    void markConflictResolved(int conflictId);

    // ========================================================================
    // FUSE-specific operations (isolated from Mirror Sync tables)
    // These methods operate on fuse_* tables only
    // ========================================================================

    /**
     * @brief Get FUSE metadata for a file
     * @param fileId Google Drive file ID
     * @return FuseMetadata structure or empty if not found
     */
    FuseMetadata getFuseMetadata(const QString& fileId) const;

    /**
     * @brief Get FUSE metadata by path
     * @param path Logical path in FUSE filesystem
     * @return FuseMetadata structure or empty if not found
     */
    FuseMetadata getFuseMetadataByPath(const QString& path) const;

    /**
     * @brief Save FUSE metadata for a file
     * @param metadata FuseMetadata structure to save
     * @return true if save successful
     */
    bool saveFuseMetadata(const FuseMetadata& metadata);

    /**
     * @brief Delete FUSE metadata for a file
     * @param fileId Google Drive file ID
     * @return true if deletion successful
     */
    bool deleteFuseMetadata(const QString& fileId);

    /**
     * @brief Get children of a folder in FUSE metadata
     * @param parentId Parent folder ID
     * @return List of FuseMetadata for children
     */
    QList<FuseMetadata> getFuseChildren(const QString& parentId) const;

    /**
     * @brief Get all FUSE metadata entries
     * @return List of all FuseMetadata in the database
     */
    QList<FuseMetadata> getAllFuseMetadata() const;

    /**
     * @brief Update paths of all children under a renamed directory
     * @param parentFileId File ID of the renamed directory
     * @param oldParentPath Old path of the directory (without trailing slash)
     * @param newParentPath New path of the directory (without trailing slash)
     * @return Number of children updated
     */
    int updateFuseChildrenPaths(const QString& parentFileId, const QString& oldParentPath,
                                const QString& newParentPath);

    // FUSE dirty file tracking

    /**
     * @brief Get all dirty files in FUSE mode with complete data
     * @return List of FuseDirtyFile entries pending upload
     */
    QList<FuseDirtyFile> getFuseDirtyFiles() const;

    /**
     * @brief Mark a FUSE file as dirty (needs upload)
     * @param fileId Google Drive file ID
     * @param path Logical path in FUSE filesystem
     * @return true if mark successful
     */
    bool markFuseDirty(const QString& fileId, const QString& path);

    /**
     * @brief Clear dirty flag for a FUSE file
     * @param fileId Google Drive file ID
     * @return true if clear successful
     */
    bool clearFuseDirty(const QString& fileId);

    /**
     * @brief Mark a dirty file upload as failed
     * @param fileId Google Drive file ID
     * @return true if update successful
     */
    bool markFuseUploadFailed(const QString& fileId);

    /**
     * @brief Clear all FUSE cache entries from database
     * @return true if successful
     */
    bool clearAllFuseCacheEntries();

    // FUSE cache entry management

    /**
     * @brief Get all FUSE cache entries
     * @return List of FuseCacheEntry structures
     */
    QList<FuseCacheEntry> getFuseCacheEntries() const;

    /**
     * @brief Record a new FUSE cache entry
     * @param fileId Google Drive file ID
     * @param cachePath Local cache path
     * @param size File size in bytes
     * @return true if record successful
     */
    bool recordFuseCacheEntry(const QString& fileId, const QString& cachePath, qint64 size);

    /**
     * @brief Update last access time for a FUSE cache entry
     * @param fileId Google Drive file ID
     * @return true if update successful
     */
    bool updateCacheAccessTime(const QString& fileId);

    /**
     * @brief Evict (remove) a FUSE cache entry
     * @param fileId Google Drive file ID
     * @return true if eviction successful
     */
    bool evictFuseCacheEntry(const QString& fileId);

    // FUSE sync state (change tokens, etc.)

    /**
     * @brief Clear all user data from every table in the database
     *
     * Deletes rows from: files, settings, conflicts, conflict_versions,
     * deleted_files, fuse_metadata, fuse_cache_entries, fuse_dirty_files,
     * fuse_sync_state.  Used on account sign-out to prevent data leaks
     * and conflicts when a different account signs in.
     *
     * @return true if all tables were cleared successfully
     */
    bool clearAllData();

    /**
     * @brief Get FUSE sync state value
     * @param key State key
     * @return State value or empty string
     */
    QString getFuseSyncState(const QString& key) const;

    /**
     * @brief Set FUSE sync state value
     * @param key State key
     * @param value State value
     * @return true if set successful
     */
    bool setFuseSyncState(const QString& key, const QString& value);

   signals:
    /**
     * @brief Emitted when database error occurs
     * @param error Error message
     */
    void databaseError(const QString& error) const;

   private:
    bool createTables();
    bool createFuseTables();  ///< Create FUSE-specific tables
    bool ensureSettingsTable();
    bool migrateDatabase(int currentVersion);
    bool migrateFromV1ToV2();
    int getStoredVersion() const;
    bool setStoredVersion(int version);
    void logError(const QString& operation, const QString& error) const;

    static bool isRelativePath(const QString& path);
    void requireRelativePath(const QString& path, const char* operation) const;
    void requireFileId(const QString& fileId, const char* operation) const;

    QSqlDatabase m_db;
    QString m_dbPath;
    mutable QRecursiveMutex m_mutex;                      ///< Thread-safe access to database
    mutable QAtomicInteger<int> m_concurrentAccessCount;  ///< Track concurrent access attempts
    static const QString DB_NAME;
    static const int DB_VERSION;
};

#endif  // SYNCDATABASE_H
