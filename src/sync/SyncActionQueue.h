/**
 * @file SyncActionQueue.h
 * @brief Thread-safe queue for sync actions to be executed
 *
 * Implements the Sync Action Queue from the sync procedure flow chart.
 * Receives sync actions from the Change Processor/Conflict Resolver,
 * and feeds them to the Sync Action Thread for execution.
 */

#ifndef SYNCACTIONQUEUE_H
#define SYNCACTIONQUEUE_H

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QWaitCondition>

/**
 * @enum SyncActionType
 * @brief Types of sync actions that can be executed
 */
enum class SyncActionType {
    Upload,        ///< Upload local file to remote
    Download,      ///< Download remote file to local
    DeleteLocal,   ///< Delete file from local filesystem
    DeleteRemote,  ///< Delete file from Google Drive
    MoveLocal,     ///< Move/rename file locally
    MoveRemote,    ///< Move/rename file on Google Drive
    RenameLocal,   ///< Rename file locally (same directory)
    RenameRemote   ///< Rename file on Google Drive (same parent)
};

/**
 * @struct SyncActionItem
 * @brief Represents a single sync action to be executed
 *
 * Contains all information needed to execute a sync operation,
 * whether it's uploading, downloading, or deleting files.
 */
struct SyncActionItem {
    SyncActionType actionType;  ///< Type of action (upload, download, delete, move, rename)
    QString localPath;          ///< Path relative to sync root
    QString fileId;             ///< Google Drive file ID (may be empty for new uploads)
    QDateTime modifiedTime;     ///< File modification time for timestamp preservation
    bool isFolder = false;      ///< Whether the item is a folder (for upload/download)
    QString localContentHash;   ///< Local file content hash (MD5 hex) when available
    QString remoteMd5;          ///< Remote Drive md5Checksum when available

    // Action-specific information (populate only as relevant)
    QString moveDestination;  ///< For moves: destination path (local) or parent path (remote)
    QString renameTo;         ///< For renames: new name

    /**
     * @brief Check if the action item has required fields
     * @return true if the item has valid required data
     */
    bool isValid() const {
        // Must have an action type
        // Upload/DeleteLocal/MoveLocal/RenameLocal require localPath
        // Download/DeleteRemote/MoveRemote/RenameRemote require fileId
        switch (actionType) {
            case SyncActionType::Upload:
            case SyncActionType::DeleteLocal:
            case SyncActionType::MoveLocal:
            case SyncActionType::RenameLocal:
                return !localPath.isEmpty();
            case SyncActionType::Download:
            case SyncActionType::MoveRemote:
            case SyncActionType::RenameRemote:
                return !fileId.isEmpty();
            case SyncActionType::DeleteRemote:
                return !fileId.isEmpty() || !localPath.isEmpty();
            default:
                return false;
        }
    }

    /**
     * @brief Check equality between action items
     * @param other Other item to compare
     * @return true if items represent the same action
     */
    bool operator==(const SyncActionItem& other) const {
        return actionType == other.actionType && localPath == other.localPath &&
               fileId == other.fileId;
    }
};

Q_DECLARE_METATYPE(SyncActionItem)
Q_DECLARE_METATYPE(SyncActionType)

/**
 * @class SyncActionQueue
 * @brief Thread-safe queue for collecting sync actions
 *
 * This queue collects sync actions from the Change Processor.
 * It provides a "Jobs Available Wakeup" signal/slot mechanism to
 * notify the Sync Action Thread when new items are available.
 *
 * Threading model:
 * - enqueue() is called from the Change Processor thread
 * - dequeue() is called from the Sync Action Thread
 * - waitForItems() blocks until items are available or timeout
 */
class SyncActionQueue : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the sync action queue
     * @param parent Parent object
     */
    explicit SyncActionQueue(QObject* parent = nullptr);

    ~SyncActionQueue() override;

    /**
     * @brief Add a sync action item to the queue
     * @param item Sync action item to enqueue
     *
     * Thread-safe. Emits itemsAvailable() signal if queue was empty.
     */
    void enqueue(const SyncActionItem& item);

    /**
     * @brief Add a sync action item unless an equivalent pending action exists
     * @param item Sync action item to enqueue
     * @return true if item was enqueued, false if invalid or duplicate
     *
     * Duplicate identity is based on action type and file identity:
     * - Prefer fileId when available
     * - Fallback to normalized localPath
     *
     * Thread-safe. Emits itemsAvailable() signal if queue was empty and enqueue succeeds.
     */
    bool enqueueIfNotDuplicate(const SyncActionItem& item);

    /**
     * @brief Check whether an equivalent action is already pending
     * @param item Sync action item to check
     * @return true if queue already contains an equivalent pending action
     *
     * Thread-safe.
     */
    bool containsDuplicatePending(const SyncActionItem& item) const;

    /**
     * @brief Get and remove the next item from the queue
     * @return Next item, or invalid item if queue is empty
     *
     * Thread-safe. Does not block.
     */
    SyncActionItem dequeue();

    /**
     * @brief Peek at the next item without removing it
     * @return Next item, or invalid item if queue is empty
     *
     * Thread-safe.
     */
    SyncActionItem peek() const;

    /**
     * @brief Wait for items to become available
     * @param timeoutMs Maximum time to wait in milliseconds
     * @return true if items are available, false on timeout
     *
     * Thread-safe. Optional for consumers that want to block for work.
     */
    bool waitForItems(int timeoutMs = 30000);

    /**
     * @brief Check if the queue is empty
     * @return true if queue has no items
     *
     * Thread-safe.
     */
    bool isEmpty() const;

    /**
     * @brief Get the number of items in the queue
     * @return Number of items
     *
     * Thread-safe.
     */
    int count() const;

    /**
     * @brief Clear all items from the queue
     *
     * Thread-safe.
     */
    void clear();

    /**
     * @brief Remove items matching a specific path
     * @param localPath Path to match
     * @return Number of items removed
     *
     * Thread-safe.
     */
    int removeByPath(const QString& localPath);

    /**
     * @brief Remove items matching a specific file ID
     * @param fileId File ID to match
     * @return Number of items removed
     *
     * Thread-safe.
     */
    int removeByFileId(const QString& fileId);

    /**
     * @brief Wake up any thread waiting on waitForItems()
     *
     * Used when stopping the action processor.
     */
    void wakeAll();

   signals:
    /**
     * @brief Emitted when items become available in an empty queue
     *
     * This is the "Jobs Available Wakeup Signal" from the flow chart.
     * Connected to the Sync Action Thread's wakeup slot.
     */
    void itemsAvailable();

    /**
     * @brief Emitted when queue becomes empty
     */
    void queueEmpty();

    /**
     * @brief Emitted when an item is enqueued
     * @param item The enqueued item
     */
    void itemEnqueued(const SyncActionItem& item);

   private:
    QQueue<SyncActionItem> m_queue;
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
};

#endif  // SYNCACTIONQUEUE_H
