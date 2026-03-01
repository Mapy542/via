/**
 * @file SyncSettings.cpp
 * @brief Shared sync settings loaded from QSettings
 */

#include "SyncSettings.h"

#include <QDir>

namespace {
constexpr const char* kSyncFolderKey = "sync/folder";
constexpr const char* kSyncModeKey = "sync/syncMode";
constexpr const char* kConflictStrategyKey = "sync/conflictStrategy";
constexpr const char* kRemotePollIntervalKey = "sync/remotePollIntervalMs";

QString syncModeFromLegacy(int value) {
    switch (value) {
        case 1:
            return "remote-read-only";
        case 2:
            return "remote-no-delete";
        case 0:
        default:
            return "keep-newest";
    }
}

QString conflictStrategyFromLegacy(int value) {
    switch (value) {
        case 1:
            return "keep-local";
        case 2:
            return "keep-remote";
        case 3:
            return "keep-newest";
        case 4:
            return "ask-user";
        case 0:
        default:
            return "keep-both";
    }
}
}  // namespace

SyncSettings SyncSettings::load() {
    SyncSettings settings;
    QSettings qsettings;

    settings.syncFolder =
        qsettings.value(kSyncFolderKey, QDir::homePath() + "/GoogleDrive").toString();

    QString syncMode = qsettings.value(kSyncModeKey, "").toString();
    if (syncMode.isEmpty()) {
        bool ok = false;
        int legacyMode = qsettings.value(kSyncModeKey, 0).toInt(&ok);
        if (ok) {
            syncMode = syncModeFromLegacy(legacyMode);
        }
    }
    if (syncMode.isEmpty()) {
        syncMode = "keep-newest";
    }
    settings.syncMode = syncMode;

    QString conflictStrategy = qsettings.value(kConflictStrategyKey, "").toString();
    if (conflictStrategy.isEmpty()) {
        bool ok = false;
        int legacyStrategy = qsettings.value(kConflictStrategyKey, 0).toInt(&ok);
        if (ok) {
            conflictStrategy = conflictStrategyFromLegacy(legacyStrategy);
        }
    }
    if (conflictStrategy.isEmpty()) {
        conflictStrategy = "keep-both";
    }
    settings.conflictStrategy = conflictStrategy;

    settings.ignorePatterns = defaultIgnorePatterns();

    int pollInterval = qsettings.value(kRemotePollIntervalKey, 30000).toInt();
    if (pollInterval > 0) {
        settings.remotePollIntervalMs = pollInterval;
    }

    return settings;
}

QStringList SyncSettings::defaultIgnorePatterns() {
    return {
        ".*",         // Hidden files (starting with .)
        "*.tmp",      // Temporary files
        "*.swp",      // Vim swap files
        "*~",         // Backup files
        "Thumbs.db",  // Windows thumbnail cache
        "*.part",     // Partial downloads
        "*.partial"   // Partial files
    };
}
