/**
 * @file StartupMaintenance.h
 * @brief Startup compatibility policy helpers.
 */

#ifndef STARTUPMAINTENANCE_H
#define STARTUPMAINTENANCE_H

#include <QString>

#include "sync/SyncDatabase.h"

namespace StartupMaintenance {

struct FuseMaintenanceInputs {
    QString currentNativeDocMode;
    QString previousNativeDocMode;
    bool pendingRepresentationReset = false;
    bool pendingCachePurge = false;
    int storedRepresentationEpoch = 0;
    int currentRepresentationEpoch = 0;
};

bool shouldPurgeFuseRepresentationCache(const FuseMaintenanceInputs& inputs);

struct MirrorMaintenanceInputs {
    QString currentNativeDocMode;
    QString previousNativeDocMode;
    bool pendingRepresentationReset = false;
    int storedRepresentationEpoch = 0;
    int currentRepresentationEpoch = 0;
};

struct MirrorRepresentationRebuildStats {
    int removedArtifactCount = 0;
    int clearedMappingCount = 0;
};

bool shouldRebuildMirrorRepresentation(const MirrorMaintenanceInputs& inputs);
bool purgeMirrorNativeDocArtifacts(const QString& syncFolder, SyncDatabase& syncDatabase,
                                   MirrorRepresentationRebuildStats* stats = nullptr);

struct SyncResetDecision {
    bool requiresReset = false;
    bool requiresExplicitDiscard = false;
    bool unsupportedFutureSchema = false;
    bool requestFullSyncAfterReset = false;
};

SyncResetDecision classifySyncReset(SyncDatabase::SchemaCompatibility compatibility);

}  // namespace StartupMaintenance

#endif  // STARTUPMAINTENANCE_H