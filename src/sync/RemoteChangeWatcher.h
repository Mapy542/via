/**
 * @file RemoteChangeWatcher.h
 * @brief Remote Google Drive change watcher thread
 *
 * Monitors Google Drive for remote changes and feeds them into
 * the Change Queue. Implements the "Remote Changes Watcher Thread"
 * from the sync procedure flow chart.
 */

#ifndef REMOTECHANGEWATCHER_H
#define REMOTECHANGEWATCHER_H

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>

#include "SyncSettings.h"

class ChangeQueue;
class GoogleDriveClient;
class SyncDatabase;

// Forward declarations for Drive types
struct DriveChange;
struct DriveFile;

/**
 * @class RemoteChangeWatcher
 * @brief Watches Google Drive for changes and queues them
 *
 * This class polls the Google Drive Changes API and creates
 * ChangeQueueItems for detected changes. It feeds changes to
 * the ChangeQueue for processing.
 *
 * Supports:
 * - Periodic polling for remote changes
 * - Change token management (startPageToken)
 * - Filtering (ownedByMe files only)
 * - Pause/Resume functionality
 */
class RemoteChangeWatcher : public QObject {
    Q_OBJECT

   public:
    /**
     * @enum State
     * @brief Operating state of the watcher
     */
    enum class State { Stopped, Running, Paused };

    /**
     * @brief Construct the remote change watcher
     * @param changeQueue Pointer to the change queue to feed
     * @param driveClient Pointer to the Google Drive client
     * @param syncDatabase Pointer to the sync database for persistence
     * @param parent Parent object
     */
    explicit RemoteChangeWatcher(ChangeQueue* changeQueue, GoogleDriveClient* driveClient,
                                 SyncDatabase* syncDatabase, QObject* parent = nullptr);

    ~RemoteChangeWatcher() override;

    /**
     * @brief Set the polling interval
     * @param intervalMs Interval in milliseconds (default: 30000 = 30 seconds)
     */
    void setPollingInterval(int intervalMs);

    /**
     * @brief Get the polling interval
     * @return Interval in milliseconds
     */
    int pollingInterval() const;

    /**
     * @brief Set the change page token
     * @param token Token from previous changes.list call
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

    /**
     * @brief Set the folder ID to path mapping
     * @param mapping Map of folder IDs to relative paths
     *
     * Used to resolve file paths from remote changes.
     */
    void setFolderIdToPath(const QHash<QString, QString>& mapping);

   public slots:
    /**
     * @brief Start watching for remote changes
     *
     * Starts the polling timer and begins monitoring.
     * If no change token is set, fetches the start page token first.
     */
    void start();

    /**
     * @brief Stop watching completely
     *
     * Stops polling and clears state.
     */
    void stop();

    /**
     * @brief Pause watching temporarily
     *
     * Suspends polling but maintains state.
     */
    void pause();

    /**
     * @brief Resume watching after pause
     *
     * Resumes polling.
     */
    void resume();

    /**
     * @brief Manually trigger a check for changes
     *
     * Can be called to immediately check for changes
     * without waiting for the next poll interval.
     */
    void checkNow();

   signals:
    /**
     * @brief Emitted when watcher state changes
     * @param state New state
     */
    void stateChanged(RemoteChangeWatcher::State state);

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
     * @brief Emitted when a remote change is detected
     * @param fileId File ID of the changed file
     *
     * For debugging/logging purposes. The change is also
     * automatically queued to the ChangeQueue.
     */
    void changeDetected(const QString& fileId);

   private slots:
    void onPollingTimeout();
    void onChangesReceived(const QList<DriveChange>& changes, const QString& newToken,
                           bool hasMorePages);
    void onStartPageTokenReceived(const QString& token);
    void onApiError(const QString& operation, const QString& error);

   private:
    void processChange(const DriveChange& change);
    QString resolvePath(const DriveFile& file);
    QString resolvePathFromParents(const DriveFile& file);
    bool shouldProcess(const DriveFile& file) const;

    ChangeQueue* m_changeQueue;
    SyncDatabase* m_syncDatabase;
    GoogleDriveClient* m_driveClient;
    QTimer* m_pollingTimer;

    QString m_changeToken;
    State m_state;
    mutable QMutex m_mutex;
    bool m_waitingForToken;
    bool m_changesRequestInFlight = false;
    bool m_pendingCheckRequested = false;

    // Folder ID to path mapping for resolving file paths
    mutable QHash<QString, QString> m_folderIdToPath;

    // Track recently processed file IDs to avoid re-queueing
    mutable QHash<QString, QDateTime> m_recentlyProcessedFileIds;
    static const int DEDUP_WINDOW_SECS = 60;  // Skip re-queueing same file within 60 seconds

    SyncSettings m_settings;

    // Default polling interval (30 seconds as per flow chart)
    static const int DEFAULT_POLL_INTERVAL_MS = 30000;
};

#endif  // REMOTECHANGEWATCHER_H
