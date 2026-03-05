/**
 * @file ChangeProcessor.h
 * @brief Change Processor/Conflict Resolver Thread
 *
 * Implements the Change Processor/Conflict Resolver from the sync procedure
 * flow chart. Takes changes from the Change Queue, validates them, detects
 * and resolves conflicts, and outputs sync actions to the Sync Action Queue.
 */

#ifndef CHANGEPROCESSOR_H
#define CHANGEPROCESSOR_H

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>

#include "SyncSettings.h"

class ChangeQueue;
class SyncActionQueue;
class SyncDatabase;
class GoogleDriveClient;
struct ChangeQueueItem;
struct SyncActionItem;
struct DriveFile;

/**
 * @enum ConflictResolutionStrategy
 * @brief Strategies for resolving file conflicts
 */
enum class ConflictResolutionStrategy {
    KeepLocal,   ///< Keep local version, overwrite remote
    KeepRemote,  ///< Keep remote version, overwrite local
    KeepBoth,    ///< Keep both versions (create conflict copy)
    KeepNewest,  ///< Keep whichever version is newest
    AskUser      ///< Prompt user for decision
};

/**
 * @struct ConflictInfo
 * @brief Information about a detected conflict
 */
struct ConflictInfo {
    int conflictId = -1;           ///< Conflict record ID (if persisted)
    QString localPath;             ///< Local file path (relative to sync root)
    QString fileId;                ///< Google Drive file ID
    QDateTime localModifiedTime;   ///< Local file modification time
    QDateTime remoteModifiedTime;  ///< Remote file modification time
    QDateTime dbSyncTime;          ///< Last sync time recorded in database
    bool resolved = false;         ///< Whether conflict has been resolved
    bool isConflicted = false;     ///< Whether this is a true conflict (null return type)
};

Q_DECLARE_METATYPE(ConflictInfo)
Q_DECLARE_METATYPE(ConflictResolutionStrategy)

/**
 * @class ChangeProcessor
 * @brief Processes changes from watchers and outputs sync actions
 *
 * This class implements the Change Processor/Conflict Resolver Thread from
 * the sync procedure flow chart. It:
 *
 * 1. Receives change items from the Change Queue
 * 2. Validates changes (file not in operation, date > db + 2 sec)
 * 3. Detects conflicts (both local and remote newer than db sync time)
 * 4. Resolves conflicts using the configured strategy
 * 5. Outputs sync actions to the Sync Action Queue
 *
 * Threading model:
 * - Runs on a dedicated thread
 * - Receives wakeup signals from Change Queue when items available
 * - May block on long network calls during conflict detection
 * - Outputs actions to Sync Action Queue
 *
 * Flow chart reference:
 * - Takes from: Change Queue (CQ)
 * - Outputs to: Sync Action Queue (SAQ)
 * - Signal: Jobs Available Wakeup Signal/Slot (JAW2)
 */
class ChangeProcessor : public QObject {
    Q_OBJECT

   public:
    /**
     * @enum State
     * @brief Operating state of the processor
     */
    enum class State { Stopped, Running, Paused };

    /**
     * @brief Construct the change processor
     * @param changeQueue Pointer to the input change queue
     * @param syncActionQueue Pointer to the output sync action queue
     * @param database Pointer to the sync database
     * @param driveClient Pointer to the Google Drive client (for conflict checks)
     * @param parent Parent object
     */
    explicit ChangeProcessor(ChangeQueue* changeQueue, SyncActionQueue* syncActionQueue, SyncDatabase* database,
                             GoogleDriveClient* driveClient, QObject* parent = nullptr);

    ~ChangeProcessor() override;

    /**
     * @brief Get current state
     * @return Current operating state
     */
    State state() const;

    /**
     * @brief Set the conflict resolution strategy
     * @param strategy Strategy to use for conflict resolution
     */
    void setConflictResolutionStrategy(ConflictResolutionStrategy strategy);

    /**
     * @brief Get the current conflict resolution strategy
     * @return Current strategy
     */
    ConflictResolutionStrategy conflictResolutionStrategy() const;

    /**
     * @brief Get all unresolved conflicts
     * @return List of unresolved conflicts awaiting user decision
     */
    QList<ConflictInfo> unresolvedConflicts() const;

    /**
     * @brief Get count of unresolved conflicts
     * @return Number of unresolved conflicts
     */
    int unresolvedConflictCount() const;

    /**
     * @brief Check if a file is currently being processed
     * @param localPath Path to check
     * @return true if file is in the "in operation" set
     *
     * Files marked as "in operation" are currently being processed
     * by the Sync Action Thread and should not be re-processed.
     */
    bool isFileInOperation(const QString& localPath) const;

    /**
     * @brief Mark a file as being in operation
     * @param localPath Path to mark
     *
     * Called by Sync Action Thread when starting an operation.
     */
    void markFileInOperation(const QString& localPath);

    /**
     * @brief Unmark a file as being in operation
     * @param localPath Path to unmark
     *
     * Called by Sync Action Thread when operation completes.
     */
    void unmarkFileInOperation(const QString& localPath);

    /**
     * @brief Set the sync folder path
     * @param path Absolute path to the sync folder
     *
     * All localPath values in changes are relative to this folder.
     */
    void setSyncFolder(const QString& path);

    /**
     * @brief Get the sync folder path
     * @return Absolute path to the sync folder
     */
    QString syncFolder() const;

    // Configuration - public for sharing with SyncActionThread
    QString m_syncFolder;

    /**
     * @brief Clear all in-memory state (conflicts, files-in-operation)
     *
     * Called on account sign-out to prevent stale data from leaking
     * into a subsequent session.
     */
    void clearState();

   public slots:
    /**
     * @brief Start processing changes
     *
     * Begins the processing loop. Connected to Run/Pause/Stop signals.
     */
    void start();

    /**
     * @brief Stop processing completely
     *
     * Stops the processing loop and clears state.
     */
    void stop();

    /**
     * @brief Pause processing temporarily
     *
     * Suspends change processing but maintains state.
     */
    void pause();

    /**
     * @brief Resume processing after pause
     *
     * Resumes the processing loop.
     */
    void resume();

    /**
     * @brief Wake up the processor to check for new items
     *
     * Connected to Change Queue's itemsAvailable() signal.
     * This is the "Jobs Available Wakeup Signal/Slot" from the flow chart.
     */
    void onItemsAvailable();

    /**
     * @brief Resolve a conflict manually with a specific strategy
     * @param localPath Path of the conflicting file
     * @param strategy Strategy to use for resolution
     */
    void resolveConflict(const QString& localPath, ConflictResolutionStrategy strategy);

    /**
     * @brief Rehydrate unresolved conflicts from the database
     *
     * Emits conflictDetected for persisted unresolved conflicts.
     */
    void rehydrateUnresolvedConflicts();

    /**
     * @brief Slot to update sync folder when settings change
     * @param path New sync folder path
     */
    void onSyncFolderChanged(const QString& path);

   signals:
    /**
     * @brief Emitted when processor state changes
     * @param state New state
     */
    void stateChanged(ChangeProcessor::State state);

    /**
     * @brief Emitted when an error occurs
     * @param error Error message
     */
    void error(const QString& error);

    /**
     * @brief Emitted when a conflict is detected
     * @param info Conflict information
     */
    void conflictDetected(const ConflictInfo& info);

    /**
     * @brief Emitted when a conflict is resolved
     * @param localPath Path of the resolved file
     * @param strategy Strategy used for resolution
     */
    void conflictResolved(const QString& localPath, ConflictResolutionStrategy strategy);

    /**
     * @brief Emitted when a change is validated and processed
     * @param localPath Path of the processed file
     */
    void changeProcessed(const QString& localPath);

    /**
     * @brief Emitted when a change is skipped (validation failed)
     * @param localPath Path of the skipped file
     * @param reason Reason for skipping
     */
    void changeSkipped(const QString& localPath, const QString& reason);

   private slots:
    void processNextChange();

   private:
    /**
     * @brief Validate a change against database records
     * @param change The change to validate
     * @return true if change should be processed
     *
     * Implements validation step from flow chart:
     * 1. File not in operation
     * 2. File date modified > db recorded last sync time + 2 sec
     */
    bool validateChange(ChangeQueueItem& change);

    /**
     * @brief Validate that no equivalent action is already pending in SAQ
     * @param change The normalized, validated change
     * @return true if no duplicate pending action exists
     */
    bool dedupValidate(const ChangeQueueItem& change);

    /**
     * @brief Build the sync action corresponding to a change
     * @param change Source change
     * @param action Output action when build succeeds
     * @return true if action could be derived for the change
     */
    bool buildSyncActionForChange(const ChangeQueueItem& change, SyncActionItem& action) const;

    /**
     * @brief Check for conflicts between local and remote versions
     * @param change The change to check
     * @return ConflictInfo if conflict detected, otherwise empty
     *
     * Implements conflict detection from flow chart:
     * - Conflict if both local and remote changes have newer mod times
     *   than db recorded last sync mod time
     *
     * Note: May perform LONG BLOCKING NETWORK CALL to fetch remote metadata
     */
    ConflictInfo checkForConflict(ChangeQueueItem& change);

    /**
     * @brief Compute MD5 hash for a local file relative path
     * @param localPath Local path relative to sync root
     * @return Lowercase hex MD5 hash, or empty string on failure
     */
    QString computeLocalFileMd5(const QString& localPath) const;

    /**
     * @brief Resolve a detected conflict
     * @param conflict The conflict to resolve
     * @param strategy Strategy to use
     *
     * May queue multiple sync actions for resolution (e.g., KeepBoth
     * requires creating conflict copy and downloading remote).
     */
    void resolveConflictInternal(const ConflictInfo& conflict, ConflictResolutionStrategy strategy);

    /**
     * @brief Determine and queue sync actions for a validated change
     * @param change The validated change
     *
     * Implements the "Determine sync actions needed based on change type
     * and origin" step from the flow chart.
     */
    void determineAndQueueActions(const ChangeQueueItem& change);

    /**
     * @brief Store or append a conflict for manual resolution
     * @param conflict Conflict information
     */
    ConflictInfo storeManualConflict(const ConflictInfo& conflict);

    /**
     * @brief Append a new conflict version for an incoming change
     * @param change Incoming change for a conflicted path
     */
    void appendConflictVersionForChange(const ChangeQueueItem& change);

    /**
     * @brief Generate a conflict copy path
     * @param originalPath Original file path
     * @return Path for the conflict copy
     */
    QString generateConflictCopyPath(const QString& originalPath) const;

    // Core components
    ChangeQueue* m_changeQueue;
    SyncActionQueue* m_syncActionQueue;
    SyncDatabase* m_database;
    GoogleDriveClient* m_driveClient;

    // State
    State m_state;
    mutable QMutex m_stateMutex;

    // Configuration
    ConflictResolutionStrategy m_conflictStrategy;
    SyncSettings m_cachedSettings;

    // Files currently being processed by Sync Action Thread
    QSet<QString> m_filesInOperation;
    mutable QMutex m_filesInOpMutex;

    // Unresolved conflicts (for AskUser strategy)
    QHash<QString, ConflictInfo> m_unresolvedConflicts;
    mutable QMutex m_conflictsMutex;

    // Processing control
    bool m_processingActive;

    // Constants from flow chart
    static const int MIN_CHANGE_DIFF_SECS = 2;  ///< Change must be 2+ seconds newer than DB
};

#endif  // CHANGEPROCESSOR_H
