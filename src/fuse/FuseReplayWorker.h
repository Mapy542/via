/**
 * @file FuseReplayWorker.h
 * @brief Dedicated background worker for replaying durable FUSE journal operations.
 */

#ifndef FUSEREPLAYWORKER_H
#define FUSEREPLAYWORKER_H

#include <QMutex>
#include <QObject>
#include <QTimer>
#include <QWaitCondition>
#include <functional>
#include <optional>

#include "api/DriveFile.h"
#include "sync/SyncDatabase.h"
#include "sync/SyncSettings.h"

class GoogleDriveClient;

enum class FuseReplayWorkerState { Stopped, Running, Paused, Replaying };
Q_DECLARE_METATYPE(FuseReplayWorkerState)

class FuseReplayWorker : public QObject {
    Q_OBJECT

   public:
    explicit FuseReplayWorker(SyncDatabase* database, GoogleDriveClient* driveClient,
                              QObject* parent = nullptr);
    ~FuseReplayWorker() override;

    int syncIntervalMs() const;
    void setSyncIntervalMs(int ms);
    FuseReplayWorkerState state() const;

   public slots:
    void start();
    void stop();
    void pause();
    void resume();
    void syncNow();
    void reloadSettings();

   signals:
    void stateChanged(FuseReplayWorkerState state);
    void replayCycleCompleted(int completedCount, int deferredOrFailedCount);
    void replayEntryCompleted(qint64 entryId);
    void replayEntryBlocked(qint64 entryId, const QString& error);

   private slots:
    void onSyncTimerTimeout();
    void onFileUploadedDetailed(const DriveFile& file, const QString& localPath);
    void onFileUpdated(const DriveFile& file);
    void onFolderCreatedDetailed(const DriveFile& folder, const QString& localPath);
    void onFileMovedDetailed(const DriveFile& file);
    void onFileMovedAndRenamedDetailed(const DriveFile& file);
    void onFileRenamedDetailed(const DriveFile& file);
    void onFileDeleted(const QString& fileId);
    void onFileTrashed(const QString& fileId);
    void onDriveErrorDetailed(const QString& operation, const QString& errorMsg, int httpStatus,
                              const QString& fileId, const QString& localPath);

   private:
    enum class EntryProcessResult { Deferred, Completed, Failed };

    void processPendingEntries();
    EntryProcessResult processEntry(const FuseJournalEntry& entry);
    EntryProcessResult replayCreateFile(const FuseJournalEntry& entry, FuseNode node);
    EntryProcessResult replayCreateDirectory(const FuseJournalEntry& entry, FuseNode node);
    EntryProcessResult replayRenameOrMove(const FuseJournalEntry& entry, FuseNode node);
    EntryProcessResult replayTrashOrDelete(const FuseJournalEntry& entry, const FuseNode& node);
    EntryProcessResult replayWriteOrTruncate(const FuseJournalEntry& entry, const FuseNode& node);
    EntryProcessResult completeLocalOnlyEntry(const FuseJournalEntry& entry, const FuseNode& node,
                                              const QString& reason = QString());
    EntryProcessResult handleConflict(const FuseJournalEntry& entry, const QString& error,
                                      int httpStatus);

    bool waitForRemoteCompletion(const QString& operation, const QString& remoteFileId,
                                 const QString& localPath, const std::function<void()>& startCall,
                                 DriveFile* completedFile, QString* errorOut,
                                 int* httpStatusOut = nullptr);
    bool resolveRemoteParentId(const QString& parentNodeId, const QString& fallbackRemoteParentId,
                               QString* remoteParentIdOut) const;
    FuseNode findNodeForEntry(const FuseJournalEntry& entry) const;
    static std::optional<RemoteMutationType> remoteMutationTypeForEntry(
        FuseJournalOperationType operationType);
    static QString relativeFusePath(const QString& fusePath);
    static bool isNotFoundError(const QString& error, int httpStatus);
    static bool isConflictError(const QString& error, int httpStatus);
    static bool isTransientReplayError(const QString& error, int httpStatus);
    static FuseMetadata fuseMetadataFromNode(const FuseNode& node);
    void setState(FuseReplayWorkerState newState);

    SyncDatabase* m_database = nullptr;
    GoogleDriveClient* m_driveClient = nullptr;
    QTimer* m_syncTimer = nullptr;
    mutable QMutex m_mutex;
    QWaitCondition m_remoteCondition;
    FuseReplayWorkerState m_state = FuseReplayWorkerState::Stopped;
    int m_syncIntervalMs = 5000;
    int m_remoteTimeoutMs = 30000;
    bool m_remoteInFlight = false;
    bool m_remoteDone = false;
    bool m_remoteSuccess = false;
    QString m_currentOperation;
    QString m_currentRemoteFileId;
    QString m_currentLocalPath;
    DriveFile m_lastCompletedFile;
    QString m_lastError;
    int m_lastHttpStatus = 0;
    SyncSettings m_syncSettings;
};

#endif  // FUSEREPLAYWORKER_H