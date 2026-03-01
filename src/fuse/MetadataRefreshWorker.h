/**
 * @file MetadataRefreshWorker.h
 * @brief Background worker for refreshing FUSE metadata from remote changes
 *
 * Polls Google Drive for remote changes and updates local caches accordingly.
 * Implements the "Metadata Refresh Thread" from the FUSE procedure flow chart.
 *
 * Flow (from dataflow.md):
 *   METADATA_REFRESH_THREAD --> POLL_CHANGES{Poll for Remote Changes default: every 30s}
 *   POLL_CHANGES --> GET_CHANGE_TOKEN(Get token from FuseDatabase.fuse_sync_state)
 *   GET_CHANGE_TOKEN --> API_LIST_CHANGES(Queue API: List Changes Since Token)
 *   API_LIST_CHANGES --> PROCESS_CHANGES{Changes Received?}
 *   PROCESS_CHANGES --Modified--> INVALIDATE_CACHE
 *   PROCESS_CHANGES --Deleted--> DELETE_META_DB(Delete from FuseDatabase.fuse_metadata)
 *   PROCESS_CHANGES --Created--> UPDATE_META_CACHE
 *   DELETE_META_DB --> UPDATE_CHANGE_TOKEN(Update Change Token)
 *   UPDATE_META_CACHE --> SAVE_META_DB(Save to FuseDatabase.fuse_metadata)
 *   SAVE_META_DB --> UPDATE_CHANGE_TOKEN
 *   UPDATE_CHANGE_TOKEN --> SAVE_TOKEN_DB(Save to FuseDatabase.fuse_sync_state)
 *   SAVE_TOKEN_DB --> SLEEP_REFRESH(Sleep default: 30s)
 *   SLEEP_REFRESH --> POLL_CHANGES
 *
 * Integration Points:
 * - MetadataRefreshWorker --> GoogleDriveClient: listChanges API
 * - MetadataRefreshWorker --> FileCache: invalidate() for modified files
 * - MetadataRefreshWorker --> MetadataCache: update/remove metadata
 * - MetadataRefreshWorker --> SyncDatabase: read/write fuse_sync_state for change token
 */

#ifndef METADATAREFRESHWORKER_H
#define METADATAREFRESHWORKER_H

#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>

// Forward declarations
class GoogleDriveClient;
class MetadataCache;
class FileCache;
class SyncDatabase;
struct DriveChange;
struct DriveFile;

/**
 * @class MetadataRefreshWorker
 * @brief Background worker that polls for remote changes and updates FUSE caches
 *
 * This worker runs in the background (using QTimer for periodic polling) and:
 * 1. Polls Google Drive Changes API at configurable intervals (default 30s)
 * 2. Processes changes: created, modified, deleted files
 * 3. Updates MetadataCache for created/modified files
 * 4. Invalidates FileCache entries for modified files (forces re-download)
 * 5. Removes entries from MetadataCache for deleted files
 * 6. Persists change token in SyncDatabase for incremental updates
 *
 * Thread Safety:
 * - Uses QTimer (event-driven, runs in main thread context)
 * - All cache operations are delegated to thread-safe cache classes
 * - Change token access is protected by mutex
 */
class MetadataRefreshWorker : public QObject {
    Q_OBJECT

   public:
    /**
     * @enum State
     * @brief Operating state of the worker
     */
    enum class State { Stopped, Running, Paused };

    /**
     * @brief Construct the metadata refresh worker
     * @param metadataCache Pointer to the metadata cache for updates
     * @param fileCache Pointer to the file cache for invalidation
     * @param database Pointer to sync database for token persistence
     * @param driveClient Pointer to Google Drive API client
     * @param parent Parent QObject
     */
    explicit MetadataRefreshWorker(MetadataCache* metadataCache, FileCache* fileCache,
                                   SyncDatabase* database, GoogleDriveClient* driveClient,
                                   QObject* parent = nullptr);

    ~MetadataRefreshWorker() override;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set the polling interval
     * @param intervalMs Interval in milliseconds (default: 30000 = 30 seconds)
     *
     * From FUSE Procedure Flow Chart: "Poll for Remote Changes default: every 30s"
     */
    void setPollingInterval(int intervalMs);

    /**
     * @brief Get the polling interval
     * @return Interval in milliseconds
     */
    int pollingInterval() const;

    /**
     * @brief Set the change page token
     * @param token Token from Google Drive changes.list API
     *
     * Normally loaded from database on start. Use for manual override.
     */
    void setChangeToken(const QString& token);

    /**
     * @brief Get the current change token
     * @return Current change page token
     */
    QString changeToken() const;

    /**
     * @brief Get current state
     * @return Current operating state
     */
    State state() const;

   public slots:
    /**
     * @brief Start the refresh worker
     *
     * Loads change token from database (fuse_sync_state), starts polling timer.
     * If no token exists, requests start page token from API first.
     */
    void start();

    /**
     * @brief Stop the refresh worker completely
     *
     * Stops polling timer and clears state.
     */
    void stop();

    /**
     * @brief Pause the refresh worker temporarily
     *
     * Suspends polling but maintains state.
     */
    void pause();

    /**
     * @brief Resume the refresh worker after pause
     *
     * Resumes polling and immediately checks for changes.
     */
    void resume();

    /**
     * @brief Manually trigger an immediate check for changes
     *
     * Can be called to check for changes without waiting for the poll interval.
     */
    void checkNow();

   signals:
    /**
     * @brief Emitted when worker state changes
     * @param state New state
     */
    void stateChanged(MetadataRefreshWorker::State state);

    /**
     * @brief Emitted when an error occurs
     * @param error Error message
     */
    void error(const QString& error);

    /**
     * @brief Emitted when the change token is updated
     * @param token New change token
     */
    void changeTokenUpdated(const QString& token);

    /**
     * @brief Emitted when a remote change is processed
     * @param fileId File ID that was changed
     * @param changeType Type of change: "created", "modified", "deleted"
     */
    void changeProcessed(const QString& fileId, const QString& changeType);

    /**
     * @brief Emitted when refresh cycle completes
     * @param changesCount Number of changes processed
     */
    void refreshCompleted(int changesCount);

   private slots:
    /**
     * @brief Handle polling timer timeout
     */
    void onPollingTimeout();

    /**
     * @brief Handle changes received from Google Drive API
     * @param changes List of changes
     * @param newToken New change page token for next request
     */
    void onChangesReceived(const QList<DriveChange>& changes, const QString& newToken);

    /**
     * @brief Handle start page token received from API
     * @param token Start page token
     */
    void onStartPageTokenReceived(const QString& token);

    /**
     * @brief Handle API errors
     * @param operation Operation that failed
     * @param errorMessage Error message
     */
    void onApiError(const QString& operation, const QString& errorMessage);

   private:
    /**
     * @brief Process a single change from the API
     * @param change The change to process
     *
     * Handles three cases per flow chart:
     * - Created: Update MetadataCache with new file info
     * - Modified: Invalidate FileCache + update MetadataCache
     * - Deleted: Remove from MetadataCache (FileCache auto-handles)
     */
    void processChange(const DriveChange& change);

    /**
     * @brief Update MetadataCache with file information
     * @param file File metadata from API
     */
    void updateMetadataCache(const DriveFile& file);

    /**
     * @brief Remove file from caches (for deleted files)
     * @param fileId Google Drive file ID
     */
    void removeFromCaches(const QString& fileId);

    /**
     * @brief Invalidate file cache entry (for modified files)
     * @param fileId Google Drive file ID
     *
     * Forces re-download on next access.
     */
    void invalidateFileCache(const QString& fileId);

    /**
     * @brief Check if file should be processed
     * @param file File metadata to check
     * @return true if file should be processed
     *
     * Filters: ownedByMe only, excludes Google Docs, excludes trashed
     */
    bool shouldProcess(const DriveFile& file) const;

    /**
     * @brief Load change token from database
     * @return Stored token or empty string
     */
    QString loadChangeToken() const;

    /**
     * @brief Save change token to database
     * @param token Token to persist
     */
    void saveChangeToken(const QString& token);

    // Dependencies
    MetadataCache* m_metadataCache;
    FileCache* m_fileCache;
    SyncDatabase* m_database;
    GoogleDriveClient* m_driveClient;

    // Polling timer
    QTimer* m_pollingTimer;

    // State
    QString m_changeToken;
    State m_state;
    mutable QMutex m_mutex;
    bool m_waitingForToken;

    // Constants
    static const int DEFAULT_POLL_INTERVAL_MS = 30000;  // 30 seconds per flow chart
    static const QString FUSE_CHANGE_TOKEN_KEY;         // Key for fuse_sync_state table
};

#endif  // METADATAREFRESHWORKER_H
