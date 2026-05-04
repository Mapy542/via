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

struct SyncResetDecision {
    bool requiresReset = false;
    bool requiresExplicitDiscard = false;
    bool unsupportedFutureSchema = false;
    bool requestFullSyncAfterReset = false;
};

SyncResetDecision classifySyncReset(SyncDatabase::SchemaCompatibility compatibility);

}  // namespace StartupMaintenance

#endif  // STARTUPMAINTENANCE_H