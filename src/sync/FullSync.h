/**
 * @file FullSync.h
 * @brief Full Sync system for discovering all local and remote files
 *
 * Implements the Initial Sync Signal (ISS) from the sync procedure flow chart.
 * Discovers all local and remote files and creates change queue items for all,
 * ensuring the drive is fully synchronized before change monitoring happens.
 */

#ifndef FULLSYNC_H
#define FULLSYNC_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <atomic>

#include "SyncSettings.h"

class ChangeQueue;
class SyncDatabase;
class GoogleDriveClient;
class ChangeProcessor;
struct DriveFile;

/**
 * @brief Node in file tree for tree-based remote file structure
 */
struct FileTreeNode {
    QString name;
    QString relativePath;
    bool isFolder = false;
    QDateTime modifiedTime;
    QString fileId;
    QHash<QString, FileTreeNode*> children;

    ~FileTreeNode() { qDeleteAll(children); }
};

/**
 * @class FullSync
 * @brief Discovers all files and queues them for synchronization
 *
 * This class implements the Initial Sync Signal (ISS) from the sync procedure
 * flow chart:
 *
 * ISS(Initial Sync Signal)-->FA(Find all local and remote files, create
 * change queue items for all, conflict resolver to ignore file which have
 * not changed since last sync)
 * FA-->CQ
 *
 * The Full Sync:
 * 1. Sets the ChangeProcessor to initial sync mode
 * 2. Scans all local files in the sync folder
 * 3. Fetches all remote files from Google Drive
 * 4. Creates ChangeQueueItem for each file
 * 5. The ChangeProcessor's conflict resolver will ignore files unchanged since last sync
 *
 * This ensures that:
 * - The drive is fully synchronized before change monitoring happens
 * - New installations sync all existing files
 * - The "Sync Now" button can trigger a full resync
 */
class FullSync : public QObject {
    Q_OBJECT

   public:
    /**
     * @enum State
     * @brief Operating state of the full sync
     */
    enum class State {
        Idle,            ///< Not running
        ScanningLocal,   ///< Scanning local files
        FetchingRemote,  ///< Fetching remote files from Google Drive
        Complete,        ///< Sync discovery complete
        Error            ///< An error occurred
    };

    /**
     * @brief Construct the full sync handler
     * @param changeQueue Pointer to the change queue to populate
     * @param database Pointer to the sync database
     * @param driveClient Pointer to the Google Drive API client
     * @param changeProcessor Pointer to the change processor (for initial sync mode)
     * @param parent Parent object
     */
    explicit FullSync(ChangeQueue* changeQueue, SyncDatabase* database,
                      GoogleDriveClient* driveClient, ChangeProcessor* changeProcessor,
                      QObject* parent = nullptr);

    ~FullSync() override;

    /**
     * @brief Get current state
     * @return Current operating state
     */
    State state() const;

    /**
     * @brief Set the sync folder path
     * @param path Absolute path to the sync folder
     */
    void setSyncFolder(const QString& path);

    /**
     * @brief Get the sync folder path
     * @return Absolute path to the sync folder
     */
    QString syncFolder() const;

    /**
     * @brief Get the number of local files discovered
     * @return Count of local files
     */
    int localFileCount() const;

    /**
     * @brief Check if full sync is currently running
     * @return true if scanning or fetching
     */
    bool isRunning() const;

   public slots:
    /**
     * @brief Start a full sync (local + remote)
     */
    void fullSync();

    /**
     * @brief Start a local-only full sync (local scan only)
     */
    void fullSyncLocal();

    /**
     * @brief Start the full sync process (legacy alias)
     */
    void start();

    /**
     * @brief Cancel the full sync if running
     */
    void cancel();

   signals:
    /**
     * @brief Emitted when state changes
     * @param state New state
     */
    void stateChanged(FullSync::State state);

    /**
     * @brief Emitted on progress update
     * @param phase Current phase description
     * @param current Current progress value
     * @param total Total items (0 if unknown)
     */
    void progressUpdated(const QString& phase, int current, int total);

    /**
     * @brief Emitted when full sync completes successfully
     * @param localCount Number of local files discovered
     * @param remoteCount Number of remote files discovered
     */
    void completed(int localCount, int remoteCount);

    /**
     * @brief Emitted when an error occurs
     * @param error Error message
     */
    void error(const QString& error);

   private slots:
    void onFilesListed(const QList<DriveFile>& files, const QString& nextPageToken);
    void onDriveError(const QString& operation, const QString& errorMsg);

   private:
    enum class Mode { Full, LocalOnly };

    void startInternal(Mode mode);

    /**
     * @brief Scan local files in the sync folder
     */
    void scanLocalFiles();

    /**
     * @brief Start fetching remote files from Google Drive
     */
    void fetchRemoteFiles();

    /**
     * @brief Process a batch of remote files
     * @param files List of files received from the API
     * @param nextPageToken Token for next page (empty if done)
     */
    void processRemoteFiles(const QList<DriveFile>& files, const QString& nextPageToken);

    /**
     * @brief Queue a local file to the change queue
     * @param absolutePath Absolute path to the local file
     * @param isDirectory Whether this is a directory
     */
    void queueLocalFile(const QString& absolutePath, bool isDirectory);

    /**
     * @brief Queue a remote file to the change queue
     * @param file Remote file metadata
     * @param localPath Resolved local path for the file
     */
    void queueRemoteFile(const DriveFile& file, const QString& localPath);

    /**
     * @brief Build the local folder structure for remote files
     */
    void buildRemoteFolderStructure();

    /**
     * @brief Get relative path from absolute path
     * @param absolutePath Absolute path
     * @return Path relative to sync folder
     */
    QString getRelativePath(const QString& absolutePath) const;

    /**
     * @brief Complete the full sync process
     */
    void finishSync();

    // Core components
    ChangeQueue* m_changeQueue;
    SyncDatabase* m_database;
    GoogleDriveClient* m_driveClient;
    ChangeProcessor* m_changeProcessor;

    // State
    std::atomic<State> m_state;
    Mode m_mode;
    mutable QMutex m_mutex;
    bool m_cancelled;

    // Configuration
    QString m_syncFolder;

    // Progress tracking
    int m_localFileCount;
    int m_remoteFileCount;
    int m_orphanCount;
    QSet<QString> m_discoveredLocalPaths;  ///< Set of local paths discovered to avoid duplicates

    // Remote file discovery state
    QList<DriveFile> m_allRemoteItems;  ///< All remote files discovered
    QString m_currentPageToken;         ///< Current page token for remote file listing

    // Root of remote file tree
    FileTreeNode* m_remoteTree;

    // Constants
    QString ROOT_FOLDER_ID;  ///< Google Drive root folder ID

    // Settings snapshot
    SyncSettings m_settings;

    /**
     * @brief Check if a local file should be ignored
     * @param fileName Name of the file
     * @return true if file should be skipped
     */
    bool shouldIgnoreFile(const QString& fileName) const;

    /**
     * @brief Check if a remote file should be skipped
     * @param file Remote file to check
     * @return true if file should be skipped (trashed, Google Doc, not owned)
     */
    bool shouldSkipRemoteFile(const DriveFile& file) const;
};

#endif  // FULLSYNC_H