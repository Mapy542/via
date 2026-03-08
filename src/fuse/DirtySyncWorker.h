/**
 * @file DirtySyncWorker.h
 * @brief Background worker for uploading dirty (locally modified) files in FUSE mode
 *
 * The DirtySyncWorker periodically checks for files that have been modified locally
 * through the FUSE filesystem and uploads them to Google Drive. This component
 * is part of the FUSE Mode Components as defined in the FUSE Procedure Flow Chart.
 *
 * Key responsibilities (from FUSE Procedure Flow Chart):
 * - Periodically poll for dirty files (default: every 5 seconds)
 * - Query FuseDatabase.fuse_dirty_files for files needing upload
 * - Batch upload dirty files via GoogleDriveClient
 * - Clear dirty flags after successful upload
 * - Mark upload failures for retry
 * - Configurable sync interval
 *
 * Threading model:
 * - This QObject can be moved to a separate QThread by the owner (e.g., FuseDriver)
 * - Uses QTimer for periodic polling, which operates in the thread's event loop
 * - Communicates via signals/slots with FileCache and GoogleDriveClient
 * - Thread-safe access to shared state via mutex
 *
 * Integration points:
 * - FileCache: provides getDirtyFiles(), clearDirty(), markUploadFailed()
 * - SyncDatabase: persistent dirty file tracking (fuse_dirty_files table)
 * - GoogleDriveClient: performs actual uploads via updateFile()
 * - FuseDriver: starts/stops worker when FUSE mounts/unmounts
 */

#ifndef DIRTYSYNCWORKER_H
#define DIRTYSYNCWORKER_H

#include <QDateTime>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include "api/DriveFile.h"

// Forward declarations
class FileCache;
class GoogleDriveClient;
class SyncDatabase;
struct DirtyFileEntry;

/**
 * @enum DirtySyncWorkerState
 * @brief Operational states for the dirty sync worker
 */
enum class DirtySyncWorkerState {
    Stopped,   ///< Worker is not running
    Running,   ///< Worker is actively processing
    Paused,    ///< Worker is paused (mount active but sync disabled)
    Uploading  ///< Currently uploading files
};

/**
 * @struct UploadResult
 * @brief Result of an upload operation
 */
struct UploadResult {
    QString fileId;        ///< File ID that was uploaded
    QString path;          ///< File path
    bool success;          ///< Whether upload succeeded
    QString errorMessage;  ///< Error message if failed
    QDateTime uploadedAt;  ///< When upload completed
};

Q_DECLARE_METATYPE(DirtySyncWorkerState)
Q_DECLARE_METATYPE(UploadResult)

/**
 * @class DirtySyncWorker
 * @brief Background worker for periodic upload of dirty files
 *
 * This class implements the "Dirty Files Sync Thread" from the FUSE Procedure
 * Flow Chart. It periodically checks for files marked dirty in the FileCache
 * and uploads them to Google Drive.
 *
 * Lifecycle:
 * 1. Created by FuseDriver when FUSE is enabled
 * 2. Started when FUSE filesystem mounts
 * 3. Periodically polls for dirty files at configurable interval
 * 4. Stopped when FUSE unmounts (flushes remaining dirty files first)
 *
 * Error handling:
 * - Network errors: Mark file for retry, continue with next file
 * - Auth errors: Pause and wait for token refresh
 * - Rate limit: Implement exponential backoff
 *
 * The worker uses Qt's signal/slot mechanism to coordinate with the main
 * thread where GoogleDriveClient operates, ensuring thread safety.
 */
class DirtySyncWorker : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the dirty sync worker
     * @param fileCache Pointer to file cache (for dirty file queries)
     * @param driveClient Pointer to Google Drive API client
     * @param database Pointer to sync database
     * @param parent Parent QObject
     */
    explicit DirtySyncWorker(FileCache* fileCache, GoogleDriveClient* driveClient,
                             SyncDatabase* database, QObject* parent = nullptr);

    ~DirtySyncWorker() override;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Get the sync interval in milliseconds
     * @return Sync interval (default: 5000ms as per procedure chart)
     */
    int syncIntervalMs() const;

    /**
     * @brief Set the sync interval
     * @param ms Interval in milliseconds (minimum: 1000ms)
     */
    void setSyncIntervalMs(int ms);

    /**
     * @brief Get the maximum retries for failed uploads
     * @return Maximum retry count (default: 3)
     */
    int maxRetries() const;

    /**
     * @brief Set the maximum retries for failed uploads
     * @param count Maximum retry count
     */
    void setMaxRetries(int count);

    /**
     * @brief Get the upload timeout in milliseconds
     * @return Upload timeout (default: 300000ms = 5 minutes)
     */
    int uploadTimeoutMs() const;

    /**
     * @brief Set the upload timeout
     * @param ms Timeout in milliseconds (minimum: 10000ms)
     */
    void setUploadTimeoutMs(int ms);

    /**
     * @brief Get the current state
     * @return Current worker state
     */
    DirtySyncWorkerState state() const;

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * @brief Get number of pending dirty files
     * @return Number of files waiting for upload
     */
    int pendingCount() const;

    /**
     * @brief Get number of files uploaded since last start
     * @return Upload count
     */
    int uploadedCount() const;

    /**
     * @brief Get number of failed uploads since last start
     * @return Failed upload count
     */
    int failedCount() const;

   public slots:
    /**
     * @brief Start the worker
     *
     * Begins periodic polling for dirty files.
     * Called when FUSE filesystem mounts.
     */
    void start();

    /**
     * @brief Stop the worker
     *
     * Stops polling. Does NOT flush remaining dirty files.
     * Use flushAndStop() for graceful shutdown.
     */
    void stop();

    /**
     * @brief Pause the worker
     *
     * Temporarily stops polling without stopping the thread.
     * Use when sync should be disabled but FUSE remains mounted.
     */
    void pause();

    /**
     * @brief Resume the worker after pause
     */
    void resume();

    /**
     * @brief Flush all dirty files and then stop
     *
     * Used during FUSE unmount to ensure all changes are uploaded.
     * This is a blocking operation that waits for all uploads to complete
     * or fail.
     */
    void flushAndStop();

    /**
     * @brief Trigger an immediate sync cycle
     *
     * Useful for forcing upload without waiting for timer.
     */
    void syncNow();

   signals:
    /**
     * @brief Emitted when state changes
     * @param state New state
     */
    void stateChanged(DirtySyncWorkerState state);

    /**
     * @brief Emitted when a file upload starts
     * @param fileId File being uploaded
     * @param path File path
     */
    void uploadStarted(const QString& fileId, const QString& path);

    /**
     * @brief Emitted when a file upload completes successfully
     * @param fileId Uploaded file ID
     * @param path File path
     */
    void uploadCompleted(const QString& fileId, const QString& path);

    /**
     * @brief Emitted when a file upload fails
     * @param fileId File ID
     * @param path File path
     * @param error Error message
     */
    void uploadFailed(const QString& fileId, const QString& path, const QString& error);

    /**
     * @brief Emitted when sync cycle completes
     * @param uploadedCount Number of files uploaded
     * @param failedCount Number of files that failed
     */
    void syncCycleCompleted(int uploadedCount, int failedCount);

    /**
     * @brief Emitted on error
     * @param error Error message
     */
    void error(const QString& error);

    /**
     * @brief Emitted when flush completes
     * @param success Whether all files were uploaded successfully
     */
    void flushCompleted(bool success);

   private slots:
    /**
     * @brief Handle sync timer timeout
     */
    void onSyncTimerTimeout();

    /**
     * @brief Handle file upload completion from GoogleDriveClient
     * @param file Uploaded file metadata
     */
    void onFileUploaded(const DriveFile& file);

    /**
     * @brief Handle upload error from GoogleDriveClient (detailed variant)
     * @param operation Operation that failed
     * @param errorMsg Error message
     * @param httpStatus HTTP status code
     * @param fileId File ID associated with the error
     * @param localPath Local path associated with the request
     */
    void onUploadErrorDetailed(const QString& operation, const QString& errorMsg, int httpStatus,
                               const QString& fileId, const QString& localPath);

   private:
    /**
     * @brief Process dirty files queue
     *
     * Core sync logic:
     * 1. Query dirty files from FileCache
     * 2. For each dirty file, initiate upload
     * 3. Wait for upload completion
     * 4. Clear dirty flag on success or mark failed on error
     */
    void processDirtyFiles();

    /**
     * @brief Upload a single file
     * @param fileId File ID to upload
     * @param path File path
     * @return true if upload was initiated successfully
     */
    bool uploadFile(const QString& fileId, const QString& path);

    /**
     * @brief Set the worker state
     * @param newState New state
     */
    void setState(DirtySyncWorkerState newState);

    // Dependencies
    FileCache* m_fileCache;
    GoogleDriveClient* m_driveClient;
    SyncDatabase* m_database;

    // Timer for periodic sync
    QTimer* m_syncTimer;

    // State
    DirtySyncWorkerState m_state;
    mutable QMutex m_mutex;

    // Configuration
    int m_syncIntervalMs;
    int m_maxRetries;
    int m_uploadTimeoutMs;

    // Statistics
    int m_uploadedCount;
    int m_failedCount;

    // Current upload tracking
    QString m_currentUploadFileId;
    bool m_uploadInProgress;
    bool m_uploadDone;  ///< Set by signal handlers to guard against lost wakeups
    QWaitCondition m_uploadCondition;
    bool m_uploadSuccess;
    QString m_uploadError;
    DriveFile m_lastUploadedFile;  ///< Populated by onFileUploaded for metadata update

    // Retry tracking  (GPT5.3 #8)
    QMap<QString, int> m_retryCounts;

    // Flush state
    bool m_flushing;
    QWaitCondition m_flushCondition;

    // Default sync interval: 5 seconds (as per FUSE Procedure Flow Chart)
    static constexpr int DEFAULT_SYNC_INTERVAL_MS = 5000;
    static constexpr int MIN_SYNC_INTERVAL_MS = 1000;
    static constexpr int DEFAULT_MAX_RETRIES = 3;
    static constexpr int DEFAULT_UPLOAD_TIMEOUT_MS = 300000;  // 5 minutes
    static constexpr int MIN_UPLOAD_TIMEOUT_MS = 10000;       // 10 seconds
};

#endif  // DIRTYSYNCWORKER_H
