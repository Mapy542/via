/**
 * @file SyncDatabase.h
 * @brief SQLite database for tracking file sync state
 */

#ifndef SYNCDATABASE_H
#define SYNCDATABASE_H

#include <QAtomicInteger>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <memory>

#include "utils/NativeDocSupport.h"

class SyncDatabase;

class SyncDatabaseConnectionHandle {
   public:
    explicit SyncDatabaseConnectionHandle(const SyncDatabase* owner = nullptr);

    operator QSqlDatabase() const;
    bool isOpen() const;
    QString connectionName() const;
    QStringList tables() const;
    QSqlError lastError() const;

   private:
    QSqlDatabase database() const;

    const SyncDatabase* m_owner = nullptr;
};

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
    QString fileId;                 ///< Google Drive file ID (primary key)
    QString path;                   ///< Logical path in FUSE filesystem
    QString name;                   ///< File/folder name
    QString remoteName;             ///< Original Google Drive name before aliasing
    QString nativeDocModeOverride;  ///< Per-file native-doc representation override
    QString parentId;               ///< Parent folder ID
    bool isFolder;                  ///< Whether this is a folder
    qint64 size;                    ///< File size in bytes
    QString mimeType;               ///< MIME type
    QString remoteMimeType;         ///< Original Google-native MIME type (e.g.
                                    ///< application/vnd.google-apps.document)
    QString webViewLink;            ///< Google Drive web view URL
    QDateTime createdTime;          ///< Creation timestamp
    QDateTime modifiedTime;         ///< Last modification timestamp
    QDateTime cachedAt;             ///< When metadata was cached
    QDateTime lastAccessed;         ///< Last access time for LRU
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
    QString fileId;                  ///< Google Drive file ID (primary key)
    QString path;                    ///< Logical path in FUSE filesystem
    QDateTime markedDirtyAt;         ///< When file was marked dirty
    QDateTime lastUploadAttempt;     ///< Last upload attempt time
    bool uploadFailed;               ///< Whether last upload failed
    quint64 generation = 0;          ///< Latest local dirty generation
    quint64 uploadedGeneration = 0;  ///< Latest local generation confirmed uploaded
};

/**
 * @struct FuseNode
 * @brief Authoritative local FUSE node persisted independently of remote IDs
 *
 * Maps to fuse_nodes table schema.
 */
struct FuseNode {
    QString nodeId;                 ///< Stable local node identity
    QString parentNodeId;           ///< Parent local node identity (empty for root children)
    QString remoteFileId;           ///< Google Drive file ID once replay is acknowledged
    QString remoteParentId;         ///< Remote parent ID when known
    QString path;                   ///< Visible logical path in FUSE filesystem
    QString name;                   ///< Visible basename
    QString remoteName;             ///< Remote-visible name when it diverges locally
    QString mimeType;               ///< Current local MIME type when known
    QString remoteMimeType;         ///< Remote Drive MIME type when known
    QString webViewLink;            ///< Remote web link for native docs or shortcuts
    QString nativeDocModeOverride;  ///< Per-node native-doc representation override
    bool isFolder = false;          ///< Whether the node represents a directory
    bool isPendingCreate = false;   ///< Whether the node still needs remote creation replay
    bool isTrashed = false;         ///< Whether the node currently lives in local trash state
    qint64 size = 0;                ///< Last authoritative local size
    QDateTime createdTime;          ///< Local creation timestamp
    QDateTime modifiedTime;         ///< Last local mutation timestamp
    QDateTime lastAccessed;         ///< Last access time for read-model and LRU decisions
    QDateTime lastSyncedAt;         ///< Last successful remote reconciliation time
};

/**
 * @struct FuseNodeContentState
 * @brief Authoritative local content state for a persisted FUSE node
 *
 * Maps to fuse_node_contents table schema.
 */
struct FuseNodeContentState {
    QString nodeId;                   ///< Stable local node identity
    QString localContentPath;         ///< Absolute path to authoritative local content
    quint64 localGeneration = 0;      ///< Latest committed local content generation
    quint64 remoteAckGeneration = 0;  ///< Latest local generation acknowledged remotely
    qint64 size = 0;                  ///< Last committed content size in bytes
    QDateTime lastLocalWrite;         ///< Last successful local write timestamp
};

/**
 * @enum FuseJournalOperationType
 * @brief Replayable local FUSE operation categories persisted in the journal
 */
enum class FuseJournalOperationType {
    CreateFile = 0,
    CreateDirectory,
    WriteGeneration,
    Truncate,
    Rename,
    Move,
    Trash,
    Delete,
    Restore,
    UpdateNativeDocMetadata,
    UpdateShortcutMetadata,
};

/**
 * @enum FuseJournalEntryStatus
 * @brief Durable lifecycle state for a replayable journal entry
 */
enum class FuseJournalEntryStatus {
    Pending = 0,
    InFlight,
    Completed,
    Failed,
    BlockedConflict,
};

/**
 * @struct FuseJournalEntry
 * @brief Durable local mutation intent recorded before remote replay
 *
 * Maps to fuse_journal table schema.
 */
struct FuseJournalEntry {
    qint64 entryId = 0;      ///< Monotonic journal sequence number
    QString idempotencyKey;  ///< Stable replay token for remote dedupe
    FuseJournalOperationType operationType = FuseJournalOperationType::CreateFile;
    FuseJournalEntryStatus status = FuseJournalEntryStatus::Pending;
    QString nodeId;                   ///< Local node identity being mutated
    QString parentNodeId;             ///< Source parent node identity when relevant
    QString destinationParentNodeId;  ///< Destination parent node identity when relevant
    QString path;                     ///< Source visible path when relevant
    QString visibleName;              ///< Source visible basename
    QString destinationPath;          ///< Destination visible path when relevant
    QString destinationVisibleName;   ///< Destination visible basename when relevant
    QString remoteFileId;             ///< Remote file ID snapshot when known
    QString remoteParentId;           ///< Remote parent ID snapshot when known
    quint64 localGeneration = 0;      ///< Local content generation tied to this intent
    qint64 dependencyEntryId = 0;     ///< Earlier journal entry that must replay first
    QString payloadJson;              ///< Extensible structured operation payload
    QString lastError;                ///< Last replay failure message
    int retryCount = 0;               ///< Retry attempts consumed so far
    QDateTime createdAt;              ///< When the journal entry was first appended
    QDateTime updatedAt;              ///< Last status transition time
    QDateTime acknowledgedAt;         ///< When remote replay was confirmed
};

/**
 * @struct FuseOperationAck
 * @brief Remote acknowledgement details recorded for replay bookkeeping
 *
 * Maps to fuse_operation_acks table schema.
 */
struct FuseOperationAck {
    qint64 ackId = 0;                    ///< Monotonic acknowledgement row ID
    qint64 journalEntryId = 0;           ///< Associated local journal entry ID
    QString idempotencyKey;              ///< Stable replay token acknowledged remotely
    QString nodeId;                      ///< Local node identity associated with the ack
    QString remoteFileId;                ///< Authoritative remote file ID when acknowledged
    QString remoteParentId;              ///< Authoritative remote parent ID when acknowledged
    quint64 acknowledgedGeneration = 0;  ///< Highest local generation acknowledged remotely
    QString remoteChangeToken;           ///< Remote change-token snapshot when available
    QString payloadJson;                 ///< Extensible remote response payload
    QString lastError;                   ///< Replay failure message associated with the ack
    QDateTime acknowledgedAt;            ///< When the remote replay was acknowledged
    QDateTime appliedAt;                 ///< When local state consumed the acknowledgement
};

/**
 * @struct FuseMutationTransaction
 * @brief Crash-safe local mutation batch persisted atomically with a journal entry
 */
struct FuseMutationTransaction {
    QList<FuseNode> nodesToUpsert;                      ///< Authoritative nodes to save
    QList<QString> nodeIdsToDelete;                     ///< Node IDs to delete from local state
    QList<FuseNodeContentState> contentStatesToUpsert;  ///< Content-state rows to save
    QList<QString> contentStateNodeIdsToDelete;         ///< Content-state node IDs to delete
    FuseJournalEntry journalEntry;                      ///< Replay intent appended in the same tx
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
    enum class SchemaCompatibility {
        Current,
        ResetRequired,
        ResetBlockedByDirtyState,
        UnsupportedFutureSchema,
    };

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
     * @brief Recreate the database file with the current schema baseline
     * @return true if the database was recreated successfully
     */
    bool recreateCurrentSchema();

    /**
     * @brief Close the database connection
     */
    void close();

    /**
     * @brief Close and remove the SQLite connection owned by the current thread
     *
     * Safe to call from worker threads before they shut down so the main thread
     * does not need to touch foreign-thread QSqlDatabase connections later.
     */
    void closeCurrentThreadConnection();

    /**
     * @brief Check if database is open
     * @return true if database is open
     */
    bool isOpen() const;

    /**
     * @brief Get the last detected schema compatibility state
     * @return Compatibility state from the most recent initialize attempt
     */
    SchemaCompatibility lastSchemaCompatibility() const;

    /**
     * @brief Check whether pending dirty uploads are still tracked in the DB
     * @return true when destructive reset could discard pending upload state
     */
    bool hasPendingDirtyUploads() const;

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
     * @brief Rewrite a folder mapping and all descendant paths as one tree update
     * @param fileId Google Drive file ID for the moved or renamed folder
     * @param oldLocalPath Previous folder path (relative)
     * @param newLocalPath New folder path (relative)
     * @return true when the folder row and descendant rows were updated successfully
     */
    bool updateLocalPathTree(const QString& fileId, const QString& oldLocalPath,
                             const QString& newLocalPath);

    /**
     * @brief Delete a file state by Drive file ID
     * @param fileId Google Drive file ID
     * @return true if deletion successful
     */
    bool deleteFileStateById(const QString& fileId);

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
    void setContentHashesAtSync(const QString& localPath, const QString& remoteMd5,
                                const QString& localHash);

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

    /**
     * @brief Get shared native-doc state for a file
     * @param fileId Google Drive file ID
     * @return NativeDocState or empty if not found
     */
    NativeDocState getNativeDocState(const QString& fileId) const;

    /**
     * @brief Save shared native-doc state keyed by file ID
     * @param state Native-doc state to save
     * @return true if save successful
     */
    bool saveNativeDocState(const NativeDocState& state);

    /**
     * @brief Delete shared native-doc state for a file
     * @param fileId Google Drive file ID
     * @return true if deletion successful
     */
    bool deleteNativeDocState(const QString& fileId);

    // Change token

    /**
     * @brief Get the stored change page token
     * @return Change page token or empty string if not set
     */
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
    int upsertConflictRecord(const QString& localPath, const QString& fileId,
                             const QString& conflictPath = QString());

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

    // FUSE authoritative offline node state

    /**
     * @brief Get an authoritative local FUSE node by local node ID
     * @param nodeId Stable local node identity
     * @return FuseNode structure or empty if not found
     */
    FuseNode getFuseNode(const QString& nodeId) const;

    /**
     * @brief Get an authoritative local FUSE node by visible FUSE path
     * @param path Visible logical FUSE path
     * @return FuseNode structure or empty if not found
     */
    FuseNode getFuseNodeByPath(const QString& path) const;

    /**
     * @brief Save or update an authoritative local FUSE node
     * @param node Persisted local node state
     * @return true if save successful
     */
    bool saveFuseNode(const FuseNode& node);

    /**
     * @brief Get immediate children of an authoritative local FUSE node
     * @param parentNodeId Stable local parent node identity
     * @return List of child nodes
     */
    QList<FuseNode> getFuseChildNodes(const QString& parentNodeId) const;

    /**
     * @brief Get all authoritative local FUSE nodes
     * @return List of all local nodes sorted by path
     */
    QList<FuseNode> getAllFuseNodes() const;

    /**
     * @brief Delete an authoritative local FUSE node by local node ID
     * @param nodeId Stable local node identity
     * @return true if deletion succeeded
     */
    bool deleteFuseNode(const QString& nodeId);

    /**
     * @brief Get persisted authoritative content state for a local FUSE node
     * @param nodeId Stable local node identity
     * @return FuseNodeContentState structure or empty if not found
     */
    FuseNodeContentState getFuseNodeContentState(const QString& nodeId) const;

    /**
     * @brief Save or update authoritative local content state for a FUSE node
     * @param state Persisted content state
     * @return true if save successful
     */
    bool saveFuseNodeContentState(const FuseNodeContentState& state);

    /**
     * @brief Get all persisted authoritative content-state rows for mount recovery
     * @return List of content-state rows sorted by node ID
     */
    QList<FuseNodeContentState> getAllFuseNodeContentStates() const;

    /**
     * @brief Delete authoritative content state for a local FUSE node
     * @param nodeId Stable local node identity
     * @return true if deletion succeeded
     */
    bool deleteFuseNodeContentState(const QString& nodeId);

    /**
     * @brief Append a new offline FUSE journal entry
     * @param entry Durable local mutation intent
     * @return Monotonic entry ID on success, or 0 on failure
     */
    qint64 appendFuseJournalEntry(const FuseJournalEntry& entry);

    /**
     * @brief Get all offline FUSE journal entries in append order
     * @return List of journal entries ordered by entry ID
     */
    QList<FuseJournalEntry> getAllFuseJournalEntries() const;

    /**
     * @brief Get pending offline FUSE journal entries in replay order
     * @return List of pending journal entries ordered by entry ID
     */
    QList<FuseJournalEntry> getPendingFuseJournalEntries() const;

    /**
     * @brief Update replay status for a persisted journal entry
     * @param entryId Journal entry ID
     * @param status New status value
     * @param lastError Optional replay failure message
     * @param retryCount Retry count to persist; pass negative to leave unchanged
     * @param acknowledgedAt Optional acknowledgement timestamp
     * @return true if update successful
     */
    bool updateFuseJournalEntryStatus(qint64 entryId, FuseJournalEntryStatus status,
                                      const QString& lastError = QString(), int retryCount = -1,
                                      const QDateTime& acknowledgedAt = QDateTime());

    /**
     * @brief Persist or update remote replay acknowledgement details
     * @param ack Replay acknowledgement row
     * @return true if save successful
     */
    bool saveFuseOperationAck(const FuseOperationAck& ack);

    /**
     * @brief Get remote replay acknowledgement details for a journal entry
     * @param journalEntryId Local journal entry ID
     * @return Acknowledgement row or empty if not found
     */
    FuseOperationAck getFuseOperationAck(qint64 journalEntryId) const;

    /**
     * @brief Get all remote replay acknowledgements in append order
     * @return List of acknowledgement rows ordered by ack ID
     */
    QList<FuseOperationAck> getAllFuseOperationAcks() const;

    /**
     * @brief Get the oldest unapplied replay acknowledgement for a remote file ID
     * @param remoteFileId Remote file ID to match
     * @return Acknowledgement row or empty if no unapplied ack exists
     */
    FuseOperationAck getPendingFuseOperationAckByRemoteFileId(const QString& remoteFileId) const;

    /**
     * @brief Mark a replay acknowledgement as consumed by metadata reconciliation
     * @param ackId Acknowledgement row ID
     * @return true if the update succeeded
     */
    bool markFuseOperationAckApplied(qint64 ackId);

    /**
     * @brief Atomically persist local node-tree changes with a replay journal entry
     * @param mutation Crash-safe local mutation batch
     * @param journalEntryIdOut Optional returned journal entry ID
     * @return true if the transaction committed successfully
     */
    bool commitFuseMutationTransaction(const FuseMutationTransaction& mutation,
                                       qint64* journalEntryIdOut = nullptr);

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
     * @param generation Latest local dirty generation for the file
     * @param uploadedGeneration Latest generation already uploaded to Drive
     * @return true if mark successful
     */
    bool markFuseDirty(const QString& fileId, const QString& path, quint64 generation = 1,
                       quint64 uploadedGeneration = 0);

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
     * @brief Record that a dirty generation was uploaded successfully
     * @param fileId Google Drive file ID
     * @param uploadedGeneration Latest local generation confirmed uploaded
     * @return true if update successful
     */
    bool markFuseUploadedGeneration(const QString& fileId, quint64 uploadedGeneration);

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
     * @brief Clear FUSE representation state for mode-change restart
     *
     * Clears fuse_metadata, fuse_cache_entries, and fuse_sync_state so
     * the FUSE mount rebuilds from scratch under the new serving mode.
     * Does NOT touch fuse_dirty_files (those should be flushed before
     * calling this) or any mirror-sync tables.
     *
     * @return true if all three tables were cleared successfully
     */
    bool clearFuseRepresentationState();

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
    friend class SyncDatabaseConnectionHandle;

    struct PreparedStatementCache {
        QString connectionName;
        QHash<QString, std::shared_ptr<QSqlQuery>> queries;
    };

    QSqlDatabase databaseForCurrentThread() const;
    QSqlDatabase databaseForCurrentThreadUnlocked() const;
    QSqlQuery* preparedQueryForCurrentThreadUnlocked(const QString& cacheKey, const QString& sql,
                                                     const char* operation) const;
    bool openConnectionUnlocked();
    void closeUnlocked();
    void clearPreparedStatementsForThreadUnlocked(quintptr threadKey);
    void clearAllPreparedStatementsUnlocked();
    void closeConnectionByNameUnlocked(const QString& connectionName);
    void closeAllConnectionsUnlocked();
    QString connectionNameForThreadUnlocked(quintptr threadKey) const;
    QString ensureConnectionNameForThreadUnlocked(quintptr threadKey);
    bool createTables();
    bool createFuseTables();  ///< Create FUSE-specific tables
    bool ensureSettingsTable();
    int getStoredSchemaEpoch() const;
    bool setStoredSchemaEpoch(int epoch);
    int getStoredVersion() const;
    bool setStoredVersion(int version);
    SchemaCompatibility detectSchemaCompatibility() const;
    bool recreateDatabaseUnlocked();
    static bool removeDatabaseFiles(const QString& dbPath);
    void logError(const QString& operation, const QString& error) const;

    static bool isRelativePath(const QString& path);
    void requireRelativePath(const QString& path, const char* operation) const;
    void requireFileId(const QString& fileId, const char* operation) const;

    mutable SyncDatabaseConnectionHandle m_db;
    QString m_dbPath;
    mutable QHash<quintptr, QString> m_connectionNamesByThread;
    mutable QHash<quintptr, PreparedStatementCache> m_preparedStatementsByThread;
    QString m_primaryConnectionName;
    bool m_connectionsReady = false;
    mutable QRecursiveMutex m_mutex;                      ///< Thread-safe access to database
    mutable QAtomicInteger<int> m_concurrentAccessCount;  ///< Track concurrent access attempts
    SchemaCompatibility m_lastSchemaCompatibility = SchemaCompatibility::Current;
    static const QString DB_NAME;
    static const QString SCHEMA_EPOCH_KEY;
    static const int DB_VERSION;
    static const int CURRENT_SCHEMA_EPOCH;
};

#endif  // SYNCDATABASE_H
