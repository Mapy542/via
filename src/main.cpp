/**
 * @file main.cpp
 * @brief Entry point for the Via application
 *
 * This is the main entry point for the Via application,
 * a Google Drive desktop client for Linux.
 */

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <memory>

#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "auth/TokenStorage.h"
#include "fuse/FuseDriver.h"
#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/FullSync.h"
#include "sync/LocalChangeWatcher.h"
#include "sync/RemoteChangeWatcher.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncActionThread.h"
#include "sync/SyncDatabase.h"
#include "ui/MainWindow.h"
#include "ui/SystemTrayManager.h"
#include "utils/AutostartManager.h"
#include "utils/LogManager.h"
#include "utils/NotificationManager.h"
#include "utils/ThemeHelper.h"
#include "utils/UpdateChecker.h"

/**
 * @brief Initialize application directories
 * @return true if directories were created successfully
 */
bool initializeDirectories() {
    // Create application data directory
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dataDir(dataPath);
    if (!dataDir.exists()) {
        if (!dataDir.mkpath(".")) {
            qCritical() << "Failed to create application data directory:" << dataPath;
            return false;
        }
    }

    // Create default sync directory
    QString syncPath = QDir::homePath() + "/GoogleDrive";
    QDir syncDir(syncPath);
    if (!syncDir.exists()) {
        if (!syncDir.mkpath(".")) {
            qCritical() << "Failed to create sync directory:" << syncPath;
            return false;
        }
    }

    // Create cache directory
    QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir cacheDir(cachePath);
    if (!cacheDir.exists()) {
        if (!cacheDir.mkpath(".")) {
            qCritical() << "Failed to create cache directory:" << cachePath;
            return false;
        }
    }

    return true;
}

QString normalizedAbsolutePath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool pathsOverlap(const QString& pathA, const QString& pathB) {
    if (pathA.isEmpty() || pathB.isEmpty()) {
        return false;
    }

    const QString a = normalizedAbsolutePath(pathA);
    const QString b = normalizedAbsolutePath(pathB);

    if (a == b) {
        return true;
    }

    return a.startsWith(b + "/") || b.startsWith(a + "/");
}

void startFuseComponent(FuseDriver* fuseDriver, const QString& syncFolder) {
    if (!fuseDriver) {
        return;
    }

    if (fuseDriver->isMounted()) {
        return;
    }

    if (!FuseDriver::isFuseAvailable()) {
        qWarning() << "FUSE enabled in settings, but /dev/fuse is unavailable";
        return;
    }

    if (pathsOverlap(fuseDriver->mountPoint(), syncFolder)) {
        qWarning() << "FUSE mount point overlaps sync folder, refusing to mount:"
                   << fuseDriver->mountPoint() << "syncFolder=" << syncFolder;
        return;
    }

    if (!fuseDriver->mount()) {
        qWarning() << "Failed to mount FUSE filesystem";
    }
}

void stopFuseComponent(FuseDriver* fuseDriver) {
    if (!fuseDriver) {
        return;
    }

    if (fuseDriver->isMounted()) {
        fuseDriver->unmount();
    }
}

/**
 * @brief Start all sync components
 * @param localWatcher Local change watcher
 * @param remoteWatcher Remote change watcher
 * @param changeProcessor Change processor/conflict resolver
 * @param syncActionThread Sync action thread
 */
void startSyncComponents(LocalChangeWatcher* localWatcher, RemoteChangeWatcher* remoteWatcher,
                         ChangeProcessor* changeProcessor, SyncActionThread* syncActionThread) {
    qInfo() << "Starting sync components...";
    localWatcher->start();
    remoteWatcher->start();
    changeProcessor->start();
    syncActionThread->start();
}

/**
 * @brief Stop all sync components
 * @param localWatcher Local change watcher
 * @param remoteWatcher Remote change watcher
 * @param changeProcessor Change processor/conflict resolver
 * @param syncActionThread Sync action thread
 */
void stopSyncComponents(LocalChangeWatcher* localWatcher, RemoteChangeWatcher* remoteWatcher,
                        ChangeProcessor* changeProcessor, SyncActionThread* syncActionThread) {
    qInfo() << "Stopping sync components...";
    syncActionThread->stop();
    changeProcessor->stop();
    remoteWatcher->stop();
    localWatcher->stop();
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Set application metadata
    app.setApplicationName("Via");
    app.setApplicationDisplayName("Via");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Via");
    app.setOrganizationDomain("via.local");

    // Set application icon (used in taskbar and window title bars)
    app.setWindowIcon(ThemeHelper::icon("drive-idle.svg"));

    // Keep application running even when all windows are closed (for system tray)
    app.setQuitOnLastWindowClosed(false);

    // Initialize directories
    if (!initializeDirectories()) {
        QMessageBox::critical(nullptr, "Initialization Error",
                              "Failed to initialize application directories.\n"
                              "Please check permissions and try again.");
        return 1;
    }

    // Initialize logging to file
    if (!LogManager::instance().initialize()) {
        qWarning() << "Failed to initialize file logging, continuing with console only";
    } else {
        qInfo() << "Log file:" << LogManager::instance().currentLogPath();
    }

    // Install desktop integration (desktop file, icon, autostart sync)
    AutostartManager::installDesktopIntegration();

    // Initialize token storage
    TokenStorage tokenStorage;

    // Initialize Google Auth Manager
    GoogleAuthManager authManager(&tokenStorage);

    // Initialize Google Drive API client
    GoogleDriveClient driveClient(&authManager);

    // Initialize sync database
    SyncDatabase syncDatabase;
    if (!syncDatabase.initialize()) {
        QMessageBox::critical(nullptr, "Database Error",
                              "Failed to initialize the sync database.\n"
                              "Please check permissions and try again.");
        return 1;
    }

    // Initialize notification manager
    NotificationManager notificationManager;

    // Check for updates (runs asynchronously, shows dialog if update found)
    UpdateChecker updateChecker;
    updateChecker.checkForUpdates(/* silent = */ true);

    QSettings settings;

    // Read sync system mode: "mirror-only", "fuse-only", or "both"
    // Migrate from legacy "advanced/enableFuse" boolean if needed
    QString syncSystemMode = settings.value("advanced/syncSystem", "").toString();
    if (syncSystemMode.isEmpty()) {
        bool legacyFuse = settings.value("advanced/enableFuse", false).toBool();
        syncSystemMode = legacyFuse ? "both" : "mirror-only";
    }
    const bool fuseEnabled = (syncSystemMode == "fuse-only" || syncSystemMode == "both");
    const bool mirrorEnabled = (syncSystemMode == "mirror-only" || syncSystemMode == "both");
    qInfo() << "Sync system mode:" << syncSystemMode << "(mirror:" << mirrorEnabled
            << "fuse:" << fuseEnabled << ")";

    const QString fuseMountPoint =
        settings.value("advanced/fuseMountPoint", QDir::homePath() + "/GoogleDriveFuse").toString();
    const qint64 cacheSizeMb = settings.value("advanced/cacheSize", 5000).toLongLong();

    FuseDriver fuseDriver(&driveClient, &syncDatabase);
    if (!fuseMountPoint.isEmpty()) {
        fuseDriver.setMountPoint(fuseMountPoint);
    }
    fuseDriver.setMaxCacheSizeBytes(cacheSizeMb * 1024LL * 1024LL);

    // Initialize sync components (Change Queue, Sync Action Queue, Watchers, Processor)
    ChangeQueue changeQueue;
    SyncActionQueue syncActionQueue;

    // Initialize local change watcher
    LocalChangeWatcher localWatcher(&changeQueue);
    QString syncFolder =
        settings.value("sync/folder", QDir::homePath() + "/GoogleDrive").toString();
    // TODO: merge this. ignore patters are set in many places. just store in db or something
    localWatcher.setSyncFolder(syncFolder);
    localWatcher.setIgnorePatterns({"*.tmp", "*.swp", "*~", ".git/*", ".DS_Store"});

    // Initialize remote change watcher
    RemoteChangeWatcher remoteWatcher(&changeQueue, &driveClient, &syncDatabase);

    // Load stored change token if available
    QString storedToken = syncDatabase.getChangeToken();
    if (!storedToken.isEmpty()) {
        remoteWatcher.setChangeToken(storedToken);
    }

    // Initialize change processor/conflict resolver
    ChangeProcessor changeProcessor(&changeQueue, &syncActionQueue, &syncDatabase, &driveClient);

    QObject::connect(&fuseDriver, &FuseDriver::mountError, &notificationManager,
                     [&notificationManager](const QString& error) {
                         notificationManager.showError("FUSE Mount Error", error);
                     });
    // Initialize sync action thread (executes sync actions from queue)
    SyncActionThread syncActionThread(&syncActionQueue, &syncDatabase, &driveClient,
                                      &changeProcessor, &localWatcher);
    syncActionThread.setSyncFolder(syncFolder);
    changeProcessor.setSyncFolder(syncFolder);  // share sync folder with change processor

    // Wire conflict resolution strategy from settings to ChangeProcessor
    {
        SyncSettings syncSettings = SyncSettings::load();
        ConflictResolutionStrategy strategy = ConflictResolutionStrategy::KeepBoth;
        if (syncSettings.conflictStrategy == "keep-local") {
            strategy = ConflictResolutionStrategy::KeepLocal;
        } else if (syncSettings.conflictStrategy == "keep-remote") {
            strategy = ConflictResolutionStrategy::KeepRemote;
        } else if (syncSettings.conflictStrategy == "keep-both") {
            strategy = ConflictResolutionStrategy::KeepBoth;
        } else if (syncSettings.conflictStrategy == "keep-newest") {
            strategy = ConflictResolutionStrategy::KeepNewest;
        } else if (syncSettings.conflictStrategy == "ask-user") {
            strategy = ConflictResolutionStrategy::AskUser;
        }
        changeProcessor.setConflictResolutionStrategy(strategy);
    }

    // Initialize full sync handler
    FullSync fullSync(&changeQueue, &syncDatabase, &driveClient, &changeProcessor);
    fullSync.setSyncFolder(syncFolder);

    // Periodic local-only full sync (every 5 minutes)
    QTimer fullSyncLocalTimer(&app);
    fullSyncLocalTimer.setInterval(5 * 60 * 1000);
    fullSyncLocalTimer.setSingleShot(false);
    QObject::connect(&fullSyncLocalTimer, &QTimer::timeout, &fullSync, &FullSync::fullSyncLocal);

    // Connect change queue to processor wake-up signal
    QObject::connect(&changeQueue, &ChangeQueue::itemsAvailable, &changeProcessor,
                     &ChangeProcessor::onItemsAvailable);

    // Connect sync action queue to action thread wake-up signal
    QObject::connect(&syncActionQueue, &SyncActionQueue::itemsAvailable, &syncActionThread,
                     &SyncActionThread::onItemsAvailable);

    // Connect remote watcher to save change tokens to database
    QObject::connect(&remoteWatcher, &RemoteChangeWatcher::changeTokenUpdated, &syncDatabase,
                     &SyncDatabase::setChangeToken);

    // Initialize system tray manager
    SystemTrayManager trayManager(&authManager, &syncActionQueue, &changeProcessor);
    trayManager.show();

    // Initialize main window
    // When mirror sync is disabled, pass nullptr for sync components so UI disables sync actions
    MainWindow mainWindow(&authManager, &driveClient, mirrorEnabled ? &syncActionQueue : nullptr,
                          mirrorEnabled ? &changeProcessor : nullptr,
                          mirrorEnabled ? &syncActionThread : nullptr,
                          mirrorEnabled ? &fullSync : nullptr, &notificationManager);

    // Connect signals for application-wide coordination

    // When authenticated, start sync components based on configured mode
    QObject::connect(&authManager, &GoogleAuthManager::authenticated, &app,
                     [&localWatcher, &remoteWatcher, &changeProcessor, &syncActionThread, &fullSync,
                      &fullSyncLocalTimer, &fuseDriver, fuseEnabled, mirrorEnabled, &syncFolder]() {
                         if (mirrorEnabled) {
                             startSyncComponents(&localWatcher, &remoteWatcher, &changeProcessor,
                                                 &syncActionThread);
                             fullSyncLocalTimer.start();
                             // Trigger full sync after authentication to ensure drive is fully
                             // synced
                             QTimer::singleShot(500, &fullSync, &FullSync::fullSync);
                         }
                         if (fuseEnabled) {
                             startFuseComponent(&fuseDriver, syncFolder);
                         }
                     });

    // When logged out, stop sync components
    QObject::connect(&authManager, &GoogleAuthManager::loggedOut, &app,
                     [&localWatcher, &remoteWatcher, &changeProcessor, &syncActionThread, &fullSync,
                      &fullSyncLocalTimer, &fuseDriver, mirrorEnabled, fuseEnabled]() {
                         if (mirrorEnabled) {
                             fullSync.cancel();
                             fullSyncLocalTimer.stop();
                             stopSyncComponents(&localWatcher, &remoteWatcher, &changeProcessor,
                                                &syncActionThread);
                         }
                         if (fuseEnabled) {
                             stopFuseComponent(&fuseDriver);
                         }
                     });

    // Connect tray manager to main window
    QObject::connect(&trayManager, &SystemTrayManager::showWindowRequested, &mainWindow,
                     &MainWindow::show);
    QObject::connect(&trayManager, &SystemTrayManager::quitRequested, &app, &QApplication::quit);

    // Connect tray "Sync Now" to full sync (only when mirror sync is enabled)
    if (mirrorEnabled) {
        QObject::connect(&trayManager, &SystemTrayManager::fullSyncRequested, &fullSync,
                         &FullSync::fullSync);
    }

    // Connect storage info to tray for storage-level icons
    QObject::connect(&driveClient, &GoogleDriveClient::aboutInfoReceived, &trayManager,
                     &SystemTrayManager::updateStorageInfo);

    // Connect sync action thread status updates to tray
    QObject::connect(
        &syncActionThread, &SyncActionThread::actionCompleted, &trayManager,
        [&trayManager](const SyncActionItem&) { trayManager.updateSyncStatus("Syncing..."); });
    QObject::connect(&syncActionThread, &SyncActionThread::actionFailed, &trayManager,
                     [&trayManager](const SyncActionItem&, const QString&) {
                         trayManager.updateSyncStatus("Sync error");
                     });

    // Connect full sync state changes to tray
    QObject::connect(&fullSync, &FullSync::stateChanged, &trayManager,
                     [&trayManager](FullSync::State state) {
                         switch (state) {
                             case FullSync::State::ScanningLocal:
                                 trayManager.updateSyncStatus("Scanning local files...");
                                 break;
                             case FullSync::State::FetchingRemote:
                                 trayManager.updateSyncStatus("Fetching remote files...");
                                 break;
                             case FullSync::State::Complete:
                                 trayManager.updateSyncStatus("Syncing...");
                                 break;
                             case FullSync::State::Error:
                                 trayManager.updateSyncStatus("Sync error");
                                 break;
                             case FullSync::State::Idle:
                                 break;
                         }
                     });

    // Periodically refresh storage info (every 10 minutes)
    QTimer storageRefreshTimer(&app);
    storageRefreshTimer.setInterval(10 * 60 * 1000);
    storageRefreshTimer.setSingleShot(false);
    QObject::connect(&storageRefreshTimer, &QTimer::timeout, &driveClient,
                     &GoogleDriveClient::getAboutInfo);
    QObject::connect(&authManager, &GoogleAuthManager::authenticated, &storageRefreshTimer,
                     [&storageRefreshTimer, &driveClient]() {
                         storageRefreshTimer.start();
                         // Fetch once immediately
                         QTimer::singleShot(2000, &driveClient, &GoogleDriveClient::getAboutInfo);
                     });
    QObject::connect(&authManager, &GoogleAuthManager::loggedOut, &storageRefreshTimer,
                     &QTimer::stop);

    // Connect change processor errors to notification manager
    QObject::connect(&changeProcessor, &ChangeProcessor::error, &notificationManager,
                     [&notificationManager](const QString& error) {
                         notificationManager.showError("Sync Error", error);
                     });

    // Connect conflict detection to notification
    QObject::connect(&changeProcessor, &ChangeProcessor::conflictDetected, &notificationManager,
                     [&notificationManager](const ConflictInfo& info) {
                         QString fileName = QFileInfo(info.localPath).fileName();
                         notificationManager.showConflict(fileName);
                     });

    // Connect conflict detection to system tray for warning icon
    QObject::connect(&changeProcessor, &ChangeProcessor::conflictDetected, &trayManager,
                     [&trayManager](const ConflictInfo&) { trayManager.setHasConflicts(true); });
    QObject::connect(&changeProcessor, &ChangeProcessor::conflictResolved, &trayManager,
                     [&trayManager](const QString&, ConflictResolutionStrategy) {
                         // TODO: Check if all conflicts are resolved before clearing
                         // For now, assume the user can clear it; this could be improved
                     });

    // Connect sync action thread errors to notification manager
    QObject::connect(&syncActionThread, &SyncActionThread::error, &notificationManager,
                     [&notificationManager](const QString& error) {
                         notificationManager.showError("Sync Action Error", error);
                     });

    // Connect progress bar to sync action thread
    QObject::connect(
        &syncActionThread, &SyncActionThread::actionProgress, &mainWindow,
        [&mainWindow](const SyncActionItem&, qint64 bytesProcessed, qint64 bytesTotal) {
            mainWindow.updateSyncProgress(bytesProcessed, bytesTotal);
        });

    QObject::connect(&syncActionThread, &SyncActionThread::tokenRefreshRequested, &authManager,
                     &GoogleAuthManager::refreshTokens);

    bool refreshInFlight = false;
    qint64 lastRefreshAttemptMs = 0;
    constexpr qint64 AUTH_REFRESH_COOLDOWN_MS = 10000;

    QObject::connect(
        &driveClient, &GoogleDriveClient::authenticationFailure, &app,
        [&authManager, &refreshInFlight, &lastRefreshAttemptMs](
            const QString& operation, int httpStatus, const QString& errorMsg) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (refreshInFlight || (nowMs - lastRefreshAttemptMs) < AUTH_REFRESH_COOLDOWN_MS) {
                qInfo() << "Auth refresh suppressed (in-flight/cooldown) op=" << operation
                        << "status=" << httpStatus;
                return;
            }

            if (authManager.refreshToken().isEmpty()) {
                qWarning() << "Auth failure without refresh token; skipping auto-refresh";
                return;
            }

            qWarning() << "Auth failure detected from operation:" << operation
                       << "status:" << httpStatus << "error:" << errorMsg
                       << "-> requesting token refresh";
            refreshInFlight = true;
            lastRefreshAttemptMs = nowMs;
            authManager.refreshTokens();
        });

    QObject::connect(&authManager, &GoogleAuthManager::tokenRefreshed, &app,
                     [&refreshInFlight]() { refreshInFlight = false; });

    QObject::connect(&authManager, &GoogleAuthManager::tokenRefreshError, &app,
                     [&refreshInFlight, &notificationManager, &mainWindow](const QString& error) {
                         refreshInFlight = false;
                         mainWindow.addRecentActivity("Token refresh error: " + error);
                         notificationManager.showWarning("Authentication Warning", error);
                     });

    QObject::connect(
        &authManager, &GoogleAuthManager::authExpired, &app,
        [&refreshInFlight, &localWatcher, &remoteWatcher, &changeProcessor, &syncActionThread,
         &fullSync, &fullSyncLocalTimer, &trayManager, &mainWindow, &notificationManager,
         &fuseDriver](const QString& reason) {
            refreshInFlight = false;
            fullSync.cancel();
            fullSyncLocalTimer.stop();
            stopFuseComponent(&fuseDriver);
            stopSyncComponents(&localWatcher, &remoteWatcher, &changeProcessor, &syncActionThread);

            mainWindow.setAuthExpired(reason);
            trayManager.updateAuthState(false);
            trayManager.updateSyncStatus("Authentication expired");
            trayManager.showNotification("Session Expired",
                                         "Google Drive session expired. Sign in again.",
                                         QSystemTrayIcon::Warning);
            notificationManager.showWarning(
                "Authentication Expired",
                "Session expired. Re-authentication is required to resume sync.");
        });

    // Auto-login if tokens are available - check after connections are established
    if (tokenStorage.hasValidTokens()) {
        qInfo() << "Found stored tokens, checking validity...";

        if (tokenStorage.isTokenExpired()) {
            // Tokens expired - refresh them first
            qInfo() << "Tokens expired, refreshing...";
            // Start-up after successful refresh is handled by the authenticated signal path.
            authManager.refreshTokens();
        } else {
            // Tokens are valid and not expired - start sync components directly
            qInfo() << "Valid tokens found (authenticated=" << authManager.isAuthenticated() << ")";
            qInfo() << "Starting sync components immediately";

            // Use QTimer::singleShot to ensure event loop is running
            QTimer::singleShot(100, &app,
                               [&localWatcher, &remoteWatcher, &changeProcessor, &syncActionThread,
                                &fullSync, &fullSyncLocalTimer, &fuseDriver, fuseEnabled,
                                mirrorEnabled, &syncFolder, &trayManager]() {
                                   if (mirrorEnabled) {
                                       startSyncComponents(&localWatcher, &remoteWatcher,
                                                           &changeProcessor, &syncActionThread);
                                       fullSyncLocalTimer.start();
                                       // Trigger full sync after starting components
                                       QTimer::singleShot(500, &fullSync, &FullSync::fullSync);
                                   }
                                   if (fuseEnabled) {
                                       startFuseComponent(&fuseDriver, syncFolder);
                                   }
                                   trayManager.updateAuthState(true);
                               });
        }
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     [&fuseDriver]() { stopFuseComponent(&fuseDriver); });

    // Show main window on first run or if not logged in
    if (!tokenStorage.hasValidTokens()) {
        mainWindow.show();
    }

    qInfo() << "Via started successfully";

    return app.exec();
}
