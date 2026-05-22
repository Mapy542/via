/**
 * @file SyncSettings.h
 * @brief Shared sync settings loaded from QSettings
 */

#ifndef SYNCSETTINGS_H
#define SYNCSETTINGS_H

#include <QSettings>
#include <QString>
#include <QStringList>

enum class SyncMode {
    KeepNewest,
    RemoteReadOnly,
    RemoteNoDelete,
};

enum class RemoteMutationType {
    Upload,
    CreateFile,
    CreateFolder,
    Rename,
    Move,
    Delete,
    Trash,
};

struct SyncSettings {
    static constexpr int DEFAULT_REMOTE_POLL_INTERVAL_MS = 30000;
    static constexpr int DEFAULT_MIRROR_DORMANT_TIME_MS = 0;
    static constexpr int DEFAULT_MIRROR_DUTY_CYCLE_PERCENT = 100;
    static constexpr int MIN_MIRROR_DORMANT_TIME_MS = 0;
    static constexpr int MAX_MIRROR_DORMANT_TIME_MS = 60000;
    static constexpr int MIN_MIRROR_DUTY_CYCLE_PERCENT = 1;
    static constexpr int MAX_MIRROR_DUTY_CYCLE_PERCENT = 100;

    QString syncFolder;
    QString syncMode;
    QString conflictStrategy;
    QString duplicateNameStrategy;
    QString nativeDocMode;
    QStringList ignorePatterns;
    int remotePollIntervalMs = DEFAULT_REMOTE_POLL_INTERVAL_MS;
    int mirrorDormantTimeMs = DEFAULT_MIRROR_DORMANT_TIME_MS;
    int mirrorDutyCyclePercent = DEFAULT_MIRROR_DUTY_CYCLE_PERCENT;

    static SyncSettings load();
    static QString normalizeSyncModeId(const QString& value);
    static SyncMode syncModeFromString(const QString& value);
    static int normalizeMirrorDormantTimeMs(int value);
    static int normalizeMirrorDutyCyclePercent(int value);

    SyncMode syncModeValue() const { return syncModeFromString(syncMode); }
    bool allowsRemoteMutation(RemoteMutationType mutation) const;
    bool isRemoteReadOnly() const { return syncModeValue() == SyncMode::RemoteReadOnly; }
    bool isRemoteNoDelete() const { return syncModeValue() == SyncMode::RemoteNoDelete; }

    static QStringList defaultIgnorePatterns();
};

#endif  // SYNCSETTINGS_H
