/**
 * @file SystemTrayManager.cpp
 * @brief Implementation of the system tray manager
 */

#include "SystemTrayManager.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QIcon>
#include <QSettings>
#include <QUrl>

#include "auth/GoogleAuthManager.h"
#include "sync/ChangeProcessor.h"
#include "sync/SyncActionQueue.h"
#include "utils/ThemeHelper.h"

SystemTrayManager::SystemTrayManager(GoogleAuthManager* authManager,
                                     SyncActionQueue* syncActionQueue,
                                     ChangeProcessor* changeProcessor, QObject* parent)
    : QObject(parent),
      m_authManager(authManager),
      m_syncActionQueue(syncActionQueue),
      m_changeProcessor(changeProcessor),
      m_syncPaused(false),
      m_hasConflicts(false),
      m_storagePercent(-1.0) {
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

    // Connect to change processor state changes
    if (m_changeProcessor) {
        connect(m_changeProcessor, &ChangeProcessor::stateChanged, this,
                [this](ChangeProcessor::State) {
                    // The periodic timer handles status text; just refresh now
                    refreshStatus();
                });
    }

    // Periodic status timer — mirrors MainWindow's update timer
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &SystemTrayManager::refreshStatus);
    m_statusTimer->start(5000);
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
        m_trayIcon->showMessage(title, message, icon, 5000);
    }
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

void SystemTrayManager::updateSyncStatus(const QString& status) {
    m_statusAction->setText(status);
    setToolTip(status);

    // If there are conflicts, show warn icon regardless of other status
    if (m_hasConflicts) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("warn.svg"));
        return;
    }

    // Update icon based on status using theme-aware icons
    // Active/error/auth states take precedence over storage warnings
    if (status.contains("Syncing") || status.contains("Uploading") ||
        status.contains("Downloading") || status.contains("Scanning") ||
        status.contains("Fetching")) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("sync-active.svg"));
    } else if (status.contains("Error") || status.contains("Failed")) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("error.svg"));
    } else if (status.contains("Paused")) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("paused.svg"));
    } else if (status.contains("expired") || status.contains("Authentication")) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("auth-expired.svg"));
    } else if (status.contains("Not connected") || status.contains("Offline")) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("no-connection.svg"));
    } else if (m_storagePercent >= 90.0) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("critical-low-storage.svg"));
    } else if (m_storagePercent >= 75.0) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("low-storage.svg"));
    } else if (status.contains("Up to date") || status.contains("Complete") ||
               status.contains("Ready")) {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("drive-idle.svg"));
    } else {
        m_trayIcon->setIcon(ThemeHelper::trayIcon("drive-idle.svg"));
    }
}

void SystemTrayManager::setHasConflicts(bool hasConflicts) {
    if (m_hasConflicts != hasConflicts) {
        m_hasConflicts = hasConflicts;
        if (hasConflicts) {
            m_trayIcon->setIcon(ThemeHelper::trayIcon("warn.svg"));
            showNotification("Conflicts Detected",
                             "There are file conflicts that need your attention.",
                             QSystemTrayIcon::Warning);
        } else {
            // Re-apply the current status icon
            updateSyncStatus(m_statusAction->text());
        }
    }
}

void SystemTrayManager::updateAuthState(bool authenticated) {
    m_openFolderAction->setEnabled(authenticated);
    m_pauseSyncAction->setEnabled(authenticated);
    m_syncNowAction->setEnabled(authenticated);
    m_recentChangesAction->setEnabled(authenticated);

    if (authenticated) {
        m_statusAction->setText("Ready to sync");
    } else {
        m_statusAction->setText("Not connected");
    }
}

void SystemTrayManager::updatePauseAction(bool paused) {
    m_syncPaused = paused;
    m_pauseSyncAction->setText(paused ? "Resume Sync" : "Pause Sync");
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
    if (m_changeProcessor) {
        if (m_syncPaused) {
            m_changeProcessor->resume();
            updatePauseAction(false);
            showNotification("Sync Resumed", "Google Drive sync has resumed.");
        } else {
            m_changeProcessor->pause();
            updatePauseAction(true);
            showNotification("Sync Paused", "Google Drive sync has been paused.");
        }
    }
}

void SystemTrayManager::onSyncNowClicked() {
    emit fullSyncRequested();
    showNotification("Syncing", "Starting full Google Drive sync...");
}

void SystemTrayManager::onRecentChangesClicked() { emit showWindowRequested(); }

void SystemTrayManager::updateStorageInfo(qint64 storageUsed, qint64 storageLimit) {
    if (storageLimit <= 0) {
        m_storagePercent = -1.0;
        return;
    }

    m_storagePercent = (storageUsed * 100.0) / storageLimit;

    if (m_storagePercent >= 90.0) {
        showNotification("Critical Storage Warning",
                         QString("Google Drive storage is %1% full!")
                             .arg(QString::number(m_storagePercent, 'f', 1)),
                         QSystemTrayIcon::Critical);
    } else if (m_storagePercent >= 75.0) {
        showNotification("Low Storage Warning",
                         QString("Google Drive storage is %1% full.")
                             .arg(QString::number(m_storagePercent, 'f', 1)),
                         QSystemTrayIcon::Warning);
    }

    // Refresh the icon to reflect storage state
    updateSyncStatus(m_statusAction->text());
}

void SystemTrayManager::refreshStatus() {
    if (!m_changeProcessor) {
        return;
    }

    ChangeProcessor::State state = m_changeProcessor->state();
    switch (state) {
        case ChangeProcessor::State::Running: {
            int pending = m_syncActionQueue ? m_syncActionQueue->count() : 0;
            if (pending > 0) {
                updateSyncStatus(QString("Syncing... (%1 pending)").arg(pending));
            } else {
                updateSyncStatus("Up to date");
            }
            break;
        }
        case ChangeProcessor::State::Paused:
            updateSyncStatus("Paused");
            break;
        case ChangeProcessor::State::Stopped:
            updateSyncStatus("Stopped");
            break;
    }
}
