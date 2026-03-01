/**
 * @file NotificationManager.h
 * @brief Desktop notification manager
 */

#ifndef NOTIFICATIONMANAGER_H
#define NOTIFICATIONMANAGER_H

#include <QDBusInterface>
#include <QObject>
#include <QSystemTrayIcon>

/**
 * @class NotificationManager
 * @brief Manages desktop notifications
 *
 * Sends notifications using either:
 * - QSystemTrayIcon (fallback)
 * - D-Bus notifications (Linux)
 */
class NotificationManager : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Notification urgency level
     */
    enum Urgency { Low, Normal, Critical };
    Q_ENUM(Urgency)

    /**
     * @brief Construct the notification manager
     * @param parent Parent object
     */
    explicit NotificationManager(QObject* parent = nullptr);

    ~NotificationManager() override;

    /**
     * @brief Check if notifications are enabled
     * @return true if notifications are enabled
     */
    bool isEnabled() const;

    /**
     * @brief Enable or disable notifications
     * @param enabled Whether notifications are enabled
     */
    void setEnabled(bool enabled);

    /**
     * @brief Set the system tray icon for fallback notifications
     * @param trayIcon Pointer to system tray icon
     */
    void setTrayIcon(QSystemTrayIcon* trayIcon);

   public slots:
    /**
     * @brief Show a notification
     * @param title Notification title
     * @param message Notification message
     * @param urgency Urgency level
     * @param timeoutMs Display duration in milliseconds (0 = no timeout)
     */
    void showNotification(const QString& title, const QString& message, Urgency urgency = Normal,
                          int timeoutMs = 5000);

    /**
     * @brief Show an info notification
     * @param title Notification title
     * @param message Notification message
     */
    void showInfo(const QString& title, const QString& message);

    /**
     * @brief Show a warning notification
     * @param title Notification title
     * @param message Notification message
     */
    void showWarning(const QString& title, const QString& message);

    /**
     * @brief Show an error notification
     * @param title Notification title
     * @param message Notification message
     */
    void showError(const QString& title, const QString& message);

    /**
     * @brief Show file sync completion notification
     * @param fileName Name of the synced file
     * @param uploaded true if uploaded, false if downloaded
     */
    void showFileSynced(const QString& fileName, bool uploaded);

    /**
     * @brief Show conflict notification
     * @param fileName Name of the conflicting file
     */
    void showConflict(const QString& fileName);

   signals:
    /**
     * @brief Emitted when a notification is clicked
     * @param id Notification ID
     */
    void notificationClicked(uint id);

    /**
     * @brief Emitted when a notification is closed
     * @param id Notification ID
     */
    void notificationClosed(uint id);

   private slots:
    void onNotificationClosed(uint id, uint reason);
    void onActionInvoked(uint id, const QString& actionKey);

   private:
    bool sendDBusNotification(const QString& title, const QString& message, const QString& icon,
                              int timeout);
    void sendTrayNotification(const QString& title, const QString& message,
                              QSystemTrayIcon::MessageIcon icon, int timeout);

    bool m_enabled;
    QSystemTrayIcon* m_trayIcon;
    QDBusInterface* m_dbusInterface;
    bool m_useDBus;
};

#endif  // NOTIFICATIONMANAGER_H
