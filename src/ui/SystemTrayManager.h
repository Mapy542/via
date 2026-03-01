/**
 * @file SystemTrayManager.h
 * @brief System tray icon and menu management
 *
 * Manages the system tray icon, context menu, and quick actions.
 */

#ifndef SYSTEMTRAYMANAGER_H
#define SYSTEMTRAYMANAGER_H

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>

class GoogleAuthManager;
class SyncActionQueue;
class ChangeProcessor;

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
     * @brief Update sync status in menu
     * @param status Current sync status
     */
    void updateSyncStatus(const QString& status);

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
};

#endif  // SYSTEMTRAYMANAGER_H
