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
#include <QDateTime>
#include <QList>
#include <QMenu>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

class GoogleAuthManager;
class ChangeProcessor;
class UiStatusCoordinator;

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
     * @param changeProcessor Pointer to the change processor/conflict resolver
     * @param statusCoordinator Pointer to the shared UI status coordinator
     * @param parent Parent object
     */
    explicit SystemTrayManager(GoogleAuthManager* authManager, ChangeProcessor* changeProcessor,
                               UiStatusCoordinator* statusCoordinator, QObject* parent = nullptr);

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
     * @brief Get the underlying tray icon instance
     * @return Pointer to the tray icon
     */
    QSystemTrayIcon* trayIcon() const { return m_trayIcon; }

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
     * @brief Record a notification in the tray history menu
     * @param title Notification title
     * @param message Notification message
     */
    void recordNotification(const QString& title, const QString& message);

    /**
     * @brief Update authentication state in menu
     * @param authenticated Whether user is authenticated
     */
    void updateAuthState(bool authenticated);

   private slots:
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void onOpenFolderClicked();
    void onPauseSyncClicked();
    void onSyncNowClicked();
    void onRecentChangesClicked();
    void applyStatusSnapshot();

   private:
    void createMenu();
    void updatePauseAction(bool paused);
    void refreshNotificationMenu();

    GoogleAuthManager* m_authManager;
    ChangeProcessor* m_changeProcessor;
    UiStatusCoordinator* m_statusCoordinator;

    struct NotificationEntry {
        QString title;
        QString message;
        QDateTime timestamp;
    };

    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayMenu;
    QMenu* m_notificationsMenu;

    // Menu actions
    QAction* m_statusAction;
    QAction* m_openFolderAction;
    QAction* m_pauseSyncAction;
    QAction* m_syncNowAction;
    QAction* m_recentChangesAction;
    QAction* m_noNotificationsAction;
    QAction* m_openWindowAction;
    QAction* m_settingsAction;
    QAction* m_quitAction;

    QList<NotificationEntry> m_notificationHistory;
    static constexpr int MAX_NOTIFICATION_HISTORY = 20;

    bool m_syncPaused;

    bool m_authenticated = false;
};

#endif  // SYSTEMTRAYMANAGER_H
