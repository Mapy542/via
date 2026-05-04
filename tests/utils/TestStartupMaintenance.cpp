/**
 * @file TestStartupMaintenance.cpp
 * @brief Unit tests for startup compatibility policy helpers.
 */

#include <QtTest/QtTest>

#include "utils/StartupMaintenance.h"

class TestStartupMaintenance : public QObject {
    Q_OBJECT

   private slots:
    void testFuseMaintenance_NoPurgeForOrdinaryStartup();
    void testFuseMaintenance_PurgesOnModeChange();
    void testFuseMaintenance_PurgesOnEpochBump();
    void testSyncReset_CurrentSchemaNeedsNoAction();
    void testSyncReset_LegacySchemaRequestsFullRebuild();
    void testSyncReset_DirtyLegacySchemaRequiresExplicitDiscard();
    void testSyncReset_FutureSchemaBlocksStartup();
};

void TestStartupMaintenance::testFuseMaintenance_NoPurgeForOrdinaryStartup() {
    const StartupMaintenance::FuseMaintenanceInputs inputs{
        .currentNativeDocMode = QStringLiteral("hide"),
        .previousNativeDocMode = QStringLiteral("hide"),
        .pendingRepresentationReset = false,
        .pendingCachePurge = false,
        .storedRepresentationEpoch = 1,
        .currentRepresentationEpoch = 1,
    };

    QVERIFY(!StartupMaintenance::shouldPurgeFuseRepresentationCache(inputs));
}

void TestStartupMaintenance::testFuseMaintenance_PurgesOnModeChange() {
    const StartupMaintenance::FuseMaintenanceInputs inputs{
        .currentNativeDocMode = QStringLiteral("export"),
        .previousNativeDocMode = QStringLiteral("hide"),
        .pendingRepresentationReset = false,
        .pendingCachePurge = false,
        .storedRepresentationEpoch = 1,
        .currentRepresentationEpoch = 1,
    };

    QVERIFY(StartupMaintenance::shouldPurgeFuseRepresentationCache(inputs));
}

void TestStartupMaintenance::testFuseMaintenance_PurgesOnEpochBump() {
    const StartupMaintenance::FuseMaintenanceInputs inputs{
        .currentNativeDocMode = QStringLiteral("hide"),
        .previousNativeDocMode = QStringLiteral("hide"),
        .pendingRepresentationReset = false,
        .pendingCachePurge = false,
        .storedRepresentationEpoch = 1,
        .currentRepresentationEpoch = 2,
    };

    QVERIFY(StartupMaintenance::shouldPurgeFuseRepresentationCache(inputs));
}

void TestStartupMaintenance::testSyncReset_CurrentSchemaNeedsNoAction() {
    const auto decision =
        StartupMaintenance::classifySyncReset(SyncDatabase::SchemaCompatibility::Current);

    QVERIFY(!decision.requiresReset);
    QVERIFY(!decision.requiresExplicitDiscard);
    QVERIFY(!decision.unsupportedFutureSchema);
    QVERIFY(!decision.requestFullSyncAfterReset);
}

void TestStartupMaintenance::testSyncReset_LegacySchemaRequestsFullRebuild() {
    const auto decision =
        StartupMaintenance::classifySyncReset(SyncDatabase::SchemaCompatibility::ResetRequired);

    QVERIFY(decision.requiresReset);
    QVERIFY(!decision.requiresExplicitDiscard);
    QVERIFY(!decision.unsupportedFutureSchema);
    QVERIFY(decision.requestFullSyncAfterReset);
}

void TestStartupMaintenance::testSyncReset_DirtyLegacySchemaRequiresExplicitDiscard() {
    const auto decision = StartupMaintenance::classifySyncReset(
        SyncDatabase::SchemaCompatibility::ResetBlockedByDirtyState);

    QVERIFY(decision.requiresReset);
    QVERIFY(decision.requiresExplicitDiscard);
    QVERIFY(!decision.unsupportedFutureSchema);
    QVERIFY(decision.requestFullSyncAfterReset);
}

void TestStartupMaintenance::testSyncReset_FutureSchemaBlocksStartup() {
    const auto decision = StartupMaintenance::classifySyncReset(
        SyncDatabase::SchemaCompatibility::UnsupportedFutureSchema);

    QVERIFY(!decision.requiresReset);
    QVERIFY(!decision.requiresExplicitDiscard);
    QVERIFY(decision.unsupportedFutureSchema);
    QVERIFY(!decision.requestFullSyncAfterReset);
}

QTEST_MAIN(TestStartupMaintenance)
#include "TestStartupMaintenance.moc"