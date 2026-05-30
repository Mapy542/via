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
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QNetworkInformation>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <memory>

#include "ViaVersion.h"
#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "auth/TokenStorage.h"
#include "auth/WakeRefreshNotificationGate.h"
#include "fuse/FileCache.h"
#include "fuse/FuseDriver.h"
#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/FullSync.h"
#include "sync/LocalChangeWatcher.h"
#include "sync/MirrorSyncRuntime.h"
#include "sync/RemoteChangeWatcher.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncActionThread.h"
#include "sync/SyncDatabase.h"
#include "ui/ConflictDialog.h"
#include "ui/MainWindow.h"
#include "ui/SystemTrayManager.h"
#include "ui/UiStatusCoordinator.h"
#include "utils/AutostartManager.h"
#include "utils/CacheMaintenance.h"
#include "utils/LogManager.h"
#include "utils/NativeDocShortcutHandler.h"
#include "utils/NotificationManager.h"
#include "utils/PathUtils.h"
#include "utils/PowerProfileMonitor.h"
#include "utils/StartupMaintenance.h"
#include "utils/SuspendMonitor.h"
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

int handleNativeDocShortcutLaunch(const QStringList& arguments) {
    int shortcutArgs = 0;
    int openedCount = 0;
    QStringList failures;

    for (int index = 1; index < arguments.size(); ++index) {
        const QString arg = arguments.at(index);
        if (arg.startsWith(QLatin1Char('-'))) {
            continue;
        }

        QString absolutePath;
        if (!isNativeDocShortcutArgument(arg, &absolutePath)) {
            continue;
        }

        ++shortcutArgs;

        const QFileInfo fileInfo(absolutePath);
        if (!fileInfo.exists() || !fileInfo.isFile()) {
            failures << QStringLiteral("%1: shortcut file is unavailable")
                            .arg(QFileInfo(absolutePath).fileName());
            continue;
        }

        QString parseError;
        const auto shortcut = parseNativeDocShortcutFile(absolutePath, &parseError);
        if (!shortcut.has_value()) {
            failures << QStringLiteral("%1: %2").arg(fileInfo.fileName(), parseError);
            continue;
        }

        if (!QDesktopServices::openUrl(shortcut->url)) {
            failures << QStringLiteral("%1: failed to open %2")
                            .arg(fileInfo.fileName(), shortcut->url.toString());
            continue;
        }

        ++openedCount;
    }

    if (shortcutArgs == 0) {
        return -1;
    }

    if (!failures.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Unable to open native document"),
                             failures.join(QLatin1Char('\n')));
    }

    return openedCount > 0 ? 0 : 1;
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

constexpr int kCurrentNativeDocRepresentationEpoch = 1;
constexpr int kCurrentFuseRepresentationEpoch = kCurrentNativeDocRepresentationEpoch;
constexpr int kCurrentMirrorRepresentationEpoch = kCurrentNativeDocRepresentationEpoch;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Set application metadata
    app.setApplicationName("Via");
    app.setApplicationDisplayName("Via");
    app.setApplicationVersion(QStringLiteral(VIA_APP_VERSION));
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

    const int nativeDocOpenExit = handleNativeDocShortcutLaunch(QCoreApplication::arguments());
    if (nativeDocOpenExit >= 0) {
        return nativeDocOpenExit;
    }

    // Initialize token storage
    TokenStorage tokenStorage;

    // Initialize Google Auth Manager
    GoogleAuthManager authManager(&tokenStorage);

    // Initialize Google Drive API clients.
    // UI/FUSE traffic stays on the main-thread client while mirror sync uses a
    // dedicated sibling client that MirrorSyncRuntime moves onto the mirror
    // worker thread.
    GoogleDriveClient driveClient(&authManager);
    GoogleDriveClient mirrorDriveClient(&authManager);

    // Initialize sync database
    SyncDatabase syncDatabase;
    bool forceImmediateMirrorRebuild = false;
    const auto initializeSyncDatabase = [&syncDatabase, &forceImmediateMirrorRebuild]() {
        if (syncDatabase.initialize()) {
            return true;
        }

        const StartupMaintenance::SyncResetDecision resetDecision =
            StartupMaintenance::classifySyncReset(syncDatabase.lastSchemaCompatibility());
        if (resetDecision.unsupportedFutureSchema) {
            QMessageBox::critical(nullptr, QStringLiteral("Database Error"),
                                  QStringLiteral("The sync database was created by a newer, "
                                                 "incompatible Via build.\n"
                                                 "Install a newer version or remove the local "
                                                 "sync database manually."));
            return false;
        }

        if (!resetDecision.requiresReset) {
            QMessageBox::critical(nullptr, QStringLiteral("Database Error"),
                                  QStringLiteral("Failed to initialize the sync database.\n"
                                                 "Please check permissions and try again."));
            return false;
        }

        QMessageBox prompt;
        prompt.setIcon(QMessageBox::Warning);
        prompt.setWindowTitle(QStringLiteral("Rebuild sync state"));
        prompt.setText(resetDecision.requiresExplicitDiscard
                           ? QStringLiteral("Via found an incompatible sync database with "
                                            "pending local uploads.")
                           : QStringLiteral("Via found an incompatible sync database."));
        prompt.setInformativeText(
            resetDecision.requiresExplicitDiscard
                ? QStringLiteral("Rebuilding will discard pending upload state that has not yet "
                                 "been confirmed on Google Drive.\n\n"
                                 "The local sync folder will be preserved. Choose Discard And "
                                 "Rebuild to recreate local sync metadata, or Exit to stop now.")
                : QStringLiteral("Via can recreate its local sync metadata and rebuild it from "
                                 "disk plus Google Drive.\n\n"
                                 "The local sync folder will be preserved."));
        QPushButton* rebuildButton = prompt.addButton(resetDecision.requiresExplicitDiscard
                                                          ? QStringLiteral("Discard And Rebuild")
                                                          : QStringLiteral("Rebuild"),
                                                      QMessageBox::AcceptRole);
        prompt.addButton(QStringLiteral("Exit"), QMessageBox::RejectRole);
        prompt.setDefaultButton(rebuildButton);
        prompt.exec();

        if (prompt.clickedButton() != rebuildButton) {
            return false;
        }

        if (!syncDatabase.recreateCurrentSchema()) {
            QMessageBox::critical(nullptr, QStringLiteral("Database Error"),
                                  QStringLiteral("Failed to rebuild the local sync database."));
            return false;
        }

        forceImmediateMirrorRebuild = resetDecision.requestFullSyncAfterReset;
        qWarning() << "Sync database recreated after incompatible schema detection";
        return true;
    };

    if (!initializeSyncDatabase()) {
        return 1;
    }

    // Initialize notification manager
    NotificationManager notificationManager;

    RuntimePauseController pauseController;

    // Check for updates (runs asynchronously, shows dialog if update found)
    UpdateChecker updateChecker;
    updateChecker.checkForUpdates(/* silent = */ true);

    QSettings settings;
    pauseController.setAutoPauseEnabled(settings.value("advanced/autoPauseEnabled", true).toBool());

    // Read sync system mode: "mirror-only", "fuse-only", or "both"
    // Migrate from legacy "advanced/enableFuse" boolean if needed
    QString syncSystemMode = settings.value("advanced/syncSystem", "").toString();
    if (syncSystemMode.isEmpty()) {
        bool legacyFuse = settings.value("advanced/enableFuse", false).toBool();
        syncSystemMode = legacyFuse ? "both" : "mirror-only";
    } else if (syncSystemMode != QLatin1String("mirror-only") &&
               syncSystemMode != QLatin1String("fuse-only") &&
               syncSystemMode != QLatin1String("both") && syncSystemMode != QLatin1String("none")) {
        syncSystemMode = QStringLiteral("mirror-only");
    }
    const bool fuseEnabled = (syncSystemMode == "fuse-only" || syncSystemMode == "both");
    const bool mirrorEnabled = (syncSystemMode == "mirror-only" || syncSystemMode == "both");
    const bool syncControlsEnabled = mirrorEnabled || fuseEnabled;
    qInfo() << "Sync system mode:" << syncSystemMode << "(mirror:" << mirrorEnabled
            << "fuse:" << fuseEnabled << ")";

    const QString fuseMountPoint =
        settings.value("advanced/fuseMountPoint", QDir::homePath() + "/GoogleDriveFuse").toString();
    const qint64 cacheSizeMb = settings.value("advanced/cacheSize", 5000).toLongLong();

    // Seed mode-tracking keys so restart-time maintenance can detect the first
    // user-visible change even if this is the first launch after upgrade.
    const QString configuredNativeDocMode =
        settings.value("advanced/nativeDocMode", "hide").toString();
    bool wroteNativeDocModeSeed = false;
    if (!settings.contains("advanced/previousNativeDocMode")) {
        settings.setValue("advanced/previousNativeDocMode", configuredNativeDocMode);
        wroteNativeDocModeSeed = true;
    }
    if (!settings.contains("advanced/previousMirrorNativeDocMode")) {
        settings.setValue("advanced/previousMirrorNativeDocMode", configuredNativeDocMode);
        wroteNativeDocModeSeed = true;
    }
    if (wroteNativeDocModeSeed) {
        settings.sync();
    }

    FuseDriver fuseDriver(&driveClient, &syncDatabase);
    fuseDriver.setPauseController(&pauseController);
    if (!fuseMountPoint.isEmpty()) {
        fuseDriver.setMountPoint(fuseMountPoint);
    }
    fuseDriver.setMaxCacheSizeBytes(cacheSizeMb * 1024LL * 1024LL);

    // ── Startup-authoritative FUSE cache maintenance ────────────────
    // Detect mode changes regardless of how the app was restarted
    // (in-app Restart Now, manual quit/relaunch, crash, etc.). Settings-
    // initiated requests may also set pending flags, but startup remains
    // authoritative and retries until a purge succeeds.
    {
        const QString currentMode = settings.value("advanced/nativeDocMode", "hide").toString();
        const QString previousMode =
            settings.value("advanced/previousNativeDocMode", "hide").toString();
        const bool pendingRepresentationReset =
            settings.value("advanced/pendingFuseRepresentationReset", false).toBool();
        const bool pendingCachePurge = settings.value("advanced/pendingCachePurge", false).toBool();
        const int storedRepresentationEpoch =
            settings
                .value("advanced/lastAppliedFuseRepresentationEpoch",
                       kCurrentFuseRepresentationEpoch)
                .toInt();
        const bool modeChanged = (currentMode != previousMode);
        const bool representationEpochChanged =
            kCurrentFuseRepresentationEpoch > storedRepresentationEpoch;
        const StartupMaintenance::FuseMaintenanceInputs maintenanceInputs{
            .currentNativeDocMode = currentMode,
            .previousNativeDocMode = previousMode,
            .pendingRepresentationReset = pendingRepresentationReset,
            .pendingCachePurge = pendingCachePurge,
            .storedRepresentationEpoch = storedRepresentationEpoch,
            .currentRepresentationEpoch = kCurrentFuseRepresentationEpoch,
        };

        if (StartupMaintenance::shouldPurgeFuseRepresentationCache(maintenanceInputs)) {
            if (representationEpochChanged) {
                qInfo() << "FUSE representation epoch changed from" << storedRepresentationEpoch
                        << "to" << kCurrentFuseRepresentationEpoch
                        << "- purging FUSE representation caches";
            } else if (modeChanged) {
                qInfo() << "Native-doc mode changed from" << previousMode << "to" << currentMode
                        << "- purging FUSE representation caches";
            } else if (pendingCachePurge) {
                qInfo() << "Pending cache purge flag detected - purging FUSE caches";
            } else {
                qInfo() << "Pending FUSE representation reset flag detected - purging caches";
            }

            const QString cachePath =
                QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            const bool purgeOk =
                CacheMaintenance::purgeFuseRepresentationCache(cachePath, syncDatabase);

            // Only advance the previous-mode marker and clear pending flags
            // when the full purge succeeded. If it failed, the mismatch or
            // pending flag will trigger a retry on the next launch.
            if (purgeOk) {
                settings.setValue("advanced/previousNativeDocMode", currentMode);
                settings.setValue("advanced/lastAppliedFuseRepresentationEpoch",
                                  kCurrentFuseRepresentationEpoch);
                settings.remove("advanced/pendingFuseRepresentationReset");
                settings.remove("advanced/pendingCachePurge");
                settings.sync();
            } else {
                qWarning() << "FUSE cache purge failed - will retry on next launch";
            }
        } else if (!settings.contains("advanced/lastAppliedFuseRepresentationEpoch")) {
            settings.setValue("advanced/lastAppliedFuseRepresentationEpoch",
                              kCurrentFuseRepresentationEpoch);
            settings.sync();
        }
    }

    // Initialize sync components (Change Queue, Sync Action Queue, Watchers, Processor)
    ChangeQueue changeQueue;
    SyncActionQueue syncActionQueue;

    // Initialize local change watcher
    LocalChangeWatcher localWatcher(&changeQueue);
    QString syncFolder =
        settings.value("sync/folder", QDir::homePath() + "/GoogleDrive").toString();

    // Startup-authoritative mirror native-doc maintenance. When the serving
    // mode changes, purge only native-doc mirror artifacts and stale path
    // mappings, then request an immediate full sync so the new representation
    // is materialized from Drive.
    {
        const QString currentMode = settings.value("advanced/nativeDocMode", "hide").toString();
        const QString previousMode =
            settings.value("advanced/previousMirrorNativeDocMode", currentMode).toString();
        const bool pendingRepresentationReset =
            settings.value("advanced/pendingMirrorRepresentationReset", false).toBool();
        const int storedRepresentationEpoch =
            settings.value("advanced/lastAppliedMirrorRepresentationEpoch", 0).toInt();
        const bool modeChanged = (currentMode != previousMode);
        const bool representationEpochChanged =
            kCurrentMirrorRepresentationEpoch > storedRepresentationEpoch;
        const StartupMaintenance::MirrorMaintenanceInputs maintenanceInputs{
            .currentNativeDocMode = currentMode,
            .previousNativeDocMode = previousMode,
            .pendingRepresentationReset = pendingRepresentationReset,
            .storedRepresentationEpoch = storedRepresentationEpoch,
            .currentRepresentationEpoch = kCurrentMirrorRepresentationEpoch,
        };

        if (StartupMaintenance::shouldRebuildMirrorRepresentation(maintenanceInputs)) {
            if (representationEpochChanged) {
                qInfo() << "Mirror native-doc representation epoch changed from"
                        << storedRepresentationEpoch << "to" << kCurrentMirrorRepresentationEpoch
                        << "- rebuilding local native-doc artifacts";
            } else if (modeChanged) {
                qInfo() << "Native-doc mode changed from" << previousMode << "to" << currentMode
                        << "- rebuilding local native-doc artifacts";
            } else {
                qInfo() << "Pending mirror representation reset flag detected - rebuilding local "
                           "native-doc artifacts";
            }

            StartupMaintenance::MirrorRepresentationRebuildStats rebuildStats;
            const bool rebuildOk = StartupMaintenance::purgeMirrorNativeDocArtifacts(
                syncFolder, syncDatabase, &rebuildStats);
            if (rebuildOk) {
                qInfo() << "Mirror native-doc rebuild removed" << rebuildStats.removedArtifactCount
                        << "artifact(s) and cleared" << rebuildStats.clearedMappingCount
                        << "mapping(s)";
                settings.setValue("advanced/previousMirrorNativeDocMode", currentMode);
                settings.setValue("advanced/lastAppliedMirrorRepresentationEpoch",
                                  kCurrentMirrorRepresentationEpoch);
                settings.remove("advanced/pendingMirrorRepresentationReset");
                settings.sync();
                forceImmediateMirrorRebuild = true;
            } else {
                qWarning() << "Mirror native-doc rebuild failed - will retry on next launch";
            }
        } else if (!settings.contains("advanced/lastAppliedMirrorRepresentationEpoch")) {
            settings.setValue("advanced/lastAppliedMirrorRepresentationEpoch",
                              kCurrentMirrorRepresentationEpoch);
            settings.sync();
        }
    }

    // Initialize remote change watcher
    RemoteChangeWatcher remoteWatcher(&changeQueue, &mirrorDriveClient, &syncDatabase);

    // Initialize change processor/conflict resolver
    ChangeProcessor changeProcessor(&changeQueue, &syncActionQueue, &syncDatabase,
                                    &mirrorDriveClient);

    QObject::connect(&fuseDriver, &FuseDriver::mountError, &notificationManager,
                     [&notificationManager](const QString& error) {
                         notificationManager.showError("FUSE Mount Error", error);
                     });
    // Initialize sync action thread (executes sync actions from queue)
    SyncActionThread syncActionThread(&syncActionQueue, &syncDatabase, &mirrorDriveClient,
                                      &changeProcessor, &localWatcher);

    // Initialize full sync handler
    FullSync fullSync(&changeQueue, &syncDatabase, &mirrorDriveClient, &changeProcessor);

    MirrorSyncRuntime mirrorSyncRuntime(&localWatcher, &remoteWatcher, &changeProcessor,
                                        &syncActionQueue, &syncActionThread, &fullSync,
                                        &mirrorDriveClient, &syncDatabase, &app);
    mirrorSyncRuntime.setSyncFolder(syncFolder);

    // Load stored change token if available
    QString storedToken = syncDatabase.getChangeToken();
    if (!storedToken.isEmpty()) {
        mirrorSyncRuntime.setChangeToken(storedToken);
    }

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
        mirrorSyncRuntime.setConflictResolutionStrategy(strategy);
    }

    const auto queueMirrorPause = [&mirrorSyncRuntime]() {
        QMetaObject::invokeMethod(&mirrorSyncRuntime, "pause", Qt::QueuedConnection);
    };
    const auto queueMirrorResume = [&mirrorSyncRuntime]() {
        QMetaObject::invokeMethod(&mirrorSyncRuntime, "resume", Qt::QueuedConnection);
    };
    const auto queueMirrorStartAndScheduleInitialSync = [&mirrorSyncRuntime](int delayMs = 500) {
        QMetaObject::invokeMethod(&mirrorSyncRuntime, "startAndScheduleInitialSync",
                                  Qt::QueuedConnection, Q_ARG(int, delayMs));
    };
    const auto queueMirrorStartupSync = [&pauseController, mirrorEnabled,
                                         &forceImmediateMirrorRebuild,
                                         queueMirrorStartAndScheduleInitialSync]() {
        if (!mirrorEnabled || pauseController.isEffectivelyPaused()) {
            return;
        }

        queueMirrorStartAndScheduleInitialSync(forceImmediateMirrorRebuild ? 0 : 500);
        forceImmediateMirrorRebuild = false;
    };
    const auto queueMirrorRestartAfterWake = [&mirrorSyncRuntime](int fullSyncDelayMs = 2000) {
        QMetaObject::invokeMethod(&mirrorSyncRuntime, "restartAfterWake", Qt::QueuedConnection,
                                  Q_ARG(int, fullSyncDelayMs));
    };
    const auto queueMirrorRequestFullSync = [&mirrorSyncRuntime]() {
        QMetaObject::invokeMethod(&mirrorSyncRuntime, "requestFullSync", Qt::QueuedConnection,
                                  Q_ARG(int, 0));
    };

    // Connect runtime to save change tokens to database
    if (mirrorEnabled) {
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::changeTokenUpdated, &syncDatabase,
                         &SyncDatabase::setChangeToken);
    }

    // Initialize shared UI status coordination and UI surfaces.
    UiStatusCoordinator statusCoordinator(&authManager, mirrorEnabled, &pauseController);
    statusCoordinator.setFuseEnabled(fuseEnabled);
    if (mirrorEnabled) {
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::pendingActionsChanged,
                         &statusCoordinator, &UiStatusCoordinator::updatePendingActions);
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::processorStateChanged,
                         &statusCoordinator, &UiStatusCoordinator::updateMirrorProcessorState);
        statusCoordinator.updatePendingActions(mirrorSyncRuntime.pendingActionCount());
        statusCoordinator.updateMirrorProcessorState(mirrorSyncRuntime.processorState());
        statusCoordinator.setHasConflicts(mirrorSyncRuntime.unresolvedConflictCount() > 0);
    }

    SystemTrayManager trayManager(&authManager, &pauseController, &statusCoordinator,
                                  syncControlsEnabled, mirrorEnabled);
    trayManager.show();
    notificationManager.setTrayIcon(trayManager.trayIcon());
    QObject::connect(&notificationManager, &NotificationManager::notificationShown, &trayManager,
                     [&trayManager](const QString& title, const QString& message, bool) {
                         trayManager.recordNotification(title, message);
                     });

    // Surface export failures through the desktop notification backend so
    // they can remain visible/persistent in supported notification centers.
    QObject::connect(
        &driveClient, &GoogleDriveClient::errorDetailed, &notificationManager,
        [&notificationManager](const QString& operation, const QString& errorMsg, int httpStatus,
                               const QString& /*fileId*/, const QString& /*localPath*/) {
            if (!operation.startsWith(QLatin1String("exportFile")))
                return;
            const QString detail = nativeDocExportFailureMessage(errorMsg, httpStatus);
            notificationManager.showPersistentError(QStringLiteral("Export Failed"), detail);
        });
    QObject::connect(&fuseDriver, &FuseDriver::nativeDocExportFailed, &notificationManager,
                     [&notificationManager](const QString& path, const QString& error) {
                         notificationManager.showPersistentError(
                             QStringLiteral("Export Failed"),
                             QStringLiteral("%1\n%2").arg(path, error));
                     });
    QObject::connect(&fuseDriver, &FuseDriver::fuseUploadFailed, &notificationManager,
                     [&notificationManager](const QString& path, const QString& error) {
                         notificationManager.showError(QStringLiteral("Upload Failed"),
                                                       QStringLiteral("%1\n%2").arg(path, error));
                     });

    // Initialize main window
    // When mirror sync is disabled, pass nullptr for sync components so UI disables sync actions
    MainWindow mainWindow(&authManager, &driveClient, mirrorEnabled ? &mirrorSyncRuntime : nullptr,
                          &fuseDriver, &pauseController, &statusCoordinator, &notificationManager,
                          syncControlsEnabled, mirrorEnabled);

    if (mirrorEnabled) {
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::processorError, &mainWindow,
                         [&mainWindow](const QString& error) {
                             mainWindow.addRecentActivity("Error: " + error);
                         });
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::conflictDetected, &mainWindow,
                         [&mainWindow](const ConflictInfo& info) {
                             mainWindow.addRecentActivity("Conflict: " + info.localPath);
                         });
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::conflictResolved, &mainWindow,
                         [&mainWindow](const QString& localPath, ConflictResolutionStrategy) {
                             mainWindow.addRecentActivity("Conflict resolved: " + localPath);
                         });
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::changeProcessed, &mainWindow,
                         [&mainWindow](const QString& localPath) {
                             mainWindow.addRecentActivity("Processed: " + localPath);
                         });
        QObject::connect(
            &mirrorSyncRuntime, &MirrorSyncRuntime::fullSyncProgressUpdated, &mainWindow,
            [&mainWindow](const QString& phase, int current, int total) {
                Q_UNUSED(total);
                mainWindow.addRecentActivity(QString("%1 (%2 files)").arg(phase).arg(current));
            });
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::fullSyncCompleted, &mainWindow,
                         [&mainWindow](int localCount, int remoteCount) {
                             mainWindow.addRecentActivity(
                                 QString("Full sync complete: %1 local, %2 remote files")
                                     .arg(localCount)
                                     .arg(remoteCount));
                         });
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::fullSyncError, &mainWindow,
                         [&mainWindow](const QString& error) {
                             mainWindow.addRecentActivity("Full sync error: " + error);
                         });
    }

    // Initialize auto-pause sources that can safely feed the shared runtime policy.
    if (QNetworkInformation::loadDefaultBackend()) {
        QNetworkInformation* netInfo = QNetworkInformation::instance();
        if (netInfo) {
            qInfo() << "Network backend loaded:" << netInfo->backendName();
            QObject::connect(netInfo, &QNetworkInformation::reachabilityChanged, &app,
                             [&pauseController](QNetworkInformation::Reachability reachability) {
                                 pauseController.setAutoPauseReasonActive(
                                     RuntimePauseController::AutoPauseReason::Offline,
                                     reachability != QNetworkInformation::Reachability::Online);
                             });

            pauseController.setAutoPauseReasonActive(
                RuntimePauseController::AutoPauseReason::Offline,
                netInfo->reachability() != QNetworkInformation::Reachability::Online);

            if (netInfo->supports(QNetworkInformation::Feature::Metered)) {
                QObject::connect(netInfo, &QNetworkInformation::isMeteredChanged, &app,
                                 [&pauseController](bool isMetered) {
                                     pauseController.setAutoPauseReasonActive(
                                         RuntimePauseController::AutoPauseReason::MeteredNetwork,
                                         isMetered);
                                 });

                pauseController.setAutoPauseReasonActive(
                    RuntimePauseController::AutoPauseReason::MeteredNetwork, netInfo->isMetered());
            } else {
                qInfo() << "QNetworkInformation: metered state unsupported by backend";
            }
        }
    } else {
        qInfo() << "QNetworkInformation: no backend available, skipping connectivity monitoring";
    }

    PowerProfileMonitor powerProfileMonitor(&app);
    QObject::connect(&powerProfileMonitor, &PowerProfileMonitor::powerSaverChanged, &app,
                     [&pauseController](bool powerSaverActive) {
                         pauseController.setAutoPauseReasonActive(
                             RuntimePauseController::AutoPauseReason::PowerSaver, powerSaverActive);
                     });
    pauseController.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::PowerSaver,
                                             powerProfileMonitor.isPowerSaverActive());

    const auto pauseState =
        std::make_shared<RuntimePauseController::Snapshot>(pauseController.snapshot());
    const auto lastBlockedNoticeMs = std::make_shared<qint64>(0);

    QObject::connect(
        &pauseController, &RuntimePauseController::stateChanged, &app,
        [&pauseController, &notificationManager, &fuseDriver, &authManager, mirrorEnabled,
         fuseEnabled, pauseState, queueMirrorPause, queueMirrorResume]() {
            const RuntimePauseController::Snapshot previous = *pauseState;
            const RuntimePauseController::Snapshot current = pauseController.snapshot();
            *pauseState = current;

            if (previous.effectivePause == current.effectivePause) {
                return;
            }

            if (current.effectivePause) {
                qInfo() << "Runtime pause engaged:" << pauseController.effectiveStatusText();
                if (mirrorEnabled) {
                    queueMirrorPause();
                }
                if (fuseEnabled) {
                    fuseDriver.pauseSync();
                }
                if (authManager.isAuthenticated()) {
                    notificationManager.showWarning(pauseController.pauseNotificationTitle(),
                                                    pauseController.pauseNotificationMessage());
                }
                return;
            }

            qInfo() << "Runtime pause cleared";
            if (mirrorEnabled && authManager.isAuthenticated()) {
                queueMirrorResume();
            }
            if (fuseEnabled) {
                fuseDriver.resumeSync();
            }
            if (authManager.isAuthenticated()) {
                notificationManager.showInfo(QStringLiteral("Sync Resumed"),
                                             pauseController.resumeNotificationMessage());
            }
        });

    // Detect system suspend/resume and recover after wake
    SuspendMonitor suspendMonitor(&app);
    WakeRefreshNotificationGate wakeRefreshNotificationGate;

    QObject::connect(
        &suspendMonitor, &SuspendMonitor::resumed, &app,
        [&authManager, &fuseDriver, &trayManager, &notificationManager, &statusCoordinator,
         &wakeRefreshNotificationGate, &pauseController, mirrorEnabled, fuseEnabled,
         queueMirrorRestartAfterWake]() {
            qInfo() << "Resume handler: refreshing auth and restarting components";
            statusCoordinator.updateMirrorStatus("Recovering from sleep...");

            // 1. Force a token refresh — connections are likely stale and the
            //    access token may have expired while the machine was asleep.
            if (!authManager.refreshToken().isEmpty()) {
                wakeRefreshNotificationGate.beginWakeRefreshAttempt();
                authManager.refreshTokens();
            }

            // 2. After the refresh completes (or fails), restart workers that
            //    may be stuck on dead connections.
            auto* resumeConn = new QMetaObject::Connection;
            auto doRestart =
                [&, resumeConn]() {
                    QObject::disconnect(*resumeConn);
                    delete resumeConn;

                    if (!authManager.isAuthenticated()) {
                        qWarning()
                            << "Resume handler: not authenticated after refresh, skipping restart";
                        return;
                    }

                    // 3. Restart mirror sync components — they may be waiting on
                    //    dead network sockets inside polling loops.
                    if (mirrorEnabled && !pauseController.isEffectivelyPaused()) {
                        queueMirrorRestartAfterWake();
                    }

                    // 4. Kick FUSE background workers.  Stopping and starting them
                    //    resets their QTimers and clears any stalled API calls.
                    if (fuseEnabled && fuseDriver.isMounted() &&
                        !pauseController.isEffectivelyPaused()) {
                        fuseDriver.refreshMetadata();
                    }

                    statusCoordinator.updateMirrorStatus("Syncing...");
                    qInfo() << "Resume handler: recovery complete";
                };

            // Connect to both success and failure so we always resume.
            *resumeConn = QObject::connect(&authManager, &GoogleAuthManager::tokenRefreshed,
                                           &authManager, doRestart, Qt::SingleShotConnection);
            // Also handle the case where refresh fails — still restart workers
            // so cached operations can proceed.
            QObject::connect(&authManager, &GoogleAuthManager::tokenRefreshError, &authManager,
                             doRestart, Qt::SingleShotConnection);
            // If there's no refresh token, trigger restart immediately.
            if (authManager.refreshToken().isEmpty()) {
                doRestart();
            }
        });

    // Connect signals for application-wide coordination

    // When authenticated, start sync components based on configured mode
    QObject::connect(&authManager, &GoogleAuthManager::authenticated, &app,
                     [&fuseDriver, &pauseController, fuseEnabled, mirrorEnabled, &syncFolder,
                      queueMirrorStartupSync]() {
                         Q_UNUSED(mirrorEnabled);
                         Q_UNUSED(pauseController);
                         queueMirrorStartupSync();
                         if (fuseEnabled) {
                             startFuseComponent(&fuseDriver, syncFolder);
                             if (pauseController.isEffectivelyPaused()) {
                                 fuseDriver.pauseSync();
                             }
                         }
                     });

    // When logged out, stop sync components and purge session state
    QObject::connect(
        &authManager, &GoogleAuthManager::loggedOut, &app,
        [&mirrorSyncRuntime, &fuseDriver, mirrorEnabled, fuseEnabled, &changeQueue,
         &syncActionQueue, &syncDatabase, &syncFolder]() {
            // --- 1. Cancel / stop running components ---
            if (mirrorEnabled) {
                mirrorSyncRuntime.cancelAndStop();
            }
            if (fuseEnabled) {
                stopFuseComponent(&fuseDriver);
            }

            // --- 2. Drain in-memory queues ---
            changeQueue.clear();
            syncActionQueue.clear();

            // --- 3. Clear per-component in-memory state ---
            if (mirrorEnabled) {
                mirrorSyncRuntime.clearSessionState();
            }

            // --- 4. Wipe the sync database ---
            syncDatabase.clearAllData();

            // --- 5. Prompt user about local sync folder ---
            if (!syncFolder.isEmpty() && QDir(syncFolder).exists()) {
                auto answer = QMessageBox::question(
                    nullptr, QStringLiteral("Remove local files?"),
                    QStringLiteral("You have signed out.\n\n"
                                   "Do you want to delete the local sync folder and all its "
                                   "contents?\n\n%1")
                        .arg(syncFolder),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

                if (answer == QMessageBox::Yes) {
                    const PathUtils::RecursiveRootRemovalDecision decision =
                        PathUtils::classifyRecursiveRootRemoval(syncFolder);

                    if (decision.action ==
                        PathUtils::RecursiveRootRemovalAction::RemoveRecursively) {
                        if (QDir(decision.absolutePath).removeRecursively()) {
                            qInfo()
                                << "Local sync folder purged on sign-out:" << decision.absolutePath;
                        } else {
                            qWarning() << "Failed to purge local sync folder on sign-out:"
                                       << decision.absolutePath;
                        }
                    } else if (decision.action ==
                               PathUtils::RecursiveRootRemovalAction::RemoveSymlinkOnly) {
                        if (QFile::remove(decision.absolutePath)) {
                            qInfo() << "Local sync-folder symlink removed on sign-out:"
                                    << decision.absolutePath << "->" << decision.canonicalPath;
                        } else {
                            qWarning() << "Failed to remove sync-folder symlink on sign-out:"
                                       << decision.absolutePath;
                        }
                    } else {
                        qWarning() << "Refusing to delete dangerous path:" << decision.absolutePath
                                   << "(depth=" << decision.depth
                                   << "canonical=" << decision.canonicalPath << ")";
                    }
                }
            }

            qInfo() << "Account sign-out cleanup complete";
        });

    // Connect tray manager to main window
    QObject::connect(&trayManager, &SystemTrayManager::showWindowRequested, &mainWindow,
                     &MainWindow::showAndActivate);
    QObject::connect(&trayManager, &SystemTrayManager::quitRequested, &app, &QApplication::quit);

    // Handle sign-out requests from MainWindow.  Check for pending dirty
    // uploads and show a tailored confirmation so the user knows their
    // unsaved changes will be uploaded before the session ends.
    QObject::connect(
        &mainWindow, &MainWindow::logoutRequested, &app,
        [&fuseDriver, &authManager, &mainWindow, fuseEnabled]() {
            int dirtyCount = 0;
            if (fuseEnabled && fuseDriver.isMounted() && fuseDriver.fileCache()) {
                dirtyCount = fuseDriver.fileCache()->getDirtyFiles().size();
            }

            QString msg = dirtyCount > 0
                              ? QStringLiteral(
                                    "You have %1 unsaved file(s) that will be uploaded to Google "
                                    "Drive before you are signed out.\n\n"
                                    "Are you sure you want to sign out?")
                                    .arg(dirtyCount)
                              : QStringLiteral(
                                    "Are you sure you want to sign out?\n\n"
                                    "Synchronization will stop and you will need to sign in again "
                                    "to resume.");

            auto reply = QMessageBox::question(&mainWindow, QStringLiteral("Sign Out"), msg,
                                               QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply == QMessageBox::Yes) {
                mainWindow.addRecentActivity("Signed out");
                authManager.logout();
            }
        });

    QObject::connect(&mainWindow, &MainWindow::clearCacheRequested, &app,
                     [&fuseDriver, fuseEnabled, &settings]() {
                         settings.setValue("advanced/pendingCachePurge", true);
                         settings.sync();

                         if (fuseEnabled && fuseDriver.isMounted()) {
                             fuseDriver.unmount();
                         }

                         QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                                 QCoreApplication::arguments());
                         QCoreApplication::quit();
                     });

    // Handle restart requests from the settings dialog.  Set a pending
    // flag as a convenience hint so the next startup can fast-path the
    // representation reset.  Startup is authoritative — it also compares
    // current vs previous mode independently, so this flag is a belt-and-
    // suspenders safety net, not the sole trigger.
    QObject::connect(
        &mainWindow, &MainWindow::restartRequested, &app, [&fuseDriver, fuseEnabled, &settings]() {
            const QString currentMode = settings.value("advanced/nativeDocMode", "hide").toString();
            bool wrotePendingReset = false;

            const QString previousMode =
                settings.value("advanced/previousNativeDocMode", currentMode).toString();
            if (currentMode != previousMode) {
                settings.setValue("advanced/pendingFuseRepresentationReset", true);
                wrotePendingReset = true;
            }

            const QString previousMirrorMode =
                settings.value("advanced/previousMirrorNativeDocMode", currentMode).toString();
            if (currentMode != previousMirrorMode) {
                settings.setValue("advanced/pendingMirrorRepresentationReset", true);
                wrotePendingReset = true;
            }

            if (wrotePendingReset) {
                settings.sync();
            }

            // Safe FUSE unmount (flushes dirty files, stops workers, clears caches)
            if (fuseEnabled && fuseDriver.isMounted()) {
                fuseDriver.unmount();
            }

            // Relaunch and quit
            QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                    QCoreApplication::arguments());
            QCoreApplication::quit();
        });

    // Connect tray "Sync Now" to full sync (only when mirror sync is enabled)
    if (mirrorEnabled) {
        const auto queueFullSyncRequest = [&pauseController, queueMirrorRequestFullSync]() {
            if (pauseController.isEffectivelyPaused()) {
                return;
            }

            queueMirrorRequestFullSync();
        };

        QObject::connect(&trayManager, &SystemTrayManager::fullSyncRequested, &app,
                         queueFullSyncRequest);
        QObject::connect(&mainWindow, &MainWindow::fullSyncRequested, &app, queueFullSyncRequest);
    }

    // Connect storage info to shared UI status coordinator
    QObject::connect(&driveClient, &GoogleDriveClient::aboutInfoReceived, &statusCoordinator,
                     &UiStatusCoordinator::updateStorageInfo);

    // Connect sync action thread status updates to shared UI status coordinator
    if (mirrorEnabled) {
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::syncActionCompleted,
                         &statusCoordinator, [&statusCoordinator](const SyncActionItem&) {
                             statusCoordinator.updateMirrorStatus("Syncing...");
                         });
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::syncActionFailed,
                         &statusCoordinator,
                         [&statusCoordinator](const SyncActionItem&, const QString&) {
                             statusCoordinator.updateMirrorStatus("Sync error");
                         });

        // Connect full sync state changes to shared UI status coordinator
        QObject::connect(
            &mirrorSyncRuntime, &MirrorSyncRuntime::fullSyncStateChanged, &statusCoordinator,
            [&statusCoordinator](FullSync::State state) {
                switch (state) {
                    case FullSync::State::ScanningLocal:
                        statusCoordinator.updateMirrorStatus("Scanning local files...");
                        break;
                    case FullSync::State::FetchingRemote:
                        statusCoordinator.updateMirrorStatus("Fetching remote files...");
                        break;
                    case FullSync::State::Complete:
                        statusCoordinator.updateMirrorStatus("Syncing...");
                        break;
                    case FullSync::State::Error:
                        statusCoordinator.updateMirrorStatus("Sync error");
                        break;
                    case FullSync::State::Idle:
                        break;
                }
            });
    }

    // Connect FUSE subsystem signals to shared UI status coordinator
    if (fuseEnabled) {
        QObject::connect(&fuseDriver, &FuseDriver::mounted, &statusCoordinator,
                         [&statusCoordinator]() { statusCoordinator.updateFuseStatus("Mounted"); });
        QObject::connect(&fuseDriver, &FuseDriver::unmounted, &statusCoordinator,
                         [&statusCoordinator]() { statusCoordinator.updateFuseStatus("Idle"); });
        QObject::connect(&fuseDriver, &FuseDriver::mountError, &statusCoordinator,
                         [&statusCoordinator](const QString& error) {
                             statusCoordinator.updateFuseStatus("Error: " + error);
                         });
        QObject::connect(&fuseDriver, &FuseDriver::dirtyFilesFlushed, &statusCoordinator,
                         &UiStatusCoordinator::onDirtyFilesFlushed);
        QObject::connect(&fuseDriver, &FuseDriver::metadataRefreshStarted, &statusCoordinator,
                         &UiStatusCoordinator::onMetadataRefreshStarted);
        QObject::connect(&fuseDriver, &FuseDriver::metadataRefreshed, &statusCoordinator,
                         &UiStatusCoordinator::onMetadataRefreshFinished);
        QObject::connect(&fuseDriver, &FuseDriver::metadataRefreshFailed, &statusCoordinator,
                         &UiStatusCoordinator::onMetadataRefreshFailed);
        QObject::connect(&fuseDriver, &FuseDriver::downloadStarted, &statusCoordinator,
                         &UiStatusCoordinator::onDownloadStarted);
        QObject::connect(&fuseDriver, &FuseDriver::downloadFinished, &statusCoordinator,
                         &UiStatusCoordinator::onDownloadFinished);
        QObject::connect(&fuseDriver, &FuseDriver::uploadActivityChanged, &statusCoordinator,
                         &UiStatusCoordinator::onUploadActivityChanged);

        // Wire FUSE activity signals to Recent Activity list
        QObject::connect(&fuseDriver, &FuseDriver::fuseFileCreated, &mainWindow,
                         [&mainWindow](const QString& path) {
                             mainWindow.addRecentActivity("Created: " + path);
                         });
        QObject::connect(&fuseDriver, &FuseDriver::fuseFolderCreated, &mainWindow,
                         [&mainWindow](const QString& path) {
                             mainWindow.addRecentActivity("Created folder: " + path);
                         });
        QObject::connect(&fuseDriver, &FuseDriver::fuseItemTrashed, &mainWindow,
                         [&mainWindow](const QString& path) {
                             mainWindow.addRecentActivity("Trashed: " + path);
                         });
        QObject::connect(
            &fuseDriver, &FuseDriver::fuseItemRenamed, &mainWindow,
            [&mainWindow](const QString& from, const QString& to) {
                mainWindow.addRecentActivity(
                    QString("Renamed: %1 -> %2").arg(from.section('/', -1), to.section('/', -1)));
            });
        QObject::connect(&fuseDriver, &FuseDriver::fuseItemMoved, &mainWindow,
                         [&mainWindow](const QString& from, const QString& to) {
                             mainWindow.addRecentActivity(QString("Moved: %1 -> %2").arg(from, to));
                         });
        QObject::connect(&fuseDriver, &FuseDriver::uploadStarted, &mainWindow,
                         [&mainWindow](const QString&, const QString& path) {
                             mainWindow.addRecentActivity("Uploading: " + path);
                         });
        QObject::connect(&fuseDriver, &FuseDriver::uploadFinished, &mainWindow,
                         [&mainWindow](const QString&, const QString& path) {
                             mainWindow.addRecentActivity("Uploaded: " + path);
                         });
        QObject::connect(
            &fuseDriver, &FuseDriver::fuseUploadFailed, &mainWindow,
            [&mainWindow](const QString& path, const QString& error) {
                mainWindow.addRecentActivity(QString("Upload failed: %1 (%2)").arg(path, error));
            });
        QObject::connect(&fuseDriver, &FuseDriver::nativeDocExportFailed, &mainWindow,
                         [&mainWindow](const QString& path, const QString& error) {
                             mainWindow.addRecentActivity(
                                 QString("Native doc export failed: %1 (%2)").arg(path, error));
                         });
        QObject::connect(
            &fuseDriver, &FuseDriver::fuseRemoteChange, &mainWindow,
            [&mainWindow](const QString& displayPath, const QString& changeType) {
                mainWindow.addRecentActivity(QString("Remote %1: %2").arg(changeType, displayPath));
            });
        QObject::connect(&fuseDriver, &FuseDriver::driveOperationBlocked, &notificationManager,
                         [&notificationManager, &mainWindow, lastBlockedNoticeMs](
                             const QString& action, const QString& path, const QString& message) {
                             mainWindow.addRecentActivity(
                                 QString("Blocked while paused: %1 (%2)").arg(action, path));

                             const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                             if ((nowMs - *lastBlockedNoticeMs) < 4000) {
                                 return;
                             }

                             *lastBlockedNoticeMs = nowMs;
                             notificationManager.showWarning(QStringLiteral("Drive Access Paused"),
                                                             message);
                         });
    }

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

    ConflictDialog conflictDialog(&mainWindow);
    if (mirrorEnabled) {
        // Connect change processor errors to notification manager
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::processorError,
                         &notificationManager, [&notificationManager](const QString& error) {
                             notificationManager.showError("Sync Error", error);
                         });

        // Connect conflict detection to notification
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::conflictDetected,
                         &notificationManager, [&notificationManager](const ConflictInfo& info) {
                             QString fileName = QFileInfo(info.localPath).fileName();
                             notificationManager.showConflict(fileName);
                         });

        // MIS-01: Wire ConflictDialog for interactive conflict resolution
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::conflictDetected, &conflictDialog,
                         [&conflictDialog](const ConflictInfo& info) {
                             conflictDialog.addConflict(info);
                             conflictDialog.show();
                             conflictDialog.raise();
                             conflictDialog.activateWindow();
                         });
        QObject::connect(&conflictDialog, &ConflictDialog::conflictResolved, &mirrorSyncRuntime,
                         &MirrorSyncRuntime::resolveConflict);

        // Connect conflict detection to shared UI status coordinator for warning icon state
        QObject::connect(
            &mirrorSyncRuntime, &MirrorSyncRuntime::conflictDetected, &statusCoordinator,
            [&statusCoordinator](const ConflictInfo&) { statusCoordinator.setHasConflicts(true); });
        QObject::connect(
            &mirrorSyncRuntime, &MirrorSyncRuntime::conflictResolved, &statusCoordinator,
            [&statusCoordinator, &mirrorSyncRuntime](const QString&, ConflictResolutionStrategy) {
                // LOG-03: Only clear conflict icon when all conflicts are resolved
                if (mirrorSyncRuntime.unresolvedConflictCount() == 0) {
                    statusCoordinator.setHasConflicts(false);
                }
            });

        // Connect sync action thread errors to notification manager
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::syncActionError,
                         &notificationManager, [&notificationManager](const QString& error) {
                             notificationManager.showError("Sync Action Error", error);
                         });

        // Connect progress bar to sync action thread
        QObject::connect(
            &mirrorSyncRuntime, &MirrorSyncRuntime::syncActionProgress, &mainWindow,
            [&mainWindow](const SyncActionItem&, qint64 bytesProcessed, qint64 bytesTotal) {
                mainWindow.updateSyncProgress(bytesProcessed, bytesTotal);
            });

        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::tokenRefreshRequested,
                         &authManager, &GoogleAuthManager::refreshTokens);
    }

    bool refreshInFlight = false;
    qint64 lastRefreshAttemptMs = 0;
    constexpr qint64 AUTH_REFRESH_COOLDOWN_MS = 10000;

    const auto handleDriveAuthenticationFailure =
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
        };

    QObject::connect(&driveClient, &GoogleDriveClient::authenticationFailure, &app,
                     handleDriveAuthenticationFailure);
    if (mirrorEnabled) {
        QObject::connect(&mirrorSyncRuntime, &MirrorSyncRuntime::authenticationFailure, &app,
                         handleDriveAuthenticationFailure);
    }

    QObject::connect(&authManager, &GoogleAuthManager::tokenRefreshed, &app,
                     [&refreshInFlight, &wakeRefreshNotificationGate]() {
                         refreshInFlight = false;
                         wakeRefreshNotificationGate.markTokenRefreshed();
                     });

    QObject::connect(
        &authManager, &GoogleAuthManager::tokenRefreshError, &app,
        [&refreshInFlight, &notificationManager, &mainWindow,
         &wakeRefreshNotificationGate](const QString& error) {
            refreshInFlight = false;

            const bool wakeAuthExpired = wakeRefreshNotificationGate.sawWakeAuthExpired();
            if (wakeRefreshNotificationGate.consumeTokenRefreshWarningSuppression()) {
                qWarning() << "Resume handler: suppressed wake token refresh warning"
                           << (wakeAuthExpired ? "after auth expiration:" : ":") << error;
                return;
            }

            mainWindow.addRecentActivity("Token refresh error: " + error);
            notificationManager.showWarning("Authentication Warning", error);
        });

    QObject::connect(&authManager, &GoogleAuthManager::authExpired, &app,
                     [&refreshInFlight, &mirrorSyncRuntime, &trayManager, &mainWindow,
                      &notificationManager, &statusCoordinator, &fuseDriver,
                      &wakeRefreshNotificationGate, &app](const QString& reason) {
                         refreshInFlight = false;
                         wakeRefreshNotificationGate.markAuthExpired();
                         mirrorSyncRuntime.cancelAndStop();
                         stopFuseComponent(&fuseDriver);

                         statusCoordinator.setAuthExpired(reason);
                         mainWindow.setAuthExpired(reason);
                         trayManager.updateAuthState(false);
                         trayManager.showNotification(
                             "Session Expired", "Google Drive session expired. Sign in again.",
                             QSystemTrayIcon::Warning);
                         notificationManager.showWarning(
                             "Authentication Expired",
                             "Session expired. Re-authentication is required to resume sync.");

                         QTimer::singleShot(0, &app, [&wakeRefreshNotificationGate]() {
                             if (wakeRefreshNotificationGate.sawWakeAuthExpired()) {
                                 wakeRefreshNotificationGate.reset();
                             }
                         });
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
            QTimer::singleShot(
                100, &app,
                [&fuseDriver, &pauseController, fuseEnabled, mirrorEnabled, &syncFolder,
                 &trayManager, &statusCoordinator, queueMirrorStartupSync]() {
                    Q_UNUSED(mirrorEnabled);
                    Q_UNUSED(pauseController);
                    queueMirrorStartupSync();
                    if (fuseEnabled) {
                        startFuseComponent(&fuseDriver, syncFolder);
                        if (pauseController.isEffectivelyPaused()) {
                            fuseDriver.pauseSync();
                        }
                    }
                    statusCoordinator.updateAuthState(true);
                    trayManager.updateAuthState(true);
                });
        }
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     [&fuseDriver, &mirrorSyncRuntime, fuseEnabled]() {
                         mirrorSyncRuntime.shutdown();

                         // Warn the user that we're about to block and upload any pending
                         // dirty files before quitting, so they don't force-kill the process.
                         if (fuseEnabled && fuseDriver.isMounted() && fuseDriver.fileCache()) {
                             int dirtyCount = fuseDriver.fileCache()->getDirtyFiles().size();
                             if (dirtyCount > 0) {
                                 QMessageBox::information(
                                     nullptr, QStringLiteral("Uploading pending files"),
                                     QStringLiteral("Uploading %1 unsaved file(s) to Google Drive "
                                                    "before quitting.\n\n"
                                                    "Please wait — do not force-quit.")
                                         .arg(dirtyCount));
                             }
                         }
                         stopFuseComponent(&fuseDriver);
                     });

    // Show main window on first run or if not logged in
    if (!tokenStorage.hasValidTokens()) {
        mainWindow.show();
    }

    qInfo() << "Via started successfully";

    return app.exec();
}
