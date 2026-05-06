/**
 * @file TestStartupMaintenance.cpp
 * @brief Unit tests for startup compatibility policy helpers.
 */

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "utils/StartupMaintenance.h"

class TestStartupMaintenance : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void testFuseMaintenance_NoPurgeForOrdinaryStartup();
    void testFuseMaintenance_PurgesOnModeChange();
    void testFuseMaintenance_PurgesOnEpochBump();
    void testMirrorMaintenance_NoRebuildForOrdinaryStartup();
    void testMirrorMaintenance_RebuildsOnModeChange();
    void testMirrorMaintenance_RebuildsOnEpochBump();
    void testMirrorMaintenance_PurgesOnlyNativeDocArtifacts();
    void testSyncReset_CurrentSchemaNeedsNoAction();
    void testSyncReset_LegacySchemaRequestsFullRebuild();
    void testSyncReset_DirtyLegacySchemaRequiresExplicitDiscard();
    void testSyncReset_FutureSchemaBlocksStartup();
};

void TestStartupMaintenance::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("ViaTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TestStartupMaintenance"));
}

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

void TestStartupMaintenance::testMirrorMaintenance_NoRebuildForOrdinaryStartup() {
    const StartupMaintenance::MirrorMaintenanceInputs inputs{
        .currentNativeDocMode = QStringLiteral("hide"),
        .previousNativeDocMode = QStringLiteral("hide"),
        .pendingRepresentationReset = false,
        .storedRepresentationEpoch = 1,
        .currentRepresentationEpoch = 1,
    };

    QVERIFY(!StartupMaintenance::shouldRebuildMirrorRepresentation(inputs));
}

void TestStartupMaintenance::testMirrorMaintenance_RebuildsOnModeChange() {
    const StartupMaintenance::MirrorMaintenanceInputs inputs{
        .currentNativeDocMode = QStringLiteral("browser-shortcut"),
        .previousNativeDocMode = QStringLiteral("hide"),
        .pendingRepresentationReset = false,
        .storedRepresentationEpoch = 1,
        .currentRepresentationEpoch = 1,
    };

    QVERIFY(StartupMaintenance::shouldRebuildMirrorRepresentation(inputs));
}

void TestStartupMaintenance::testMirrorMaintenance_RebuildsOnEpochBump() {
    const StartupMaintenance::MirrorMaintenanceInputs inputs{
        .currentNativeDocMode = QStringLiteral("hide"),
        .previousNativeDocMode = QStringLiteral("hide"),
        .pendingRepresentationReset = false,
        .storedRepresentationEpoch = 0,
        .currentRepresentationEpoch = 1,
    };

    QVERIFY(StartupMaintenance::shouldRebuildMirrorRepresentation(inputs));
}

void TestStartupMaintenance::testMirrorMaintenance_PurgesOnlyNativeDocArtifacts() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString syncFolder = tempDir.filePath(QStringLiteral("sync"));
    QVERIFY(QDir().mkpath(syncFolder));

    SyncDatabase syncDatabase;
    QVERIFY(syncDatabase.initialize());

    FileSyncState nativeDocFile;
    nativeDocFile.fileId = QStringLiteral("native-doc-id");
    nativeDocFile.localPath = QStringLiteral("Quarterly.gdoc");
    syncDatabase.saveFileState(nativeDocFile);

    NativeDocState nativeDocState;
    nativeDocState.fileId = nativeDocFile.fileId;
    nativeDocState.remoteName = QStringLiteral("Quarterly");
    nativeDocState.remoteMimeType = QStringLiteral("application/vnd.google-apps.document");
    nativeDocState.webViewLink =
        QStringLiteral("https://docs.google.com/document/d/native-doc-id/edit");
    QVERIFY(syncDatabase.saveNativeDocState(nativeDocState));

    FileSyncState normalFile;
    normalFile.fileId = QStringLiteral("plain-file-id");
    normalFile.localPath = QStringLiteral("Notes.txt");
    syncDatabase.saveFileState(normalFile);

    QFile nativeDocArtifact(QDir(syncFolder).filePath(nativeDocFile.localPath));
    QVERIFY(nativeDocArtifact.open(QIODevice::WriteOnly | QIODevice::Truncate));
    nativeDocArtifact.write("native-doc");
    nativeDocArtifact.close();

    QFile normalArtifact(QDir(syncFolder).filePath(normalFile.localPath));
    QVERIFY(normalArtifact.open(QIODevice::WriteOnly | QIODevice::Truncate));
    normalArtifact.write("plain-file");
    normalArtifact.close();

    syncDatabase.markFileDeleted(nativeDocFile.localPath, nativeDocFile.fileId);
    QVERIFY(syncDatabase.wasFileDeleted(nativeDocFile.localPath));
    syncDatabase.setChangeToken(QStringLiteral("change-token-1"));

    StartupMaintenance::MirrorRepresentationRebuildStats stats;
    QVERIFY(StartupMaintenance::purgeMirrorNativeDocArtifacts(syncFolder, syncDatabase, &stats));

    QCOMPARE(stats.removedArtifactCount, 1);
    QCOMPARE(stats.clearedMappingCount, 1);

    QVERIFY(!QFileInfo::exists(QDir(syncFolder).filePath(nativeDocFile.localPath)));
    QVERIFY(QFileInfo::exists(QDir(syncFolder).filePath(normalFile.localPath)));
    QVERIFY(syncDatabase.getLocalPath(nativeDocFile.fileId).isEmpty());
    QCOMPARE(syncDatabase.getLocalPath(normalFile.fileId), normalFile.localPath);
    QVERIFY(!syncDatabase.wasFileDeleted(nativeDocFile.localPath));
    QVERIFY(syncDatabase.getChangeToken().isEmpty());

    QVERIFY(syncDatabase.clearAllData());
    syncDatabase.close();
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