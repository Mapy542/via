/**
 * @file MainWindow.cpp
 * @brief Implementation of the main application window
 */

#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QUrl>

#include "SettingsWindow.h"
#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "sync/ChangeProcessor.h"
#include "sync/FullSync.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncActionThread.h"
#include "utils/NotificationManager.h"
#include "utils/ThemeHelper.h"

MainWindow::MainWindow(GoogleAuthManager* authManager, GoogleDriveClient* driveClient,
                       SyncActionQueue* syncActionQueue, ChangeProcessor* changeProcessor,
                       SyncActionThread* syncActionThread, FullSync* fullSync,
                       NotificationManager* notificationManager, QWidget* parent)
    : QMainWindow(parent),
      m_authManager(authManager),
      m_driveClient(driveClient),
      m_syncActionQueue(syncActionQueue),
      m_syncActionThread(syncActionThread),
      m_changeProcessor(changeProcessor),
      m_fullSync(fullSync),
      m_notificationManager(notificationManager),
      m_settingsWindow(nullptr),
      m_syncPaused(false),
      m_authExpired(false) {
    setWindowTitle("Via");
    setMinimumSize(500, 600);
    resize(550, 700);

    setupUi();
    setupMenuBar();
    connectSignals();

    // Setup update timer for periodic status updates
    m_updateTimer = new QTimer(this);
    connect(m_updateTimer, &QTimer::timeout, this, [this]() {
        if (m_authExpired) {
            updateSyncStatus("Authentication expired");
            return;
        }

        if (m_changeProcessor) {
            ChangeProcessor::State state = m_changeProcessor->state();
            if (state == ChangeProcessor::State::Running) {
                int pendingActions = m_syncActionQueue ? m_syncActionQueue->count() : 0;
                if (pendingActions > 0) {
                    updateSyncStatus(QString("Syncing... (%1 pending)").arg(pendingActions));
                } else {
                    updateSyncStatus("Up to date");
                }
            } else if (state == ChangeProcessor::State::Paused) {
                updateSyncStatus("Paused");
            } else {
                updateSyncStatus("Stopped");
            }
        }
    });
    m_updateTimer->start(5000);  // Update every 5 seconds

    // Initialize auth state
    updateAuthState(m_authManager ? m_authManager->isAuthenticated() : false);
}

MainWindow::~MainWindow() {
    if (m_settingsWindow) {
        delete m_settingsWindow;
    }
}

void MainWindow::setupUi() {
    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);

    m_mainLayout = new QVBoxLayout(m_centralWidget);
    m_mainLayout->setSpacing(15);
    m_mainLayout->setContentsMargins(20, 20, 20, 20);

    // === Status Section ===
    m_statusGroup = new QGroupBox("Sync Status", this);
    QVBoxLayout* statusLayout = new QVBoxLayout(m_statusGroup);

    QHBoxLayout* statusRow = new QHBoxLayout();
    m_statusIcon = new QLabel(this);
    m_statusIcon->setPixmap(ThemeHelper::guiIcon("drive-idle.svg").pixmap(32, 32));
    m_statusIcon->setFixedSize(32, 32);
    m_statusLabel = new QLabel("Not connected", this);
    m_statusLabel->setStyleSheet("QLabel { font-size: 14px; }");
    statusRow->addWidget(m_statusIcon);
    statusRow->addWidget(m_statusLabel, 1);
    statusLayout->addLayout(statusRow);

    // Pending actions counter
    m_pendingActionsLabel = new QLabel("Pending actions: 0", this);
    m_pendingActionsLabel->setStyleSheet("QLabel { font-size: 12px; color: #666; }");
    statusLayout->addWidget(m_pendingActionsLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setVisible(false);
    statusLayout->addWidget(m_progressBar);

    m_mainLayout->addWidget(m_statusGroup);

    // === Account Section ===
    m_accountGroup = new QGroupBox("Account", this);
    QVBoxLayout* accountLayout = new QVBoxLayout(m_accountGroup);

    m_accountLabel = new QLabel("Not logged in", this);
    m_accountLabel->setStyleSheet("QLabel { font-size: 13px; }");
    accountLayout->addWidget(m_accountLabel);

    QHBoxLayout* accountButtons = new QHBoxLayout();
    m_loginButton = new QPushButton("Sign In with Google", this);
    m_loginButton->setMinimumHeight(35);
    m_logoutButton = new QPushButton("Sign Out", this);
    m_logoutButton->setMinimumHeight(35);
    m_logoutButton->setVisible(false);
    accountButtons->addWidget(m_loginButton);
    accountButtons->addWidget(m_logoutButton);
    accountButtons->addStretch();
    accountLayout->addLayout(accountButtons);

    m_mainLayout->addWidget(m_accountGroup);

    // === Actions Section ===
    m_actionsGroup = new QGroupBox("Quick Actions", this);
    QHBoxLayout* actionsLayout = new QHBoxLayout(m_actionsGroup);

    m_openFolderButton = new QPushButton("Open Drive Folder", this);
    m_openFolderButton->setMinimumHeight(35);
    m_openFolderButton->setEnabled(false);

    m_pauseSyncButton = new QPushButton("Pause Sync", this);
    m_pauseSyncButton->setMinimumHeight(35);
    m_pauseSyncButton->setEnabled(false);

    m_refreshButton = new QPushButton("Sync Now", this);
    m_refreshButton->setMinimumHeight(35);
    m_refreshButton->setEnabled(false);

    m_settingsButton = new QPushButton("Settings", this);
    m_settingsButton->setMinimumHeight(35);

    actionsLayout->addWidget(m_openFolderButton);
    actionsLayout->addWidget(m_pauseSyncButton);
    actionsLayout->addWidget(m_refreshButton);
    actionsLayout->addWidget(m_settingsButton);

    m_mainLayout->addWidget(m_actionsGroup);

    // === Recent Activity Section ===
    m_activityGroup = new QGroupBox("Recent Activity", this);
    QVBoxLayout* activityLayout = new QVBoxLayout(m_activityGroup);

    m_activityList = new QListWidget(this);
    m_activityList->setMinimumHeight(200);
    m_activityList->setAlternatingRowColors(true);
    activityLayout->addWidget(m_activityList);

    m_mainLayout->addWidget(m_activityGroup, 1);

    // Add welcome message
    addRecentActivity("Welcome to Via!");
}

void MainWindow::setupMenuBar() {
    QMenuBar* menuBar = new QMenuBar(this);
    setMenuBar(menuBar);

    // File menu
    QMenu* fileMenu = menuBar->addMenu("&File");

    QAction* openFolderAction = fileMenu->addAction("Open Drive Folder");
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::onOpenFolderClicked);

    fileMenu->addSeparator();

    QAction* settingsAction = fileMenu->addAction("Settings...");
    settingsAction->setShortcut(QKeySequence::Preferences);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsClicked);

    fileMenu->addSeparator();

    QAction* quitAction = fileMenu->addAction("Quit");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // Sync menu
    QMenu* syncMenu = menuBar->addMenu("&Sync");

    QAction* syncNowAction = syncMenu->addAction("Sync Now");
    connect(syncNowAction, &QAction::triggered, this, &MainWindow::onRefreshClicked);

    QAction* pauseAction = syncMenu->addAction("Pause/Resume Sync");
    connect(pauseAction, &QAction::triggered, this, &MainWindow::onPauseSyncClicked);

    // Account menu
    QMenu* accountMenu = menuBar->addMenu("&Account");

    QAction* loginAction = accountMenu->addAction("Sign In...");
    connect(loginAction, &QAction::triggered, this, &MainWindow::onLoginClicked);

    QAction* logoutAction = accountMenu->addAction("Sign Out");
    connect(logoutAction, &QAction::triggered, this, &MainWindow::onLogoutClicked);

    // Help menu
    QMenu* helpMenu = menuBar->addMenu("&Help");

    QAction* reportIssueAction = helpMenu->addAction("Report Issue...");
    connect(reportIssueAction, &QAction::triggered, this,
            []() { QDesktopServices::openUrl(QUrl("https://github.com/Mapy542/Via/issues")); });

    helpMenu->addSeparator();

    QAction* aboutAction = helpMenu->addAction("About");
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(
            this, "About Via",
            "<h2>Via</h2>"
            "<p>Version 1.0.0</p>"
            "<p>A Google Drive desktop client for Linux.</p>"
            "<p>Features:</p>"
            "<ul>"
            "<li>Bidirectional file synchronization</li>"
            "<li>Virtual file system (FUSE)</li>"
            "<li>Conflict resolution</li>"
            "<li>Offline file access</li>"
            "</ul>"
            "<hr>"
            "<p><b>Open-Source Licenses</b></p>"
            "<p>This application is built with the "
            "<a href=\"https://www.qt.io/\">Qt Framework</a>, used under the "
            "<a href=\"https://www.gnu.org/licenses/lgpl-3.0.html\">GNU LGPL v3.0</a> license. "
            "Qt source code is available at "
            "<a href=\"https://code.qt.io/\">code.qt.io</a>. "
            "Qt libraries are dynamically linked; you may re-link against your own "
            "Qt build. No modifications have been made to the Qt source code.</p>"
            "<p>Icons derived from <a href=\"https://github.com/KDE/breeze-icons\">Breeze "
            "Icons</a> "
            "by the KDE Community, licensed under "
            "<a href=\"https://www.gnu.org/licenses/lgpl-3.0.html\">LGPL-3.0</a>.</p>"
            "<p>FUSE support via <a href=\"https://github.com/libfuse/libfuse\">libfuse</a>, "
            "licensed under "
            "<a href=\"https://www.gnu.org/licenses/lgpl-2.1.html\">LGPL-2.1</a>.</p>"
            "<p>This application is licensed under the "
            "<a href=\"https://opensource.org/licenses/MIT\">MIT License</a>.</p>"
            "<hr>"
            "<p><a href=\"https://github.com/Mapy542/Via\">Source Code on GitHub</a></p>"
            "<p>&copy; 2024 Via Contributors</p>");
    });
}

void MainWindow::connectSignals() {
    // Button connections
    connect(m_loginButton, &QPushButton::clicked, this, &MainWindow::onLoginClicked);
    connect(m_logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
    connect(m_openFolderButton, &QPushButton::clicked, this, &MainWindow::onOpenFolderClicked);
    connect(m_pauseSyncButton, &QPushButton::clicked, this, &MainWindow::onPauseSyncClicked);
    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshClicked);

    // Auth manager connections
    if (m_authManager) {
        connect(m_authManager, &GoogleAuthManager::authenticated, this,
                [this]() { updateAuthState(true); });
        connect(m_authManager, &GoogleAuthManager::loggedOut, this,
                [this]() { updateAuthState(false); });
        connect(m_authManager, &GoogleAuthManager::authenticationError, this,
                [this](const QString& error) {
                    QMessageBox::warning(this, "Authentication Error", error);
                    addRecentActivity("Authentication failed: " + error);
                });
        connect(
            m_authManager, &GoogleAuthManager::tokenRefreshError, this,
            [this](const QString& error) { addRecentActivity("Token refresh error: " + error); });
    }

    // SyncActionQueue and ChangeProcessor connections
    if (m_syncActionQueue) {
        connect(m_syncActionQueue, &SyncActionQueue::itemEnqueued, this,
                [this](const SyncActionItem& item) {
                    switch (item.actionType) {
                        case SyncActionType::Upload:
                            addRecentActivity("Queued upload: " + item.localPath);
                            break;
                        case SyncActionType::Download:
                            addRecentActivity("Queued download: " + item.localPath);
                            break;
                        case SyncActionType::DeleteLocal:
                        case SyncActionType::DeleteRemote:
                            addRecentActivity("Queued delete: " + item.localPath);
                            break;
                        case SyncActionType::MoveLocal:
                        case SyncActionType::MoveRemote:
                            addRecentActivity("Queued move: " + item.localPath);
                            break;
                        case SyncActionType::RenameLocal:
                        case SyncActionType::RenameRemote:
                            addRecentActivity("Queued rename: " + item.localPath);
                            break;
                    }
                    if (m_syncActionQueue) {
                        updatePendingActions(m_syncActionQueue->count());
                    }
                });
        connect(m_syncActionQueue, &SyncActionQueue::queueEmpty, this,
                [this]() { updatePendingActions(0); });
    }

    if (m_changeProcessor) {
        connect(m_changeProcessor, &ChangeProcessor::stateChanged, this,
                [this](ChangeProcessor::State state) {
                    switch (state) {
                        case ChangeProcessor::State::Running:
                            updateSyncStatus("Syncing...");
                            break;
                        case ChangeProcessor::State::Paused:
                            updateSyncStatus("Paused");
                            break;
                        case ChangeProcessor::State::Stopped:
                            updateSyncStatus("Stopped");
                            break;
                    }
                });
        connect(m_changeProcessor, &ChangeProcessor::error, this,
                [this](const QString& error) { addRecentActivity("Error: " + error); });
        connect(
            m_changeProcessor, &ChangeProcessor::conflictDetected, this,
            [this](const ConflictInfo& info) { addRecentActivity("Conflict: " + info.localPath); });
        connect(m_changeProcessor, &ChangeProcessor::conflictResolved, this,
                [this](const QString& localPath, ConflictResolutionStrategy) {
                    addRecentActivity("Conflict resolved: " + localPath);
                });
        connect(m_changeProcessor, &ChangeProcessor::changeProcessed, this,
                [this](const QString& localPath) { addRecentActivity("Processed: " + localPath); });
    }

    // FullSync connections
    if (m_fullSync) {
        connect(m_fullSync, &FullSync::stateChanged, this, [this](FullSync::State state) {
            switch (state) {
                case FullSync::State::ScanningLocal:
                    updateSyncStatus("Scanning local files...");
                    break;
                case FullSync::State::FetchingRemote:
                    updateSyncStatus("Fetching remote files...");
                    break;
                case FullSync::State::Complete:
                    updateSyncStatus("Syncing...");
                    break;
                case FullSync::State::Error:
                    updateSyncStatus("Sync error");
                    break;
                case FullSync::State::Idle:
                    // Don't change status for idle
                    break;
            }
        });
        connect(m_fullSync, &FullSync::progressUpdated, this,
                [this](const QString& phase, int current, int total) {
                    Q_UNUSED(total);
                    addRecentActivity(QString("%1 (%2 files)").arg(phase).arg(current));
                });
        connect(m_fullSync, &FullSync::completed, this, [this](int localCount, int remoteCount) {
            addRecentActivity(QString("Full sync complete: %1 local, %2 remote files")
                                  .arg(localCount)
                                  .arg(remoteCount));
        });
        connect(m_fullSync, &FullSync::error, this,
                [this](const QString& error) { addRecentActivity("Full sync error: " + error); });
    }
}

void MainWindow::updateSyncStatus(const QString& status) {
    m_statusLabel->setText(status);

    // Update status icon using palette-based theme detection for GUI
    if (status.contains("Syncing") || status.contains("Uploading") ||
        status.contains("Downloading") || status.contains("Scanning") ||
        status.contains("Fetching")) {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("sync-active.svg").pixmap(32, 32));
    } else if (status.contains("Up to date") || status.contains("Complete") ||
               status.contains("Ready")) {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("drive-idle.svg").pixmap(32, 32));
    } else if (status.contains("Error") || status.contains("Failed")) {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("error.svg").pixmap(32, 32));
    } else if (status.contains("Paused")) {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("paused.svg").pixmap(32, 32));
    } else if (status.contains("expired") || status.contains("Authentication")) {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("auth-expired.svg").pixmap(32, 32));
    } else if (status.contains("Not connected") || status.contains("Offline")) {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("no-connection.svg").pixmap(32, 32));
    } else if (status.contains("Conflict")) {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("warn.svg").pixmap(32, 32));
    } else {
        m_statusIcon->setPixmap(ThemeHelper::guiIcon("drive-idle.svg").pixmap(32, 32));
    }
}

void MainWindow::updateSyncProgress(qint64 current, qint64 total) {
    if (total > 0) {
        m_progressBar->setVisible(true);
        int percentage = static_cast<int>((current * 100) / total);
        m_progressBar->setValue(percentage);
    } else {
        m_progressBar->setVisible(false);
    }
}

void MainWindow::updatePendingActions(int count) {
    if (count > 0) {
        m_pendingActionsLabel->setText(QString("Pending actions: %1").arg(count));
        m_pendingActionsLabel->setStyleSheet(
            "QLabel { font-size: 12px; color: #0066cc; font-weight: bold; }");
    } else {
        m_pendingActionsLabel->setText("Pending actions: 0");
        m_pendingActionsLabel->setStyleSheet("QLabel { font-size: 12px; color: #666; }");
    }
}

void MainWindow::addRecentActivity(const QString& activity) {
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString formattedActivity = QString("[%1] %2").arg(timestamp, activity);

    m_activityList->insertItem(0, formattedActivity);

    // Keep only the last 100 items
    while (m_activityList->count() > 100) {
        delete m_activityList->takeItem(m_activityList->count() - 1);
    }
}

void MainWindow::updateAuthState(bool authenticated) {
    m_authExpired = false;
    m_loginButton->setVisible(!authenticated);
    m_logoutButton->setVisible(authenticated);
    m_openFolderButton->setEnabled(authenticated);
    m_pauseSyncButton->setEnabled(authenticated);
    m_refreshButton->setEnabled(authenticated);

    if (authenticated) {
        m_accountLabel->setText("Signed in to Google Drive");
        updateSyncStatus("Ready to sync");
        addRecentActivity("Signed in successfully");
    } else {
        m_accountLabel->setText("Not logged in");
        updateSyncStatus("Not connected");
    }
}

void MainWindow::setAuthExpired(const QString& reason) {
    m_authExpired = true;
    m_loginButton->setVisible(true);
    m_logoutButton->setVisible(false);
    m_openFolderButton->setEnabled(false);
    m_pauseSyncButton->setEnabled(false);
    m_refreshButton->setEnabled(false);

    QString message = "Session expired - sign in again";
    if (!reason.isEmpty()) {
        message += " (" + reason + ")";
    }

    m_accountLabel->setText(message);
    updateSyncStatus("Authentication expired");
    addRecentActivity("Authentication expired: " + reason);
}

void MainWindow::updatePauseButton(bool paused) {
    m_syncPaused = paused;
    m_pauseSyncButton->setText(paused ? "Resume Sync" : "Pause Sync");
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Hide to system tray instead of closing
    hide();
    event->ignore();
}

void MainWindow::onLoginClicked() {
    if (m_authManager) {
        addRecentActivity("Starting authentication...");
        m_authManager->authenticate();
    }
}

void MainWindow::onLogoutClicked() {
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Sign Out",
        "Are you sure you want to sign out?\n\n"
        "Synchronization will stop and you will need to sign in again to resume.",
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        if (m_authManager) {
            m_authManager->logout();
            addRecentActivity("Signed out");
        }
    }
}

void MainWindow::onSettingsClicked() {
    if (!m_settingsWindow) {
        m_settingsWindow = new SettingsWindow(m_authManager, m_syncActionQueue, m_changeProcessor,
                                              m_driveClient, this);
    }
    m_settingsWindow->show();
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
}

void MainWindow::onOpenFolderClicked() {
    QSettings settings;
    QString syncPath = settings.value("sync/folder", QDir::homePath() + "/GoogleDrive").toString();

    QUrl folderUrl = QUrl::fromLocalFile(syncPath);
    if (!QDesktopServices::openUrl(folderUrl)) {
        QMessageBox::warning(this, "Error",
                             "Could not open the Google Drive folder.\n"
                             "Path: " +
                                 syncPath);
    }
}

void MainWindow::onPauseSyncClicked() {
    if (m_changeProcessor) {
        if (m_syncPaused) {
            m_changeProcessor->resume();
            if (m_syncActionThread) {
                m_syncActionThread->resume();
            }
            updatePauseButton(false);
            updateSyncStatus("Resuming sync...");
            addRecentActivity("Sync resumed");
        } else {
            m_changeProcessor->pause();
            if (m_syncActionThread) {
                m_syncActionThread->pause();
            }
            updatePauseButton(true);
            updateSyncStatus("Paused");
            addRecentActivity("Sync paused");
        }
    }
}

void MainWindow::onRefreshClicked() {
    // The "Sync Now" button triggers a full resync using FullSync
    // Per spec: "sync now button in the main UI should also initiate a full resync"
    if (m_fullSync) {
        if (m_fullSync->isRunning()) {
            addRecentActivity("Full sync already in progress...");
            return;
        }

        addRecentActivity("Starting full sync...");

        // Ensure change processor is running to process the queued items
        if (m_changeProcessor && m_changeProcessor->state() != ChangeProcessor::State::Running) {
            m_changeProcessor->start();
        }

        // Start the full sync to discover all files
        m_fullSync->fullSync();
    } else if (m_changeProcessor) {
        // Fallback if no FullSync available
        addRecentActivity("Starting sync...");
        if (m_changeProcessor->state() != ChangeProcessor::State::Running) {
            m_changeProcessor->start();
        }
    }
}
