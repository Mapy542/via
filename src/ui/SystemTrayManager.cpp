/**
 * @file SystemTrayManager.cpp
 * @brief Implementation of the system tray manager
 */

#include "SystemTrayManager.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QIcon>
#include <QLocale>
#include <QSettings>
#include <QUrl>

#include "UiStatusCoordinator.h"
#include "auth/GoogleAuthManager.h"
#include "sync/RuntimePauseController.h"
#include "utils/ThemeHelper.h"

SystemTrayManager::SystemTrayManager(GoogleAuthManager* authManager,
                                     RuntimePauseController* pauseController,
                                     UiStatusCoordinator* statusCoordinator, QObject* parent)
    : QObject(parent),
      m_authManager(authManager),
      m_pauseController(pauseController),
      m_statusCoordinator(statusCoordinator),
      m_syncPaused(false),
      m_authenticated(false) {
    // Create system tray icon
    m_trayIcon = new QSystemTrayIcon(this);

    // Set the default idle icon with theme awareness
    m_trayIcon->setIcon(ThemeHelper::trayIcon("drive-idle.svg"));
    m_trayIcon->setToolTip("Via");

    // Create context menu
    createMenu();

    // Connect signals
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &SystemTrayManager::onTrayIconActivated);

    connect(m_trayIcon, &QSystemTrayIcon::messageClicked, this,
            &SystemTrayManager::notificationClicked);

    // Connect to auth manager
    if (m_authManager) {
        connect(m_authManager, &GoogleAuthManager::authenticated, this,
                [this]() { updateAuthState(true); });
        connect(m_authManager, &GoogleAuthManager::loggedOut, this,
                [this]() { updateAuthState(false); });
    }

    if (m_statusCoordinator) {
        connect(m_statusCoordinator, &UiStatusCoordinator::statusChanged, this,
                &SystemTrayManager::applyStatusSnapshot);
        applyStatusSnapshot();
    }

    if (m_pauseController) {
        connect(m_pauseController, &RuntimePauseController::stateChanged, this,
                &SystemTrayManager::applyPauseControllerState);
        applyPauseControllerState();
    }
}

SystemTrayManager::~SystemTrayManager() { hide(); }

void SystemTrayManager::show() { m_trayIcon->show(); }

void SystemTrayManager::hide() { m_trayIcon->hide(); }

void SystemTrayManager::setToolTip(const QString& message) {
    m_trayIcon->setToolTip("Via\n" + message);
}

void SystemTrayManager::showNotification(const QString& title, const QString& message,
                                         QSystemTrayIcon::MessageIcon icon) {
    QSettings settings;
    if (settings.value("advanced/showNotifications", true).toBool()) {
        recordNotification(title, message);
        m_trayIcon->showMessage(title, message, icon, 5000);
    }
}

void SystemTrayManager::recordNotification(const QString& title, const QString& message) {
    const QDateTime now = QDateTime::currentDateTime();
    if (!m_notificationHistory.isEmpty()) {
        const NotificationEntry& latest = m_notificationHistory.first();
        if (latest.title == title && latest.message == message &&
            latest.timestamp.secsTo(now) <= 2) {
            return;
        }
    }

    NotificationEntry entry;
    entry.title = title;
    entry.message = message;
    entry.timestamp = now;

    m_notificationHistory.prepend(entry);
    while (m_notificationHistory.size() > MAX_NOTIFICATION_HISTORY) {
        m_notificationHistory.removeLast();
    }

    refreshNotificationMenu();
}

void SystemTrayManager::createMenu() {
    m_trayMenu = new QMenu();

    // Status action (non-clickable)
    m_statusAction = m_trayMenu->addAction("Not connected");
    m_statusAction->setEnabled(false);

    m_trayMenu->addSeparator();

    // Open Drive folder
    m_openFolderAction = m_trayMenu->addAction("Open Google Drive Folder");
    m_openFolderAction->setEnabled(false);
    connect(m_openFolderAction, &QAction::triggered, this, &SystemTrayManager::onOpenFolderClicked);

    m_trayMenu->addSeparator();

    // Pause/Resume sync
    m_pauseSyncAction = m_trayMenu->addAction("Pause Sync");
    m_pauseSyncAction->setEnabled(false);
    connect(m_pauseSyncAction, &QAction::triggered, this, &SystemTrayManager::onPauseSyncClicked);

    // Sync now
    m_syncNowAction = m_trayMenu->addAction("Sync Now");
    m_syncNowAction->setEnabled(false);
    connect(m_syncNowAction, &QAction::triggered, this, &SystemTrayManager::onSyncNowClicked);

    m_trayMenu->addSeparator();

    // Recent changes
    m_recentChangesAction = m_trayMenu->addAction("Recent Changes...");
    m_recentChangesAction->setEnabled(false);
    connect(m_recentChangesAction, &QAction::triggered, this,
            &SystemTrayManager::onRecentChangesClicked);

    m_notificationsMenu = m_trayMenu->addMenu("Recent Notifications");
    m_noNotificationsAction = m_notificationsMenu->addAction("No notifications yet");
    m_noNotificationsAction->setEnabled(false);
    m_notificationsMenu->menuAction()->setEnabled(false);

    m_trayMenu->addSeparator();

    // Open main window
    m_openWindowAction = m_trayMenu->addAction("Open Via");
    connect(m_openWindowAction, &QAction::triggered, this, &SystemTrayManager::showWindowRequested);

    // Settings
    m_settingsAction = m_trayMenu->addAction("Settings...");
    connect(m_settingsAction, &QAction::triggered, this, [this]() { emit showWindowRequested(); });

    m_trayMenu->addSeparator();

    // Quit
    m_quitAction = m_trayMenu->addAction("Quit");
    connect(m_quitAction, &QAction::triggered, this, &SystemTrayManager::quitRequested);

    m_trayIcon->setContextMenu(m_trayMenu);
}

void SystemTrayManager::refreshNotificationMenu() {
    if (!m_notificationsMenu) {
        return;
    }

    m_notificationsMenu->clear();

    if (m_notificationHistory.isEmpty()) {
        m_noNotificationsAction = m_notificationsMenu->addAction("No notifications yet");
        m_noNotificationsAction->setEnabled(false);
        m_notificationsMenu->menuAction()->setEnabled(false);
        return;
    }

    m_notificationsMenu->menuAction()->setEnabled(true);

    for (const NotificationEntry& entry : m_notificationHistory) {
        const QString timestamp = entry.timestamp.toString(QStringLiteral("HH:mm:ss"));
        QString label = QStringLiteral("[%1] %2").arg(timestamp, entry.title);
        if (label.size() > 72) {
            label = label.left(69) + QStringLiteral("...");
        }

        QAction* action = m_notificationsMenu->addAction(label);
        const QString fullText =
            QStringLiteral("%1\n%2\n%3")
                .arg(QLocale::system().toString(entry.timestamp, QLocale::ShortFormat), entry.title,
                     entry.message);
        action->setToolTip(fullText);
        action->setStatusTip(fullText);
        action->setWhatsThis(fullText);
        connect(action, &QAction::triggered, this, [this, entry]() {
            if (m_trayIcon && m_trayIcon->isVisible()) {
                m_trayIcon->showMessage(entry.title, entry.message, QSystemTrayIcon::Information,
                                        15000);
            }
            emit notificationClicked();
        });
    }
}

void SystemTrayManager::updateAuthState(bool authenticated) {
    m_authenticated = authenticated;
    m_openFolderAction->setEnabled(authenticated);
    m_pauseSyncAction->setEnabled(authenticated);
    m_syncNowAction->setEnabled(authenticated);
    m_recentChangesAction->setEnabled(authenticated);
    if (authenticated) {
        applyPauseControllerState();
    } else {
        updatePauseAction(false);
    }
}

void SystemTrayManager::updatePauseAction(bool paused) {
    m_syncPaused = paused;
    m_pauseSyncAction->setText(paused ? "Resume Sync" : "Pause Sync");
}

void SystemTrayManager::applyPauseControllerState() {
    if (!m_pauseController) {
        return;
    }

    updatePauseAction(m_pauseController->isEffectivelyPaused());
}

void SystemTrayManager::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    switch (reason) {
        case QSystemTrayIcon::Trigger:
        case QSystemTrayIcon::DoubleClick:
            emit showWindowRequested();
            break;
        case QSystemTrayIcon::Context:
            // Context menu is shown automatically
            break;
        default:
            break;
    }
}

void SystemTrayManager::onOpenFolderClicked() {
    QSettings settings;
    QString syncPath = settings.value("sync/folder", QDir::homePath() + "/GoogleDrive").toString();
    QDesktopServices::openUrl(QUrl::fromLocalFile(syncPath));
}

void SystemTrayManager::onPauseSyncClicked() {
    if (!m_pauseController) {
        return;
    }

    m_pauseController->togglePause();
    showNotification(m_pauseController->isEffectivelyPaused() ? QStringLiteral("Sync Paused")
                                                              : QStringLiteral("Sync Resumed"),
                     m_pauseController->isEffectivelyPaused()
                         ? QStringLiteral("Google Drive sync has been paused.")
                         : QStringLiteral("Google Drive sync has resumed."));
}

void SystemTrayManager::onSyncNowClicked() {
    emit fullSyncRequested();
    showNotification("Syncing", "Starting full Google Drive sync...");
}

void SystemTrayManager::onRecentChangesClicked() { emit showWindowRequested(); }

void SystemTrayManager::applyStatusSnapshot() {
    if (!m_statusCoordinator) {
        return;
    }

    const UiStatusSnapshot status = m_statusCoordinator->snapshot();
    const QString summary =
        status.combinedStatusText.isEmpty() ? QStringLiteral("Idle") : status.combinedStatusText;
    m_trayIcon->setIcon(
        ThemeHelper::trayIcon(UiStatusCoordinator::iconForPriority(status.resolvedPriority)));
    m_statusAction->setText(summary);
    setToolTip(summary);
}
