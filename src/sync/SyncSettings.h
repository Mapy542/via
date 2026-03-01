/**
 * @file SyncSettings.h
 * @brief Shared sync settings loaded from QSettings
 */

#ifndef SYNCSETTINGS_H
#define SYNCSETTINGS_H

#include <QSettings>
#include <QString>
#include <QStringList>

struct SyncSettings {
    QString syncFolder;
    QString syncMode;
    QString conflictStrategy;
    QStringList ignorePatterns;
    int remotePollIntervalMs = 30000;

    static SyncSettings load();

    bool isRemoteReadOnly() const { return syncMode == "remote-read-only"; }
    bool isRemoteNoDelete() const { return syncMode == "remote-no-delete"; }

    static QStringList defaultIgnorePatterns();
};

#endif  // SYNCSETTINGS_H
