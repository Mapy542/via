/**
 * @file SyncActionThread.h
 * @brief Sync Action Thread for executing sync operations
 *
 * Implements the Sync Action Thread from the sync procedure flow chart.
 * Takes sync actions from the Sync Action Queue and executes them by
 * making Drive API calls or performing local file system operations.
 */

#ifndef SYNCACTIONTHREAD_H
#define SYNCACTIONTHREAD_H

#include <QHash>
#include <QMutex>
#include <QObject>

#include "sync/SyncActionQueue.h"

class SyncActionQueue;
class SyncDatabase;
class GoogleDriveClient;
class ChangeProcessor;
class LocalChangeWatcher;
struct SyncActionItem;
struct DriveFile;

#define MAX_CONCURRENT_DRIVE_ACTIONS 5

/**
 * @class SyncActionThread
 * @brief Executes sync actions from the Sync Action Queue
 *
 * This class implements the Sync Action Thread from the sync procedure
 * flow chart. It:
 *
 * 1. Receives sync action items from the Sync Action Queue
 * 2. Marks files as "in operation" to prevent re-processing
 * 3. Executes the action (Drive API call or local file operation)
 * 4. Updates local file metadata if applicable
 * 5. Updates the file's last modified time in the database
 * 6. Unmarks the file from "in operation"
 *
 * Threading model:
 * - Runs on a dedicated processing thread
 * - Receives wakeup signals from Sync Action Queue when items available
 * - Makes asynchronous Drive API calls
 * - Performs synchronous local file operations
 *
 * Flow chart reference:
 * - Takes from: Sync Action Queue (SAQ)
 * - Signal: Jobs Available Wakeup Signal/Slot (JAW)
 * - Outputs: Done Signal with completed SyncActionItem
 */
class SyncActionThread : public QObject {
    Q_OBJECT

   public:
    /**
     * @enum State
     * @brief Operating state of the thread
     */
    enum class State { Stopped, Running, Paused };

    /**
     * @brief Construct the sync action thread
     * @param actionQueue Pointer to the sync action queue
     * @param database Pointer to the sync database
     * @param driveClient Pointer to the Google Drive API client
     * @param changeProcessor Pointer to change processor (for in-operation tracking)
     * @param localWatcher Pointer to local change watcher (optional, for watch list updates)
     * @param parent Parent object
     */
    explicit SyncActionThread(SyncActionQueue* actionQueue, SyncDatabase* database,
                              GoogleDriveClient* driveClient, ChangeProcessor* changeProcessor,
                              LocalChangeWatcher* localWatcher = nullptr,
                              QObject* parent = nullptr);

    ~SyncActionThread() override;

    /**
     * @brief Get current state
     * @return Current operating state
     */
    State state() const;

    /**
     * @brief Set the sync folder path
     * @param path Absolute path to the sync folder
     *
     * All localPath values in sync actions are relative to this folder.
     */
    void setSyncFolder(const QString& path);

    /**
     * @brief Get the sync folder path
     * @return Absolute path to the sync folder
     */
    QString syncFolder() const;

    /**
     * @brief Clear drive-actions-in-progress map and retry state
     *
     * Called on account sign-out to discard any pending async
     * action tracking from the previous session.
     */
    void clearInProgressActions();

   public slots:
    /**
     * @brief Start processing sync actions
     *
     * Begins the processing loop.
     */
    void start();

    /**
     * @brief Stop processing completely
     *
     * Stops the processing loop.
     */
    void stop();

    /**
     * @brief Pause processing temporarily
     *
     * Suspends action processing but maintains state.
     */
    void pause();

    /**
     * @brief Resume processing after pause
     *
     * Resumes the processing loop.
     */
    void resume();

    /**
     * @brief Wake up to check for new items
     *
     * Connected to Sync Action Queue's itemsAvailable() signal.
     * This is the "Jobs Available Wakeup Signal/Slot" (JAW) from the flow chart.
     */
    void onItemsAvailable();

   signals:
    /**
     * @brief Emitted when thread state changes
     * @param state New state
     */
    void stateChanged(SyncActionThread::State state);

    /**
     * @brief Emitted when an action completes successfully
     * @param item The completed sync action item
     */
    void actionCompleted(const SyncActionItem& item);

    /**
     * @brief Emitted when an action fails
     * @param item The failed sync action item
     * @param error Error message
     */
    void actionFailed(const SyncActionItem& item, const QString& error);

    /**
     * @brief Emitted on progress during upload/download
     * @param item Current action item
     * @param bytesProcessed Bytes processed so far
     * @param bytesTotal Total bytes
     */
    void actionProgress(const SyncActionItem& item, qint64 bytesProcessed, qint64 bytesTotal);

    /**
     * @brief Emitted when an error occurs
     * @param error Error message
     */
    void error(const QString& error);

    /**
     * @brief Request token refresh after auth-related Drive failures
     */
    void tokenRefreshRequested();

   private slots:
    /**
     * @brief Process the next action from the queue
     */
    void processNextAction();

    // Drive API response handlers
    void onFileUploaded(const DriveFile& file);
    void onFileUploadedDetailed(const DriveFile& file, const QString& localPath);
    void onFileUpdated(const DriveFile& file);
    void onFileDownloaded(const QString& fileId, const QString& localPath);
    void onFileMoved(const QString& fileId);
    void onFileMovedDetailed(const DriveFile& file);
    void onFileRenamed(const QString& fileId);
    void onFileRenamedDetailed(const DriveFile& file);
    void onFileDeleted(const QString& fileId);
    void onFolderCreatedDetailed(const DriveFile& folder, const QString& localPath);
    void onDriveError(const QString&, const QString&);
    void onDriveErrorDetailed(const QString& operation, const QString& errorMsg, int httpStatus,
                              const QString& fileId, const QString& localPath);

   private:
    /**
     * @brief Execute an upload action
     * @param item The sync action item
     */
    void executeUpload(const SyncActionItem& item);

    /**
     * @brief Execute a download action
     * @param item The sync action item
     */
    void executeDownload(const SyncActionItem& item);

    /**
     * @brief Execute a delete local action
     * @param item The sync action item
     */
    void executeDeleteLocal(const SyncActionItem& item);
    bool deleteLocalPathRecursive(const QString& localPath, const QString& fileId = QString());

    /**
     * @brief Execute a delete remote action
     * @param item The sync action item
     */
    void executeDeleteRemote(const SyncActionItem& item);

    /**
     * @brief Execute a move local action
     * @param item The sync action item
     */
    void executeMoveLocal(const SyncActionItem& item);

    /**
     * @brief Execute a move remote action
     * @param item The sync action item
     */
    void executeMoveRemote(const SyncActionItem& item);

    /**
     * @brief Execute a rename local action
     * @param item The sync action item
     */
    void executeRenameLocal(const SyncActionItem& item);

    /**
     * @brief Execute a rename remote action
     * @param item The sync action item
     */
    void executeRenameRemote(const SyncActionItem& item);

    /**
     * @brief Convert relative path to absolute path
     * @param relativePath Path relative to sync folder
     * @return Absolute path
     */
    QString toAbsolutePath(const QString& relativePath) const;

    QString resolveUniqueLocalPath(const QString& desiredLocalPath, const QString& fileId,
                                   const QString& currentLocalPath,
                                   bool reuseExistingMapping) const;
    QString buildDisambiguatedPath(const QString& desiredLocalPath, const QString& fileId,
                                   int counter) const;
    bool isPathAvailableForFileId(const QString& localPath, const QString& fileId,
                                  const QString& currentLocalPath) const;
    bool resolveRemoteParentId(const QString& parentPath, QString& parentId,
                               bool forceRefresh = false);
    bool deferUntilRemoteParentReady(const QString& parentPath, const SyncActionItem& item);
    bool scheduleRetry(const SyncActionItem& item, const QString& reason, int baseDelayMs = 250);
    void clearRetryState(const SyncActionItem& item);

    /**
     * @brief Update database after successful action completion
     * @param item The completed action
     * @param fileId The Google Drive file ID (may be new for uploads)
     * @param modifiedTime The modification time to record
     */
    void updateDatabaseAfterAction(const SyncActionItem& item, const QString& fileId,
                                   const QDateTime& modifiedTime,
                                   const QString& remoteMd5 = QString(),
                                   const QString& localHash = QString());

    /**
     * @brief Compute local file MD5 hash by relative local path
     * @param localPath Relative path within sync root
     * @return Lowercase hex MD5 hash or empty string on failure
     */
    QString computeLocalFileMd5(const QString& localPath) const;

    /**
     * @brief Mark action as complete, unmark file in operation, continue processing
     * @param item The completed action item
     */
    void completeAction(const SyncActionItem& item);

    /**
     * @brief Update local file metadata (modification time)
     * @param localPath Local file path
     * @param modifiedTime New modification time
     */
    void updateLocalMetadata(const QString& localPath, const QDateTime& modifiedTime);

    /**
     * @brief Handle action failure, unmark file in operation, continue processing
     * @param item The failed action item
     * @param errorMsg Error message
     */
    void failAction(const SyncActionItem& item, const QString& errorMsg);

    QString actionKeyForFileId(const QString& fileId) const;
    QString actionKeyForLocalPath(const QString& localPath) const;
    QString actionKeyForItem(const SyncActionItem& item) const;
    QString normalizeLocalPath(const QString& path) const;
    void trackActionInProgress(const SyncActionItem& item);
    void untrackActionInProgress(const SyncActionItem& item);

    // Core components
    SyncActionQueue* m_actionQueue;
    SyncDatabase* m_database;
    GoogleDriveClient* m_driveClient;
    ChangeProcessor* m_changeProcessor;
    LocalChangeWatcher* m_localWatcher;

    // State
    State m_state;
    mutable QMutex m_stateMutex;
    QString m_syncFolder;

    // Current action being processed (for async API response handling)
    QMap<QString, SyncActionItem> m_driveActionsInProgress;
    mutable QMutex m_driveActionsMutex;

    // Processing flag (Start / Stop control)
    bool m_processingActive;

    // Retry tracking for transient/auth recoverable failures
    QHash<QString, int> m_retryCounts;
    qint64 m_lastTokenRefreshRequestMs = 0;

    static constexpr int MAX_RETRY_ATTEMPTS = 3;
    static constexpr int TOKEN_REFRESH_COOLDOWN_MS = 30000;
};

#endif  // SYNCACTIONTHREAD_H
