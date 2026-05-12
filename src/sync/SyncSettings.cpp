/**
 * @file SyncSettings.cpp
 * @brief Shared sync settings loaded from QSettings
 */

#include "SyncSettings.h"

#include <QDir>

#include "utils/NativeDocSupport.h"

namespace {
constexpr const char* kSyncFolderKey = "sync/folder";
constexpr const char* kSyncModeKey = "sync/syncMode";
constexpr const char* kConflictStrategyKey = "sync/conflictStrategy";
constexpr const char* kDuplicateNameStrategyKey = "sync/duplicateNameStrategy";
constexpr const char* kRemotePollIntervalKey = "sync/remotePollIntervalMs";
constexpr const char* kMirrorDormantTimeKey = "sync/mirrorDormantTimeMs";
constexpr const char* kMirrorDutyCycleKey = "sync/mirrorDutyCyclePct";
constexpr const char* kNativeDocModeKey = "advanced/nativeDocMode";

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

    QString duplicateNameStrategy =
        qsettings.value(kDuplicateNameStrategyKey, "file-id-suffix").toString();
    if (duplicateNameStrategy != "file-id-suffix" && duplicateNameStrategy != "numeric-suffix") {
        duplicateNameStrategy = "file-id-suffix";
    }
    settings.duplicateNameStrategy = duplicateNameStrategy;

    settings.nativeDocMode = nativeDocModeToString(
        nativeDocModeFromString(qsettings.value(kNativeDocModeKey, "hide").toString()));

    settings.ignorePatterns = defaultIgnorePatterns();

    int pollInterval =
        qsettings.value(kRemotePollIntervalKey, DEFAULT_REMOTE_POLL_INTERVAL_MS).toInt();
    if (pollInterval > 0) {
        settings.remotePollIntervalMs = pollInterval;
    }

    settings.mirrorDormantTimeMs = normalizeMirrorDormantTimeMs(
        qsettings.value(kMirrorDormantTimeKey, DEFAULT_MIRROR_DORMANT_TIME_MS).toInt());
    settings.mirrorDutyCyclePercent = normalizeMirrorDutyCyclePercent(
        qsettings.value(kMirrorDutyCycleKey, DEFAULT_MIRROR_DUTY_CYCLE_PERCENT).toInt());

    return settings;
}

int SyncSettings::normalizeMirrorDormantTimeMs(int value) {
    if (value < MIN_MIRROR_DORMANT_TIME_MS || value > MAX_MIRROR_DORMANT_TIME_MS) {
        return DEFAULT_MIRROR_DORMANT_TIME_MS;
    }

    return value;
}

int SyncSettings::normalizeMirrorDutyCyclePercent(int value) {
    if (value < MIN_MIRROR_DUTY_CYCLE_PERCENT || value > MAX_MIRROR_DUTY_CYCLE_PERCENT) {
        return DEFAULT_MIRROR_DUTY_CYCLE_PERCENT;
    }

    return value;
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
