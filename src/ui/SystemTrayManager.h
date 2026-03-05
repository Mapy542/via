/**
 * @file SystemTrayManager.h
 * @brief System tray icon and menu management
 *
 * Manages the system tray icon, context menu, and quick actions.
 * Tracks per-subsystem status (mirror sync, FUSE) independently and
 * always displays the icon for the highest-severity active condition.
 */

#ifndef SYSTEMTRAYMANAGER_H
#define SYSTEMTRAYMANAGER_H

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>
#include <QTimer>

class GoogleAuthManager;
class SyncActionQueue;
class ChangeProcessor;

/**
 * @enum TrayIconPriority
 * @brief Severity levels for tray icon selection (ascending priority)
 *
 * When multiple subsystems are active the tray always shows the icon
 * for the highest-priority (most severe) condition.
 */
enum class TrayIconPriority {
    Idle = 0,             ///< Nothing happening — drive-idle.svg
    Syncing = 1,          ///< Active sync/upload/download — sync-active.svg
    Paused = 2,           ///< Sync paused — paused.svg
    LowStorage = 3,       ///< Storage >= 75% — low-storage.svg
    CriticalStorage = 4,  ///< Storage >= 90% — critical-low-storage.svg
    Offline = 5,          ///< Not connected — no-connection.svg
    Warning = 6,          ///< Conflicts detected — warn.svg
    Error = 7,            ///< Sync/FUSE error — error.svg
    AuthExpired = 8       ///< Authentication expired — auth-expired.svg
};

/**
 * @class SystemTrayManager
 * @brief Manages the system tray icon and menu
 *
 * Provides quick access to:
 * - Open Drive folder
 * - Pause/resume sync
 * - View recent changes
 * - Open main window
 * - Quit application
 *
 * Tracks per-subsystem status independently (mirror sync, FUSE) and
 * always displays the icon for the highest-severity active condition.
 */
class SystemTrayManager : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the system tray manager
     * @param authManager Pointer to the authentication manager
     * @param syncActionQueue Pointer to the sync action queue
     * @param changeProcessor Pointer to the change processor/conflict resolver
     * @param parent Parent object
     */
    explicit SystemTrayManager(GoogleAuthManager* authManager, SyncActionQueue* syncActionQueue,
                               ChangeProcessor* changeProcessor, QObject* parent = nullptr);

    ~SystemTrayManager() override;

    /**
     * @brief Show the system tray icon
     */
    void show();

    /**
     * @brief Hide the system tray icon
     */
    void hide();

    /**
     * @brief Update the tray icon tooltip
     * @param message Tooltip message
     */
    void setToolTip(const QString& message);

    /**
     * @brief Show a system notification
     * @param title Notification title
     * @param message Notification message
     * @param icon Icon type
     */
    void showNotification(const QString& title, const QString& message,
                          QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information);

   signals:
    /**
     * @brief Emitted when user requests to show the main window
     */
    void showWindowRequested();

    /**
     * @brief Emitted when user requests to quit the application
     */
    void quitRequested();

    /**
     * @brief Emitted when user clicks "Sync Now"
     */
    void fullSyncRequested();

    /**
     * @brief Emitted when user clicks a notification
     */
    void notificationClicked();

   public slots:
    /**
     * @brief Update mirror-sync subsystem status
     * @param status Current mirror sync status text
     *
     * Parses the status string to derive a TrayIconPriority for the
     * mirror subsystem, then calls resolveIcon() to pick the winner.
     */
    void updateSyncStatus(const QString& status);

    /**
     * @brief Update FUSE subsystem status
     * @param status Current FUSE status text
     *
     * Parses the status string to derive a TrayIconPriority for the
     * FUSE subsystem, then calls resolveIcon() to pick the winner.
     */
    void updateFuseStatus(const QString& status);

    /**
     * @brief Update authentication state in menu
     * @param authenticated Whether user is authenticated
     */
    void updateAuthState(bool authenticated);

    /**
     * @brief Set conflicting files state
     * @param hasConflicts Whether there are unresolved conflicts
     *
     * When true, shows warning icon and notifies user to address conflicts.
     */
    void setHasConflicts(bool hasConflicts);

    /**
     * @brief Update storage usage information and icon
     * @param storageUsed Bytes used
     * @param storageLimit Total bytes available
     */
    void updateStorageInfo(qint64 storageUsed, qint64 storageLimit);

   private slots:
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void onOpenFolderClicked();
    void onPauseSyncClicked();
    void onSyncNowClicked();
    void onRecentChangesClicked();
    void refreshStatus();

   private:
    void createMenu();
    void updatePauseAction(bool paused);

    /**
     * @brief Pick the highest-severity icon across all subsystems and apply it
     *
     * Computes effective priority = max(mirror, fuse, global), selects the
     * corresponding icon, and builds a combined status/tooltip string.
     */
    void resolveIcon();

    /**
     * @brief Map a TrayIconPriority to its SVG icon filename
     */
    static QString iconForPriority(TrayIconPriority priority);

    /**
     * @brief Parse a status string into a TrayIconPriority
     */
    static TrayIconPriority priorityFromStatusText(const QString& status);

    /**
     * @brief Recalculate m_globalPriority from storage + conflicts
     */
    void recalcGlobalPriority();

    GoogleAuthManager* m_authManager;
    SyncActionQueue* m_syncActionQueue;
    ChangeProcessor* m_changeProcessor;

    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayMenu;

    // Menu actions
    QAction* m_statusAction;
    QAction* m_openFolderAction;
    QAction* m_pauseSyncAction;
    QAction* m_syncNowAction;
    QAction* m_recentChangesAction;
    QAction* m_openWindowAction;
    QAction* m_settingsAction;
    QAction* m_quitAction;

    bool m_syncPaused;
    bool m_hasConflicts;
    double m_storagePercent;  ///< Storage usage percentage (0-100), -1 if unknown
    QTimer* m_statusTimer;    ///< Periodic status refresh timer

    // Per-subsystem priority tracking
    TrayIconPriority m_mirrorPriority = TrayIconPriority::Idle;
    TrayIconPriority m_fusePriority = TrayIconPriority::Idle;
    TrayIconPriority m_globalPriority = TrayIconPriority::Idle;  ///< auth, storage, conflicts

    // Per-subsystem human-readable status text
    QString m_mirrorStatusText;
    QString m_fuseStatusText;

    bool m_authenticated = false;  ///< Cached auth state for priority recalc
};

#endif  // SYSTEMTRAYMANAGER_H
