/**
 * @file NotificationManager.cpp
 * @brief Implementation of desktop notification manager
 */

#include "NotificationManager.h"

#include <QDBusConnection>
#include <QDBusReply>
#include <QDebug>
#include <QSettings>

NotificationManager::NotificationManager(QObject* parent)
    : QObject(parent),
      m_enabled(true),
      m_trayIcon(nullptr),
      m_dbusInterface(nullptr),
      m_useDBus(false) {
    // Load settings
    QSettings settings;
    m_enabled = settings.value("advanced/showNotifications", true).toBool();

    // Try to connect to D-Bus notifications
    m_dbusInterface =
        new QDBusInterface("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", QDBusConnection::sessionBus(), this);

    m_useDBus = m_dbusInterface->isValid();

    if (m_useDBus) {
        qDebug() << "Using D-Bus notifications";

        // Connect to notification signals
        QDBusConnection::sessionBus().connect("org.freedesktop.Notifications",
                                              "/org/freedesktop/Notifications",
                                              "org.freedesktop.Notifications", "NotificationClosed",
                                              this, SLOT(onNotificationClosed(uint, uint)));

        QDBusConnection::sessionBus().connect("org.freedesktop.Notifications",
                                              "/org/freedesktop/Notifications",
                                              "org.freedesktop.Notifications", "ActionInvoked",
                                              this, SLOT(onActionInvoked(uint, QString)));
    } else {
        qDebug() << "D-Bus notifications not available, using system tray";
    }
}

NotificationManager::~NotificationManager() = default;

bool NotificationManager::isEnabled() const { return m_enabled; }

void NotificationManager::setEnabled(bool enabled) {
    m_enabled = enabled;

    QSettings settings;
    settings.setValue("advanced/showNotifications", enabled);
}

void NotificationManager::setTrayIcon(QSystemTrayIcon* trayIcon) { m_trayIcon = trayIcon; }

void NotificationManager::showNotification(const QString& title, const QString& message,
                                           Urgency urgency, int timeoutMs) {
    if (!m_enabled) {
        return;
    }

    if (m_useDBus) {
        QString icon;
        switch (urgency) {
            case Low:
                icon = "dialog-information";
                break;
            case Normal:
                icon = "dialog-information";
                break;
            case Critical:
                icon = "dialog-error";
                break;
        }

        sendDBusNotification(title, message, icon, timeoutMs);
    } else if (m_trayIcon) {
        QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information;
        switch (urgency) {
            case Low:
            case Normal:
                icon = QSystemTrayIcon::Information;
                break;
            case Critical:
                icon = QSystemTrayIcon::Critical;
                break;
            default:
                icon = QSystemTrayIcon::Information;
                break;
        }

        sendTrayNotification(title, message, icon, timeoutMs);
    }
}

void NotificationManager::showInfo(const QString& title, const QString& message) {
    showNotification(title, message, Normal, 5000);
}

void NotificationManager::showWarning(const QString& title, const QString& message) {
    showNotification(title, message, Normal, 7000);
}

void NotificationManager::showError(const QString& title, const QString& message) {
    showNotification(title, message, Critical, 10000);
}

void NotificationManager::showFileSynced(const QString& fileName, bool uploaded) {
    QString action = uploaded ? "uploaded to" : "downloaded from";
    showNotification("File Synced", QString("%1 has been %2 Google Drive.").arg(fileName, action),
                     Low, 3000);
}

void NotificationManager::showConflict(const QString& fileName) {
    showNotification(
        "Sync Conflict",
        QString("A conflict was detected for %1. A copy has been created.").arg(fileName), Normal,
        10000);
}

bool NotificationManager::sendDBusNotification(const QString& title, const QString& message,
                                               const QString& icon, int timeout) {
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        return false;
    }

    QVariantMap hints;
    hints["urgency"] = 1;  // Normal urgency
    hints["category"] = "transfer";

    QList<QVariant> args;
    args << "Via"          // app_name
         << uint(0)        // replaces_id
         << icon           // app_icon
         << title          // summary
         << message        // body
         << QStringList()  // actions
         << hints          // hints
         << timeout;       // expire_timeout

    QDBusReply<uint> reply =
        m_dbusInterface->callWithArgumentList(QDBus::AutoDetect, "Notify", args);

    if (reply.isValid()) {
        return true;
    } else {
        qWarning() << "D-Bus notification failed:" << reply.error().message();
        return false;
    }
}

void NotificationManager::sendTrayNotification(const QString& title, const QString& message,
                                               QSystemTrayIcon::MessageIcon icon, int timeout) {
    if (m_trayIcon && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(title, message, icon, timeout);
    }
}

void NotificationManager::onNotificationClosed(uint id, uint reason) {
    Q_UNUSED(reason)
    emit notificationClosed(id);
}

void NotificationManager::onActionInvoked(uint id, const QString& actionKey) {
    Q_UNUSED(actionKey)
    emit notificationClicked(id);
}
