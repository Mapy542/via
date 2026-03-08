/**
 * @file LocalChangeWatcher.h
 * @brief Local filesystem change watcher thread
 *
 * Monitors the local sync folder for changes and feeds them into
 * the Change Queue. Implements the "Local Changes Watcher Thread"
 * from the sync procedure flow chart.
 */

#ifndef LOCALCHANGEWATCHER_H
#define LOCALCHANGEWATCHER_H

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>

#include "ChangeQueue.h"

class ChangeQueue;

/**
 * @class LocalChangeWatcher
 * @brief Watches local filesystem for changes and queues them
 *
 * This class monitors the sync folder using QFileSystemWatcher and
 * creates ChangeQueueItems for detected changes. It runs on a dedicated
 * thread and feeds changes to the ChangeQueue.
 *
 * Supports:
 * - File/folder creation detection
 * - File modification detection
 * - File/folder deletion detection
 * - Move detection (via delete + create with same content)
 * - Rename detection (via delete + create in same directory)
 * - Pause/Resume functionality
 * - Configurable ignore patterns
 */
class LocalChangeWatcher : public QObject {
    Q_OBJECT

   public:
    /**
     * @enum State
     * @brief Operating state of the watcher
     */
    enum class State { Stopped, Running, Paused };

    /**
     * @brief Construct the local change watcher
     * @param changeQueue Pointer to the change queue to feed
     * @param parent Parent object
     */
    explicit LocalChangeWatcher(ChangeQueue* changeQueue, QObject* parent = nullptr);

    ~LocalChangeWatcher() override;

    /**
     * @brief Set the sync folder to watch
     * @param path Absolute path to the sync folder
     */
    void setSyncFolder(const QString& path);

    /**
     * @brief Get the current sync folder path
     * @return Path to the sync folder
     */
    QString syncFolder() const;

    /**
     * @brief Set file patterns to ignore
     * @param patterns List of glob patterns (e.g., "*.tmp", ".git...")
     */
    void setIgnorePatterns(const QStringList& patterns);

    /**
     * @brief Get current ignore patterns
     * @return List of ignore patterns
     */
    QStringList ignorePatterns() const;

    /**
     * @brief Get current state
     * @return Current operating state
     */
    State state() const;

   public slots:
    /**
     * @brief Start watching the sync folder
     *
     * Initializes the file watcher and begins monitoring.
     * Called via signal from the Run/Pause/Stop control signals.
     */
    void start();

    /**
     * @brief Stop watching completely
     *
     * Stops all monitoring and clears tracked state.
     */
    void stop();

    /**
     * @brief Pause watching temporarily
     *
     * Suspends change detection but maintains tracked state.
     * Changes that occur while paused are detected on resume.
     */
    void pause();

    /**
     * @brief Resume watching after pause
     *
     * Rescans the directory to detect changes made while paused.
     */
    void resume();

    /**
     * @brief Add a directory to watch list
     * @param path Absolute path to the directory to watch
     *
     * This method adds a directory and all its subdirectories to the watch list.
     * Should be called when new directories are created by sync operations.
     */
    void watchDirectory(const QString& path);

    /**
     * @brief Remove a directory from watch list
     * @param path Absolute path to the directory to stop watching
     *
     * This method removes a directory and all its subdirectories from the watch list.
     * Should be called when directories are deleted by sync operations.
     */
    void unwatchDirectory(const QString& path);

   signals:
    /**
     * @brief Emitted when watcher state changes
     * @param state New state
     */
    void stateChanged(LocalChangeWatcher::State state);

    /**
     * @brief Emitted when an error occurs
     * @param error Error message
     */
    void error(const QString& error);

    /**
     * @brief Emitted when a local change is detected
     * @param path Path to the changed file
     *
     * For debugging/logging purposes. The change is also
     * automatically queued to the ChangeQueue.
     */
    void changeDetected(const QString& path);

   private slots:
    void onDirectoryChanged(const QString& path);
    void onFileChanged(const QString& path);
    void processDebounceQueue();

   private:
    void addDirectoryRecursive(const QString& path);
    void removeDirectoryRecursive(const QString& path);
    void scanDirectory(const QString& path);
    bool shouldIgnore(const QString& path) const;
    QString getRelativePath(const QString& absolutePath) const;
    void queueChange(ChangeType type, const QString& absolutePath,
                     const QString& oldPath = QString());
    void queueDelete(const QString& absolutePath, bool isDirectory,
                     const QDateTime& modifiedTime = QDateTime());
    void detectMoveOrRename(const QString& deletedPath, const QString& createdPath);

    /**
     * @brief Check if a path is within a directory
     * @param path Path to check
     * @param directory Directory to check against
     * @return true if path is within directory (exact match or subdirectory)
     */
    static bool isPathWithinDirectory(const QString& path, const QString& directory);

    ChangeQueue* m_changeQueue;
    QFileSystemWatcher* m_watcher;
    QTimer* m_debounceTimer;

    QString m_syncFolder;
    QStringList m_ignorePatterns;
    QList<QRegularExpression> m_compiledIgnorePatterns;
    State m_state;
    mutable QRecursiveMutex m_mutex;

    // Track file state for change detection
    QHash<QString, QFileInfo> m_fileState;
    QSet<QString> m_watchedDirs;

    // For move/rename detection
    struct PendingChange {
        QString path;
        bool isDelete;
        bool isDirectory;
        qint64 size;
        QDateTime modTime;
        QDateTime detectedAt;
    };
    QList<PendingChange> m_pendingChanges;

    // Debounce settings
    static const int DEBOUNCE_DELAY_MS = 500;
    static const int MOVE_DETECTION_WINDOW_MS = 1000;
};

#endif  // LOCALCHANGEWATCHER_H
