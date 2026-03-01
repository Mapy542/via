/**
 * @file ChangeQueue.h
 * @brief Thread-safe queue for file changes from local and remote sources
 *
 * Implements the Change Queue from the sync procedure flow chart.
 * Receives changes from Local Changes Watcher and Remote Changes Watcher,
 * and feeds them to the Change Processor/Conflict Resolver.
 */

#ifndef CHANGEQUEUE_H
#define CHANGEQUEUE_H

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QWaitCondition>

/**
 * @enum ChangeType
 * @brief Types of file changes that can occur
 */
enum class ChangeType {
    Create,  ///< New file or folder created
    Modify,  ///< Existing file content modified
    Delete,  ///< File or folder deleted
    Move,    ///< File or folder moved to different location
    Rename   ///< File or folder renamed (same location, different name)
};

/**
 * @enum ChangeOrigin
 * @brief Source of the change
 */
enum class ChangeOrigin {
    Local,  ///< Change originated from local filesystem
    Remote  ///< Change originated from Google Drive
};

/**
 * @struct ChangeQueueItem
 * @brief Represents a single change event in the queue
 *
 * Contains all information needed to process a file change,
 * whether it originated locally or remotely.
 */
struct ChangeQueueItem {
    ChangeType changeType;     ///< Type of change (create, modify, delete, move, rename)
    ChangeOrigin origin;       ///< Source of change (local or remote)
    QString localPath;         ///< Path relative to sync root
    QString fileId;            ///< Google Drive file ID (may be empty for new local files)
    QDateTime detectedTime;    ///< When the change was detected
    QDateTime modifiedTime;    ///< File modification time
    bool isDirectory = false;  ///< Whether this is a directory
    QString localContentHash;  ///< Local file content hash (MD5 hex) when available
    QString remoteMd5;         ///< Remote Drive md5Checksum when available

    // Change-specific information (populate only as relevant)
    QString moveDestination;  ///< For moves: destination path relative to sync root
    QString renameTo;         ///< For renames: new name

    /**
     * @brief Check if the change item has required fields
     * @return true if the item has valid required data
     */
    bool isValid() const {
        // Must have a change type
        // Must have either localPath (for local) or fileId (for remote)
        if (origin == ChangeOrigin::Local) {
            return !localPath.isEmpty();
        } else {
            return !fileId.isEmpty() || !localPath.isEmpty();
        }
    }

    /**
     * @brief Check equality between change items
     * @param other Other item to compare
     * @return true if items represent the same change
     */
    bool operator==(const ChangeQueueItem& other) const {
        return changeType == other.changeType && origin == other.origin &&
               localPath == other.localPath && fileId == other.fileId;
    }
};

Q_DECLARE_METATYPE(ChangeQueueItem)
Q_DECLARE_METATYPE(ChangeType)
Q_DECLARE_METATYPE(ChangeOrigin)

/**
 * @class ChangeQueue
 * @brief Thread-safe queue for collecting file changes
 *
 * This queue collects changes from both local and remote watchers.
 * It provides a "Jobs Available Wakeup" signal/slot mechanism to
 * notify the Change Processor when new items are available.
 *
 * Threading model:
 * - enqueue() can be called from any thread (Local/Remote watcher threads)
 * - dequeue() is called from the Change Processor thread
 * - waitForItems() blocks until items are available or timeout
 */
class ChangeQueue : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the change queue
     * @param parent Parent object
     */
    explicit ChangeQueue(QObject* parent = nullptr);

    ~ChangeQueue() override;

    /**
     * @brief Add a change item to the queue
     * @param item Change item to enqueue
     *
     * Thread-safe. Emits itemsAvailable() signal if queue was empty.
     */
    void enqueue(const ChangeQueueItem& item);

    /**
     * @brief Get and remove the next item from the queue
     * @return Next item, or invalid item if queue is empty
     *
     * Thread-safe. Does not block.
     */
    ChangeQueueItem dequeue();

    /**
     * @brief Peek at the next item without removing it
     * @return Next item, or invalid item if queue is empty
     *
     * Thread-safe.
     */
    ChangeQueueItem peek() const;

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
     * Used when stopping the queue processor.
     */
    void wakeAll();

   signals:
    /**
     * @brief Emitted when items become available in an empty queue
     *
     * This is the "Jobs Available Wakeup Signal" from the flow chart.
     * Connected to the Change Processor's wakeup slot.
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
    void itemEnqueued(const ChangeQueueItem& item);

   private:
    QQueue<ChangeQueueItem> m_queue;
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
};

#endif  // CHANGEQUEUE_H
