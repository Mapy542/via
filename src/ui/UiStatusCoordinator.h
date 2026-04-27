/**
 * @file UiStatusCoordinator.h
 * @brief Shared status coordination for tray and main window.
 */

#ifndef UISTATUSCOORDINATOR_H
#define UISTATUSCOORDINATOR_H

#include <QObject>
#include <QString>

class QTimer;

class GoogleAuthManager;
class SyncActionQueue;
class ChangeProcessor;
class RuntimePauseController;

enum class UiStatusPriority {
    Idle = 0,
    Syncing = 1,
    Paused = 2,
    LowStorage = 3,
    CriticalStorage = 4,
    Offline = 5,
    Warning = 6,
    Error = 7,
    AuthExpired = 8
};

struct UiStatusSnapshot {
    UiStatusPriority resolvedPriority = UiStatusPriority::Idle;
    QString resolvedStatusText;
    QString combinedStatusText;
    QString mirrorStatusText;
    QString fuseStatusText;
    int pendingActions = 0;
    bool authenticated = false;
    bool authExpired = false;
};

class UiStatusCoordinator : public QObject {
    Q_OBJECT

   public:
    explicit UiStatusCoordinator(GoogleAuthManager* authManager, SyncActionQueue* syncActionQueue,
                                 ChangeProcessor* changeProcessor,
                                 RuntimePauseController* pauseController = nullptr,
                                 QObject* parent = nullptr);

    UiStatusSnapshot snapshot() const;

    static UiStatusPriority priorityFromStatusText(const QString& status);
    static QString iconForPriority(UiStatusPriority priority);

   signals:
    void statusChanged();

   public slots:
    void updateMirrorStatus(const QString& status);
    void updateFuseStatus(const QString& status);
    void updateAuthState(bool authenticated);
    void setAuthExpired(const QString& reason = QString());
    void setHasConflicts(bool hasConflicts);
    void updateStorageInfo(qint64 storageUsed, qint64 storageLimit);
    void onDownloadStarted(const QString& fileId);
    void onDownloadFinished(const QString& fileId);
    void onUploadStarted(const QString& fileId, const QString& path);
    void onUploadFinished(const QString& fileId, const QString& path);
    void onDirtyFilesFlushed(int count);
    void onMetadataRefreshStarted();
    void onMetadataRefreshFinished();
    void onMetadataRefreshFailed(const QString& error);
    void refreshMirrorStatus();

   private:
    void emitIfChanged(const UiStatusSnapshot& before);
    void recalcGlobalPriority();
    void refreshMirrorStatusInternal();
    void setMirrorStatusInternal(const QString& status);
    void setFuseStatusInternal(const QString& status);
    void clearFuseActivityState();
    UiStatusPriority effectivePriority() const;
    QString combinedStatusText() const;
    QString effectiveStatusText() const;

    GoogleAuthManager* m_authManager;
    SyncActionQueue* m_syncActionQueue;
    ChangeProcessor* m_changeProcessor;
    RuntimePauseController* m_pauseController;

    QTimer* m_statusTimer;
    QTimer* m_fuseIdleTimer;

    int m_pendingActions = 0;
    int m_fuseActiveOps = 0;
    bool m_metadataRefreshActive = false;

    bool m_authenticated = false;
    bool m_authStateExplicit = false;
    bool m_authExpired = false;
    bool m_hasConflicts = false;
    double m_storagePercent = -1.0;

    QString m_authExpiredReason;
    QString m_mirrorStatusText;
    QString m_mirrorOverrideStatus;
    QString m_fuseStatusText;

    UiStatusPriority m_mirrorPriority = UiStatusPriority::Idle;
    UiStatusPriority m_fusePriority = UiStatusPriority::Idle;
    UiStatusPriority m_globalPriority = UiStatusPriority::Idle;
};

#endif  // UISTATUSCOORDINATOR_H