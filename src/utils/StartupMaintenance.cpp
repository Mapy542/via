/**
 * @file StartupMaintenance.cpp
 * @brief Startup compatibility policy helpers.
 */

#include "StartupMaintenance.h"

namespace StartupMaintenance {

bool shouldPurgeFuseRepresentationCache(const FuseMaintenanceInputs& inputs) {
    return inputs.currentNativeDocMode != inputs.previousNativeDocMode ||
           inputs.pendingRepresentationReset || inputs.pendingCachePurge ||
           inputs.currentRepresentationEpoch > inputs.storedRepresentationEpoch;
}

SyncResetDecision classifySyncReset(SyncDatabase::SchemaCompatibility compatibility) {
    SyncResetDecision decision;

    switch (compatibility) {
        case SyncDatabase::SchemaCompatibility::Current:
            break;
        case SyncDatabase::SchemaCompatibility::ResetRequired:
            decision.requiresReset = true;
            decision.requestFullSyncAfterReset = true;
            break;
        case SyncDatabase::SchemaCompatibility::ResetBlockedByDirtyState:
            decision.requiresReset = true;
            decision.requiresExplicitDiscard = true;
            decision.requestFullSyncAfterReset = true;
            break;
        case SyncDatabase::SchemaCompatibility::UnsupportedFutureSchema:
            decision.unsupportedFutureSchema = true;
            break;
    }

    return decision;
}

}  // namespace StartupMaintenance