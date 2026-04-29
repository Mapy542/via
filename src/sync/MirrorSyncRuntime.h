/**
 * @file MirrorSyncRuntime.h
 * @brief Mirror-sync runtime facade for main-thread orchestration
 */

#ifndef MIRRORSYNCRUNTIME_H
#define MIRRORSYNCRUNTIME_H

#include <QObject>

#include "ChangeProcessor.h"

class FullSync;
class LocalChangeWatcher;
class MirrorSyncController;
class RemoteChangeWatcher;
class SyncActionQueue;
class SyncActionThread;

/**
 * @class MirrorSyncRuntime
 * @brief Single control surface for mirror-sync lifecycle and setup
 *
 * This wraps the current MirrorSyncController and the mirror components behind
 * one QObject so main.cpp can target a single runtime object. The runtime is
 * intentionally slot-based to support a future queued thread boundary.
 */
class MirrorSyncRuntime : public QObject {
    Q_OBJECT

   public:
    explicit MirrorSyncRuntime(LocalChangeWatcher* localWatcher, RemoteChangeWatcher* remoteWatcher,
                               ChangeProcessor* changeProcessor, SyncActionQueue* syncActionQueue,
                               SyncActionThread* syncActionThread, FullSync* fullSync,
                               QObject* parent = nullptr);

    ~MirrorSyncRuntime() override;

   signals:
    void pendingActionsChanged(int count);
    void processorStateChanged(ChangeProcessor::State state);

   public slots:
    void setSyncFolder(const QString& syncFolder);
    void setChangeToken(const QString& token);
    void setConflictResolutionStrategy(ConflictResolutionStrategy strategy);
    void setPeriodicLocalFullSyncInterval(int intervalMs);

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
    LocalChangeWatcher* m_localWatcher;
    RemoteChangeWatcher* m_remoteWatcher;
    ChangeProcessor* m_changeProcessor;
    SyncActionQueue* m_syncActionQueue;
    SyncActionThread* m_syncActionThread;
    FullSync* m_fullSync;
    MirrorSyncController* m_controller;
};

#endif  // MIRRORSYNCRUNTIME_H