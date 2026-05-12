/**
 * @file MirrorSyncRuntime.h
 * @brief Mirror-sync runtime facade for main-thread orchestration
 */

#ifndef MIRRORSYNCRUNTIME_H
#define MIRRORSYNCRUNTIME_H

#include <QObject>

#include "ChangeProcessor.h"
#include "FullSync.h"
#include "SyncActionQueue.h"

class GoogleDriveClient;
class LocalChangeWatcher;
class MirrorSyncController;
class RemoteChangeWatcher;
class SyncDatabase;
class SyncActionThread;
class QThread;

/**
 * @class MirrorSyncRuntime
 * @brief Single control surface for mirror-sync lifecycle and setup
 *
 * This wraps the current MirrorSyncController and the mirror components behind
 * one QObject so main.cpp can target a single runtime object. The runtime is
 * intentionally slot-based so main-thread callers can cross the mirror worker
 * boundary without depending on worker-owned objects directly.
 */
class MirrorSyncRuntime : public QObject {
    Q_OBJECT

   public:
    explicit MirrorSyncRuntime(LocalChangeWatcher* localWatcher, RemoteChangeWatcher* remoteWatcher,
                               ChangeProcessor* changeProcessor, SyncActionQueue* syncActionQueue,
                               SyncActionThread* syncActionThread, FullSync* fullSync,
                               GoogleDriveClient* mirrorDriveClient = nullptr,
                               SyncDatabase* syncDatabase = nullptr, QObject* parent = nullptr);

    ~MirrorSyncRuntime() override;

    int pendingActionCount() const;
    ChangeProcessor::State processorState() const;
    QList<ConflictInfo> unresolvedConflicts() const;
    int unresolvedConflictCount() const;
    QString lastSyncActionError() const;
    FullSync::State fullSyncState() const;
    QString fullSyncPhase() const;
    int fullSyncProgressCurrent() const;
    int fullSyncProgressTotal() const;
    int lastFullSyncLocalCount() const;
    int lastFullSyncRemoteCount() const;
    QString lastFullSyncError() const;

    void shutdown();

   signals:
    void pendingActionsChanged(int count);
    void processorStateChanged(ChangeProcessor::State state);
    void processorError(const QString& error);
    void conflictDetected(const ConflictInfo& info);
    void conflictResolved(const QString& localPath, ConflictResolutionStrategy strategy);
    void changeProcessed(const QString& localPath);
    void syncActionQueued(const SyncActionItem& item);
    void syncActionCompleted(const SyncActionItem& item);
    void syncActionFailed(const SyncActionItem& item, const QString& error);
    void syncActionProgress(const SyncActionItem& item, qint64 bytesProcessed, qint64 bytesTotal);
    void syncActionError(const QString& error);
    void fullSyncStateChanged(FullSync::State state);
    void fullSyncProgressUpdated(const QString& phase, int current, int total);
    void fullSyncCompleted(int localCount, int remoteCount);
    void fullSyncError(const QString& error);
    void changeTokenUpdated(const QString& token);
    void tokenRefreshRequested();
    void authenticationFailure(const QString& operation, int httpStatus, const QString& errorMsg);

   public slots:
    void setSyncFolder(const QString& syncFolder);
    void setChangeToken(const QString& token);
    void setConflictResolutionStrategy(ConflictResolutionStrategy strategy);
    void setPeriodicLocalFullSyncInterval(int intervalMs);
    void reloadSyncSettings();
    void resolveConflict(const QString& localPath, ConflictResolutionStrategy strategy);

    void start();
    void stop();
    void pause();
    void resume();
    void cancelAndStop();
    void startAndScheduleInitialSync(int delayMs = 500);
    void restartAfterWake(int fullSyncDelayMs = 2000);
    void requestFullSync(int delayMs = 0);
    void clearSessionState();

   private:
    void moveMirrorGraphToWorker();
    void restoreMirrorGraphToOwnerThread();

    LocalChangeWatcher* m_localWatcher;
    RemoteChangeWatcher* m_remoteWatcher;
    ChangeProcessor* m_changeProcessor;
    SyncActionQueue* m_syncActionQueue;
    SyncActionThread* m_syncActionThread;
    FullSync* m_fullSync;
    GoogleDriveClient* m_mirrorDriveClient;
    SyncDatabase* m_syncDatabase;
    MirrorSyncController* m_controller;
    QThread* m_workerThread;
    QThread* m_ownerThread;

    int m_pendingActionCount = 0;
    ChangeProcessor::State m_processorState = ChangeProcessor::State::Stopped;
    QList<ConflictInfo> m_unresolvedConflicts;

    QString m_lastSyncActionError;
    FullSync::State m_fullSyncState = FullSync::State::Idle;
    QString m_fullSyncPhase;
    int m_fullSyncProgressCurrent = 0;
    int m_fullSyncProgressTotal = 0;
    int m_lastFullSyncLocalCount = 0;
    int m_lastFullSyncRemoteCount = 0;
    QString m_lastFullSyncError;
    bool m_shutdownComplete = false;
};

#endif  // MIRRORSYNCRUNTIME_H