/**
 * @file MirrorSyncController.h
 * @brief Centralizes mirror-sync lifecycle control
 */

#ifndef MIRRORSYNCCONTROLLER_H
#define MIRRORSYNCCONTROLLER_H

#include <QHash>
#include <QObject>

class ChangeProcessor;
class FullSync;
class LocalChangeWatcher;
class RemoteChangeWatcher;
class SyncActionThread;
class QTimer;

/**
 * @class MirrorSyncController
 * @brief Owns mirror-sync lifecycle orchestration
 *
 * This class groups the mirror-sync start/stop/pause/resume/full-sync
 * operations that were previously spread across main.cpp. It is kept as a
 * QObject with slot-based control points so the mirror runtime can later move
 * behind a queued thread boundary without changing every call site again.
 */
class MirrorSyncController : public QObject {
    Q_OBJECT

   public:
    explicit MirrorSyncController(LocalChangeWatcher* localWatcher,
                                  RemoteChangeWatcher* remoteWatcher,
                                  ChangeProcessor* changeProcessor,
                                  SyncActionThread* syncActionThread, FullSync* fullSync,
                                  QObject* parent = nullptr);

    ~MirrorSyncController() override;

    void setPeriodicLocalFullSyncInterval(int intervalMs);

   public slots:
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
    static constexpr int DEFAULT_LOCAL_FULL_SYNC_INTERVAL_MS = 5 * 60 * 1000;

    void startCoreComponents(bool startRemoteWatcher);
    void seedRemoteWatcherPathAuthority(const QHash<QString, QString>& mapping);

    LocalChangeWatcher* m_localWatcher;
    RemoteChangeWatcher* m_remoteWatcher;
    ChangeProcessor* m_changeProcessor;
    SyncActionThread* m_syncActionThread;
    FullSync* m_fullSync;
    QTimer* m_fullSyncLocalTimer;
    bool m_remoteWatcherStartPending = false;
    bool m_remoteWatcherAuthorityReady = false;
};

#endif  // MIRRORSYNCCONTROLLER_H