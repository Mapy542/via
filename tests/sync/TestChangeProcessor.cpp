/**
 * @file TestChangeProcessor.cpp
 * @brief Unit tests for ChangeProcessor subsystem
 *
 * Tests cover: change classification, conflict detection, action generation,
 * folder validation, and local/remote origin handling.
 */

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#define private public
#define protected public
#include "sync/ChangeProcessor.h"
#undef protected
#undef private

#include "sync/ChangeQueue.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncDatabase.h"

class TestChangeProcessor : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // Change classification
    void testClassifyLocalCreate();
    void testClassifyLocalModify();
    void testClassifyLocalDelete();
    void testClassifyLocalMove();
    void testClassifyRemoteCreate();
    void testClassifyRemoteModify();
    void testClassifyRemoteDelete();
    void testClassifyRemoteMove();
    void testRemoteModifyPathChangeReclassifiesMove();

    // Conflict detection (relates to TODO in ChangeProcessor.cpp:393)
    void testConflictDetection_LocalAndRemoteModify();
    void testConflictDetection_DeleteVsModify();
    void testConflictResolution_PreferRemote();
    void testConflictResolution_PreferLocal();
    void testConflictResolution_KeepBoth();
    void testConflictRehydrateOnStart();
    void testConflictAppendWhenUnresolved();
    void testRemoteConflict_LocalUnchanged_NoConflict();
    void testRemoteConflict_LocalNewer_Conflict();
    void testRemoteDelete_LocalUnchanged_NoConflict();
    void testRemoteConflict_ModifyBoundary_NoConflictAtOneSecond();
    void testRemoteConflict_ModifyBoundary_NoConflictAtTwoSeconds();
    void testRemoteConflict_ModifyBoundary_ConflictAtThreeSeconds();
    void testRemoteConflict_DeleteBoundary_NoConflictAtTwoSeconds();
    void testRemoteConflict_DeleteBoundary_ConflictAtThreeSeconds();
    void testRemoteFolderPathChange_NoConflict_MoveActionQueued();

    // Folder validation (relates to TODO in ChangeProcessor.cpp:293)
    void testFolderValidation_Exists();
    void testFolderValidation_Permissions();
    void testFolderValidation_SharedDrive();
    void testFolderValidation_MultipleParents();

    // Local origin re-lookup bug (relates to TODO in ChangeProcessor.cpp:306)
    void testLocalOrigin_CacheHit();
    void testLocalOrigin_CacheMiss();
    void testLocalOrigin_StaleDataHandling();

    // Edge cases
    void testEmptyChange();
    void testMalformedChange();

    void testInOperationChangeSkipped();
    void testWakeOnItemsAvailable();

    // Validation coverage: origins and change types
    void testValidateChange_AllTypesLocal();
    void testValidateChange_AllTypesRemote();
    void testValidateChange_RemotePathChangeAllowsStaleMtime();
    void testValidateChange_LocalMoveMissingFileIdRejected();
    void testValidateChange_LocalRenameMissingFileIdRejected();
    void testValidateChange_DuplicatePendingActionSkipped();
    void testValidateChange_LocalModifySameHashSkipped();
    void testDetermineAndQueueActions_LocalMoveMissingFileIdSkipped();
    void testDetermineAndQueueActions_LocalRenameMissingFileIdSkipped();
    void testDetermineAndQueueActions_DuplicateSuppressed();
    void testDetermineAndQueueActions_RemoteModifySameHashSkipped();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    ChangeQueue* m_changeQueue = nullptr;
    SyncActionQueue* m_actionQueue = nullptr;
    ChangeProcessor* m_processor = nullptr;
    QString m_originalDataPath;

    void setupTestDatabase();
    void cleanupTestDatabase();
    ChangeQueueItem makeChange(ChangeType type, ChangeOrigin origin, const QString& localPath,
                               const QString& fileId = QString());
    void saveState(const QString& localPath, const QString& fileId,
                   const QDateTime& modifiedTimeAtSync, bool isFolder = false);
    QDateTime writeFileWithMtime(const QString& absPath, const QByteArray& data,
                                 const QDateTime& modifiedTime);
    static QDateTime toWholeSecondUtc(const QDateTime& dateTime);
    static qint64 absSecondDelta(const QDateTime& lhs, const QDateTime& rhs);
    static QString md5Hex(const QByteArray& data);
};

void TestChangeProcessor::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    setupTestDatabase();

    m_changeQueue = new ChangeQueue();
    m_actionQueue = new SyncActionQueue();
    m_processor = new ChangeProcessor(m_changeQueue, m_actionQueue, m_db, nullptr);
    m_processor->setSyncFolder(m_tempDir->path());
}

void TestChangeProcessor::cleanup() {
    delete m_processor;
    m_processor = nullptr;

    delete m_actionQueue;
    m_actionQueue = nullptr;

    delete m_changeQueue;
    m_changeQueue = nullptr;

    cleanupTestDatabase();
    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestChangeProcessor::setupTestDatabase() {
    m_originalDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    qputenv("HOME", m_tempDir->path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());
}

void TestChangeProcessor::cleanupTestDatabase() {
    if (m_db) {
        m_db->close();
        delete m_db;
        m_db = nullptr;
    }
    QStandardPaths::setTestModeEnabled(false);
}

ChangeQueueItem TestChangeProcessor::makeChange(ChangeType type, ChangeOrigin origin,
                                                const QString& localPath, const QString& fileId) {
    ChangeQueueItem change;
    change.changeType = type;
    change.origin = origin;
    change.localPath = localPath;
    change.fileId = fileId;
    change.detectedTime = QDateTime::currentDateTime();
    change.modifiedTime = QDateTime::currentDateTime();
    change.isDirectory = false;
    return change;
}

void TestChangeProcessor::saveState(const QString& localPath, const QString& fileId,
                                    const QDateTime& modifiedTimeAtSync, bool isFolder) {
    FileSyncState state;
    state.localPath = localPath;
    state.fileId = fileId;
    state.modifiedTimeAtSync = modifiedTimeAtSync;
    state.isFolder = isFolder;
    m_db->saveFileState(state);
}

QDateTime TestChangeProcessor::writeFileWithMtime(const QString& absPath, const QByteArray& data,
                                                  const QDateTime& modifiedTime) {
    Q_UNUSED(modifiedTime);

    QDir(QFileInfo(absPath).dir()).mkpath(".");
    QFile file(absPath);
    if (!file.open(QIODevice::ReadWrite)) {
        qWarning() << "Failed to open file for mtime test:" << absPath;
        return QDateTime();
    }
    if (file.write(data) <= 0) {
        qWarning() << "Failed to write file for mtime test:" << absPath;
        return QDateTime();
    }
    file.close();

    QFileInfo info(absPath);
    info.refresh();
    const QDateTime observedMtime = toWholeSecondUtc(info.lastModified());

    return observedMtime;
}

QDateTime TestChangeProcessor::toWholeSecondUtc(const QDateTime& dateTime) {
    return QDateTime::fromSecsSinceEpoch(dateTime.toUTC().toSecsSinceEpoch(), QTimeZone::UTC);
}

qint64 TestChangeProcessor::absSecondDelta(const QDateTime& lhs, const QDateTime& rhs) {
    const qint64 delta = lhs.toUTC().secsTo(rhs.toUTC());
    return delta < 0 ? -delta : delta;
}

QString TestChangeProcessor::md5Hex(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
}

void TestChangeProcessor::testClassifyLocalCreate() {
    m_actionQueue->clear();
    ChangeQueueItem change = makeChange(ChangeType::Create, ChangeOrigin::Local, "a.txt");

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::Upload);
    QCOMPARE(action.localPath, QString("a.txt"));
}

void TestChangeProcessor::testClassifyLocalModify() {
    m_actionQueue->clear();
    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Local, "b.txt");

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::Upload);
    QCOMPARE(action.localPath, QString("b.txt"));
}

void TestChangeProcessor::testClassifyLocalDelete() {
    m_actionQueue->clear();
    ChangeQueueItem change =
        makeChange(ChangeType::Delete, ChangeOrigin::Local, "c.txt", "file-123");

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::DeleteRemote);
    QCOMPARE(action.fileId, QString("file-123"));
}

void TestChangeProcessor::testClassifyLocalMove() {
    m_actionQueue->clear();
    ChangeQueueItem change = makeChange(ChangeType::Move, ChangeOrigin::Local, "d.txt", "id");
    change.moveDestination = "folder/d.txt";

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::MoveRemote);
    QCOMPARE(action.fileId, QString("id"));
    QCOMPARE(action.moveDestination, QString("folder"));
}

void TestChangeProcessor::testClassifyRemoteCreate() {
    m_actionQueue->clear();
    ChangeQueueItem change =
        makeChange(ChangeType::Create, ChangeOrigin::Remote, "e.txt", "file-e");

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::Download);
    QCOMPARE(action.fileId, QString("file-e"));
    QCOMPARE(action.localPath, QString("e.txt"));
}

void TestChangeProcessor::testClassifyRemoteModify() {
    m_actionQueue->clear();
    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, "f.txt", "file-f");

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::Download);
    QCOMPARE(action.fileId, QString("file-f"));
}

void TestChangeProcessor::testClassifyRemoteDelete() {
    m_actionQueue->clear();
    ChangeQueueItem change = makeChange(ChangeType::Delete, ChangeOrigin::Remote, "g.txt");

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::DeleteLocal);
    QCOMPARE(action.localPath, QString("g.txt"));
}

void TestChangeProcessor::testClassifyRemoteMove() {
    m_actionQueue->clear();
    ChangeQueueItem change = makeChange(ChangeType::Move, ChangeOrigin::Remote, "h.txt");
    change.moveDestination = "folder/h.txt";

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::MoveLocal);
    QCOMPARE(action.localPath, QString("h.txt"));
    QCOMPARE(action.moveDestination, QString("folder/h.txt"));
}

void TestChangeProcessor::testRemoteModifyPathChangeReclassifiesMove() {
    m_actionQueue->clear();
    QDateTime dbSyncTime = QDateTime::currentDateTime().addSecs(-120);
    saveState("ILOVEYOUSENDNUDES.pwmx", "file-1", dbSyncTime);

    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Remote,
                                        "mario/ILOVEYOUSENDNUDES.pwmx", "file-1");

    QVERIFY(m_processor->validateChange(change));
    QCOMPARE(change.changeType, ChangeType::Move);
    QCOMPARE(change.localPath, QString("ILOVEYOUSENDNUDES.pwmx"));
    QCOMPARE(change.moveDestination, QString("mario/ILOVEYOUSENDNUDES.pwmx"));
    QVERIFY(change.renameTo.isEmpty());

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::MoveLocal);
    QCOMPARE(action.localPath, QString("ILOVEYOUSENDNUDES.pwmx"));
    QCOMPARE(action.moveDestination, QString("mario/ILOVEYOUSENDNUDES.pwmx"));
}

void TestChangeProcessor::testConflictDetection_LocalAndRemoteModify() {
    // Unresolved conflict causes incoming changes to be appended as versions and skipped
    int conflictId = m_db->upsertConflictRecord("conflict.txt", "file-1");
    ConflictVersion version;
    version.localModifiedTime = QDateTime::currentDateTime().addSecs(-10);
    version.remoteModifiedTime = QDateTime::currentDateTime().addSecs(-10);
    version.dbSyncTime = QDateTime::currentDateTime().addSecs(-20);
    m_db->addConflictVersion(conflictId, version);

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, "conflict.txt", "file-1");
    change.modifiedTime = QDateTime::currentDateTime();

    QSignalSpy conflictSpy(m_processor, &ChangeProcessor::conflictDetected);
    QSignalSpy skippedSpy(m_processor, &ChangeProcessor::changeSkipped);

    m_changeQueue->enqueue(change);
    m_processor->m_state = ChangeProcessor::State::Running;
    m_processor->processNextChange();

    QCOMPARE(skippedSpy.count(), 1);
    QCOMPARE(conflictSpy.count(), 1);

    QList<ConflictRecord> conflicts = m_db->getUnresolvedConflicts();
    QCOMPARE(conflicts.count(), 1);
    QVERIFY(conflicts.first().versions.size() >= 2);
}

void TestChangeProcessor::testConflictDetection_DeleteVsModify() {
    ChangeQueueItem change =
        makeChange(ChangeType::Delete, ChangeOrigin::Local, "deleted.txt", "file-2");
    m_db->markFileDeleted("deleted.txt", "file-2");

    QVERIFY(!m_processor->validateChange(change));
}

void TestChangeProcessor::testConflictResolution_PreferRemote() {
    m_actionQueue->clear();
    ConflictInfo conflict;
    conflict.localPath = "conflict.txt";
    conflict.fileId = "file-3";
    conflict.localModifiedTime = QDateTime::currentDateTime().addSecs(-5);
    conflict.remoteModifiedTime = QDateTime::currentDateTime();

    m_processor->resolveConflictInternal(conflict, ConflictResolutionStrategy::KeepRemote);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::Download);
    QCOMPARE(action.fileId, QString("file-3"));
}

void TestChangeProcessor::testConflictResolution_PreferLocal() {
    m_actionQueue->clear();
    ConflictInfo conflict;
    conflict.localPath = "conflict.txt";
    conflict.fileId = "file-4";
    conflict.localModifiedTime = QDateTime::currentDateTime();
    conflict.remoteModifiedTime = QDateTime::currentDateTime().addSecs(-5);

    m_processor->resolveConflictInternal(conflict, ConflictResolutionStrategy::KeepLocal);

    QCOMPARE(m_actionQueue->count(), 1);
    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::Upload);
    QCOMPARE(action.localPath, QString("conflict.txt"));
}

void TestChangeProcessor::testConflictResolution_KeepBoth() {
    m_actionQueue->clear();
    ConflictInfo conflict;
    conflict.localPath = "folder/conflict.txt";
    conflict.fileId = "file-5";
    conflict.localModifiedTime = QDateTime::currentDateTime();
    conflict.remoteModifiedTime = QDateTime::currentDateTime().addSecs(-3);

    m_processor->resolveConflictInternal(conflict, ConflictResolutionStrategy::KeepBoth);

    QCOMPARE(m_actionQueue->count(), 3);
    SyncActionItem moveAction = m_actionQueue->dequeue();
    SyncActionItem downloadAction = m_actionQueue->dequeue();
    SyncActionItem uploadAction = m_actionQueue->dequeue();

    QCOMPARE(moveAction.actionType, SyncActionType::MoveLocal);
    QCOMPARE(downloadAction.actionType, SyncActionType::Download);
    QCOMPARE(uploadAction.actionType, SyncActionType::Upload);
}

void TestChangeProcessor::testConflictRehydrateOnStart() {
    int conflictId = m_db->upsertConflictRecord("rehydrate.txt", "file-6");
    ConflictVersion version;
    version.localModifiedTime = QDateTime::currentDateTime().addSecs(-10);
    version.remoteModifiedTime = QDateTime::currentDateTime().addSecs(-10);
    version.dbSyncTime = QDateTime::currentDateTime().addSecs(-20);
    m_db->addConflictVersion(conflictId, version);

    QSignalSpy conflictSpy(m_processor, &ChangeProcessor::conflictDetected);
    m_processor->rehydrateUnresolvedConflicts();

    QCOMPARE(conflictSpy.count(), 1);
    QList<QVariant> args = conflictSpy.takeFirst();
    ConflictInfo info = qvariant_cast<ConflictInfo>(args.at(0));
    QCOMPARE(info.localPath, QString("rehydrate.txt"));
    QCOMPARE(info.conflictId, conflictId);
}

void TestChangeProcessor::testConflictAppendWhenUnresolved() {
    int conflictId = m_db->upsertConflictRecord("existing.txt", "file-7");
    ConflictVersion version;
    version.localModifiedTime = QDateTime::currentDateTime().addSecs(-20);
    version.remoteModifiedTime = QDateTime::currentDateTime().addSecs(-20);
    version.dbSyncTime = QDateTime::currentDateTime().addSecs(-30);
    m_db->addConflictVersion(conflictId, version);

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Local, "existing.txt", "file-7");

    QSignalSpy conflictSpy(m_processor, &ChangeProcessor::conflictDetected);
    m_processor->appendConflictVersionForChange(change);

    QCOMPARE(conflictSpy.count(), 1);
    QList<ConflictRecord> conflicts = m_db->getUnresolvedConflicts();
    QCOMPARE(conflicts.count(), 1);
    QVERIFY(conflicts.first().versions.size() >= 2);
}

void TestChangeProcessor::testRemoteConflict_LocalUnchanged_NoConflict() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/unchanged.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(absPath, "data", baseTime);
    QVERIFY(observedMtime.isValid());

    saveState(relPath, "remote-id-10", observedMtime);

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, relPath, "remote-id-10");
    change.modifiedTime = observedMtime.addSecs(10);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(!conflict.isConflicted);
}

void TestChangeProcessor::testRemoteConflict_LocalNewer_Conflict() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/newer.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(absPath, "data", baseTime.addSecs(10));
    QVERIFY(observedMtime.isValid());

    saveState(relPath, "remote-id-11",
              observedMtime.addSecs(-(ChangeProcessor::MIN_CHANGE_DIFF_SECS + 1)));

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, relPath, "remote-id-11");
    change.modifiedTime = observedMtime;

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(conflict.isConflicted);
    QVERIFY(conflict.localModifiedTime > conflict.dbSyncTime);
}

void TestChangeProcessor::testRemoteDelete_LocalUnchanged_NoConflict() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/deleted.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(absPath, "data", baseTime);
    QVERIFY(observedMtime.isValid());

    saveState(relPath, "remote-id-12", observedMtime);

    ChangeQueueItem change =
        makeChange(ChangeType::Delete, ChangeOrigin::Remote, relPath, "remote-id-12");
    change.modifiedTime = observedMtime.addSecs(10);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(!conflict.isConflicted);
}

void TestChangeProcessor::testRemoteConflict_ModifyBoundary_NoConflictAtOneSecond() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/boundary-modify-1.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(absPath, "data", baseTime.addSecs(1));
    QVERIFY(observedMtime.isValid());
    saveState(relPath, "remote-id-mod-1", observedMtime.addSecs(-1));

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, relPath, "remote-id-mod-1");
    change.modifiedTime = observedMtime.addSecs(10);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(!conflict.isConflicted);
}

void TestChangeProcessor::testRemoteConflict_ModifyBoundary_NoConflictAtTwoSeconds() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/boundary-modify-2.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(
        absPath, "data", baseTime.addSecs(ChangeProcessor::MIN_CHANGE_DIFF_SECS));
    QVERIFY(observedMtime.isValid());
    saveState(relPath, "remote-id-mod-2",
              observedMtime.addSecs(-ChangeProcessor::MIN_CHANGE_DIFF_SECS));

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, relPath, "remote-id-mod-2");
    change.modifiedTime = observedMtime.addSecs(10);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(!conflict.isConflicted);
}

void TestChangeProcessor::testRemoteConflict_ModifyBoundary_ConflictAtThreeSeconds() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/boundary-modify-3.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(
        absPath, "data", baseTime.addSecs(ChangeProcessor::MIN_CHANGE_DIFF_SECS + 1));
    QVERIFY(observedMtime.isValid());
    saveState(relPath, "remote-id-mod-3",
              observedMtime.addSecs(-(ChangeProcessor::MIN_CHANGE_DIFF_SECS + 1)));

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, relPath, "remote-id-mod-3");
    change.modifiedTime = observedMtime.addSecs(10);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(conflict.isConflicted);
}

void TestChangeProcessor::testRemoteConflict_DeleteBoundary_NoConflictAtTwoSeconds() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/boundary-delete-2.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(
        absPath, "data", baseTime.addSecs(ChangeProcessor::MIN_CHANGE_DIFF_SECS));
    QVERIFY(observedMtime.isValid());
    saveState(relPath, "remote-id-del-2",
              observedMtime.addSecs(-ChangeProcessor::MIN_CHANGE_DIFF_SECS));

    ChangeQueueItem change =
        makeChange(ChangeType::Delete, ChangeOrigin::Remote, relPath, "remote-id-del-2");
    change.modifiedTime = observedMtime.addSecs(10);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(!conflict.isConflicted);
}

void TestChangeProcessor::testRemoteConflict_DeleteBoundary_ConflictAtThreeSeconds() {
    const QDateTime baseTime = toWholeSecondUtc(QDateTime::currentDateTimeUtc().addSecs(-60));
    const QString relPath = "remote/boundary-delete-3.txt";
    const QString absPath = m_tempDir->filePath(relPath);

    const QDateTime observedMtime = writeFileWithMtime(
        absPath, "data", baseTime.addSecs(ChangeProcessor::MIN_CHANGE_DIFF_SECS + 1));
    QVERIFY(observedMtime.isValid());
    saveState(relPath, "remote-id-del-3",
              observedMtime.addSecs(-(ChangeProcessor::MIN_CHANGE_DIFF_SECS + 1)));

    ChangeQueueItem change =
        makeChange(ChangeType::Delete, ChangeOrigin::Remote, relPath, "remote-id-del-3");
    change.modifiedTime = observedMtime.addSecs(10);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(conflict.isConflicted);
}

void TestChangeProcessor::testRemoteFolderPathChange_NoConflict_MoveActionQueued() {
    m_actionQueue->clear();

    const QString oldPath = "qwe/Windows";
    const QString newPath = "qwe/Screenshots/Windows";

    QDir syncRoot(m_tempDir->path());
    QVERIFY(syncRoot.mkpath(oldPath));
    QVERIFY(syncRoot.mkpath("qwe/Screenshots"));

    saveState(oldPath, "folder-id-remote-move", QDateTime::currentDateTime().addDays(-10), true);

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, newPath, "folder-id-remote-move");
    change.isDirectory = true;
    change.modifiedTime = QDateTime::currentDateTime().addDays(-6);

    QVERIFY(m_processor->validateChange(change));
    QCOMPARE(change.changeType, ChangeType::Move);
    QCOMPARE(change.localPath, oldPath);
    QCOMPARE(change.moveDestination, newPath);

    ConflictInfo conflict = m_processor->checkForConflict(change);
    QVERIFY(!conflict.isConflicted);

    m_processor->determineAndQueueActions(change);
    QCOMPARE(m_actionQueue->count(), 1);

    SyncActionItem action = m_actionQueue->dequeue();
    QCOMPARE(action.actionType, SyncActionType::MoveLocal);
    QCOMPARE(action.localPath, oldPath);
    QCOMPARE(action.moveDestination, newPath);
}

void TestChangeProcessor::testFolderValidation_Exists() {
    FileSyncState state;
    state.localPath = "folder";
    state.fileId = "folder-id";
    state.isFolder = true;
    m_db->saveFileState(state);

    ChangeQueueItem change = makeChange(ChangeType::Create, ChangeOrigin::Local, "folder");
    change.isDirectory = true;

    QVERIFY(!m_processor->validateChange(change));
}

void TestChangeProcessor::testFolderValidation_Permissions() {
    FileSyncState state;
    state.localPath = "folder2";
    state.fileId = "folder2-id";
    state.isFolder = true;
    m_db->saveFileState(state);
    m_db->markFileDeleted("folder2", "folder2-id");

    ChangeQueueItem change = makeChange(ChangeType::Create, ChangeOrigin::Remote, "folder2");
    change.isDirectory = true;

    QVERIFY(m_processor->validateChange(change));
}

void TestChangeProcessor::testFolderValidation_SharedDrive() {
    ChangeQueueItem change = makeChange(ChangeType::Create, ChangeOrigin::Remote, "folder3");
    change.isDirectory = true;

    QVERIFY(m_processor->validateChange(change));
}

void TestChangeProcessor::testFolderValidation_MultipleParents() {
    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Remote, "folder4");
    change.isDirectory = true;

    QVERIFY(m_processor->validateChange(change));
}

void TestChangeProcessor::testLocalOrigin_CacheHit() {
    FileSyncState state;
    state.localPath = "local.txt";
    state.fileId = "file-8";
    state.modifiedTimeAtSync = QDateTime::currentDateTime().addSecs(-20);
    m_db->saveFileState(state);

    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Local, "local.txt");
    change.modifiedTime = QDateTime::currentDateTime();

    QVERIFY(m_processor->validateChange(change));
}

void TestChangeProcessor::testLocalOrigin_CacheMiss() {
    FileSyncState state;
    state.localPath = "local2.txt";
    state.fileId = "file-9";
    state.modifiedTimeAtSync = QDateTime::currentDateTime().addSecs(-1);
    m_db->saveFileState(state);

    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Local, "local2.txt");
    change.modifiedTime = QDateTime::currentDateTime();

    QVERIFY(!m_processor->validateChange(change));
}

void TestChangeProcessor::testLocalOrigin_StaleDataHandling() {
    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Local, "local3.txt");
    change.modifiedTime = QDateTime::currentDateTime();

    QVERIFY(m_processor->validateChange(change));
}

void TestChangeProcessor::testEmptyChange() {
    ChangeQueueItem change;
    QVERIFY(!change.isValid());

    m_changeQueue->enqueue(change);
    QCOMPARE(m_changeQueue->count(), 0);
}

void TestChangeProcessor::testMalformedChange() {
    ChangeQueueItem change;
    change.origin = ChangeOrigin::Remote;
    change.changeType = ChangeType::Modify;
    change.localPath.clear();
    change.fileId.clear();

    QVERIFY(!change.isValid());
    m_changeQueue->enqueue(change);
    QCOMPARE(m_changeQueue->count(), 0);
}

void TestChangeProcessor::testInOperationChangeSkipped() {
    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Remote, "busy.txt");
    m_processor->markFileInOperation("busy.txt");

    QVERIFY(!m_processor->validateChange(change));

    m_processor->unmarkFileInOperation("busy.txt");
}

void TestChangeProcessor::testWakeOnItemsAvailable() {
    m_processor->setSyncFolder(m_tempDir->path());
    m_processor->start();

    QTRY_VERIFY(!m_processor->m_processingActive);

    QSignalSpy processedSpy(m_processor, &ChangeProcessor::changeProcessed);
    QSignalSpy skippedSpy(m_processor, &ChangeProcessor::changeSkipped);

    QString relPath = "wake.txt";
    QString absPath = m_tempDir->filePath(relPath);
    QFile file(absPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("data") > 0);
    file.close();

    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Local, relPath);
    m_changeQueue->enqueue(change);

    QTRY_COMPARE(processedSpy.count(), 1);
    QCOMPARE(skippedSpy.count(), 0);
}

void TestChangeProcessor::testValidateChange_AllTypesLocal() {
    const QDateTime baseTime = QDateTime::currentDateTime().addSecs(-10);

    saveState("local-existing.txt", "local-id-1", baseTime);

    ChangeQueueItem createChange =
        makeChange(ChangeType::Create, ChangeOrigin::Local, "local-new.txt");
    createChange.modifiedTime = baseTime.addSecs(5);
    QVERIFY(m_processor->validateChange(createChange));

    ChangeQueueItem modifyChange =
        makeChange(ChangeType::Modify, ChangeOrigin::Local, "local-existing.txt");
    modifyChange.modifiedTime = baseTime.addSecs(5);
    QVERIFY(m_processor->validateChange(modifyChange));

    ChangeQueueItem deleteChange =
        makeChange(ChangeType::Delete, ChangeOrigin::Local, "local-existing.txt");
    QVERIFY(m_processor->validateChange(deleteChange));

    ChangeQueueItem moveChange =
        makeChange(ChangeType::Move, ChangeOrigin::Local, "local-existing.txt");
    moveChange.moveDestination = "moved/local-existing.txt";
    QVERIFY(m_processor->validateChange(moveChange));

    ChangeQueueItem renameChange =
        makeChange(ChangeType::Rename, ChangeOrigin::Local, "local-existing.txt");
    renameChange.renameTo = "local-renamed.txt";
    QVERIFY(m_processor->validateChange(renameChange));
}

void TestChangeProcessor::testValidateChange_AllTypesRemote() {
    const QDateTime baseTime = QDateTime::currentDateTime().addSecs(-10);

    saveState("remote-existing.txt", "remote-id-1", baseTime);

    ChangeQueueItem createChange =
        makeChange(ChangeType::Create, ChangeOrigin::Remote, "remote-new.txt", "remote-id-new");
    createChange.modifiedTime = baseTime.addSecs(5);
    QVERIFY(m_processor->validateChange(createChange));

    ChangeQueueItem modifyChange =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, "remote-existing.txt", "remote-id-1");
    modifyChange.modifiedTime = baseTime.addSecs(5);
    QVERIFY(m_processor->validateChange(modifyChange));

    ChangeQueueItem deleteChange =
        makeChange(ChangeType::Delete, ChangeOrigin::Remote, "remote-existing.txt", "remote-id-1");
    QVERIFY(m_processor->validateChange(deleteChange));

    ChangeQueueItem moveChange =
        makeChange(ChangeType::Move, ChangeOrigin::Remote, "remote-existing.txt", "remote-id-1");
    moveChange.moveDestination = "remote/moved-existing.txt";
    QVERIFY(m_processor->validateChange(moveChange));

    ChangeQueueItem renameChange =
        makeChange(ChangeType::Rename, ChangeOrigin::Remote, "remote-existing.txt", "remote-id-1");
    renameChange.renameTo = "remote-renamed.txt";
    QVERIFY(m_processor->validateChange(renameChange));
}

void TestChangeProcessor::testValidateChange_RemotePathChangeAllowsStaleMtime() {
    const QDateTime baseTime = QDateTime::currentDateTime().addSecs(-10);
    saveState("old/path.txt", "remote-id-2", baseTime);

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, "new/path.txt", "remote-id-2");
    change.modifiedTime = baseTime;

    QVERIFY(m_processor->validateChange(change));
}

void TestChangeProcessor::testValidateChange_LocalMoveMissingFileIdRejected() {
    ChangeQueueItem change = makeChange(ChangeType::Move, ChangeOrigin::Local, "wario.png");
    change.moveDestination = "mario/wario.png";

    QVERIFY(!m_processor->validateChange(change));
}

void TestChangeProcessor::testValidateChange_LocalRenameMissingFileIdRejected() {
    ChangeQueueItem change = makeChange(ChangeType::Rename, ChangeOrigin::Local, "wario.png");
    change.renameTo = "wario2.png";

    QVERIFY(!m_processor->validateChange(change));
}

void TestChangeProcessor::testValidateChange_DuplicatePendingActionSkipped() {
    const QDateTime baseTime = QDateTime::currentDateTime().addSecs(-20);
    saveState("dupe.txt", "dupe-id", baseTime);

    SyncActionItem pending;
    pending.actionType = SyncActionType::Upload;
    pending.localPath = "dupe.txt";
    pending.fileId = "dupe-id";
    pending.modifiedTime = QDateTime::currentDateTime();
    m_actionQueue->enqueue(pending);

    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Local, "dupe.txt");
    change.fileId = "dupe-id";
    change.modifiedTime = QDateTime::currentDateTime();

    QSignalSpy skippedSpy(m_processor, &ChangeProcessor::changeSkipped);
    QVERIFY(!m_processor->validateChange(change));
    QCOMPARE(skippedSpy.count(), 1);
    const QList<QVariant> firstSignalArgs = skippedSpy.takeFirst();
    QCOMPARE(firstSignalArgs.at(0).toString(), QString("dupe.txt"));
    QVERIFY(firstSignalArgs.at(1).toString().contains("duplicate"));
}

void TestChangeProcessor::testValidateChange_LocalModifySameHashSkipped() {
    const QString relPath = "same-hash.txt";
    const QString absPath = m_tempDir->filePath(relPath);
    const QByteArray data("same-content");

    QFile file(absPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(data) > 0);
    file.close();

    FileSyncState state;
    state.localPath = relPath;
    state.fileId = "same-hash-id";
    state.modifiedTimeAtSync = QDateTime::currentDateTime().addSecs(-120);
    state.localHashAtSync = md5Hex(data);
    state.remoteMd5AtSync = md5Hex(data);
    m_db->saveFileState(state);

    ChangeQueueItem change = makeChange(ChangeType::Modify, ChangeOrigin::Local, relPath);
    change.fileId = "same-hash-id";

    QVERIFY(!m_processor->validateChange(change));
}

void TestChangeProcessor::testDetermineAndQueueActions_LocalMoveMissingFileIdSkipped() {
    m_actionQueue->clear();

    ChangeQueueItem change = makeChange(ChangeType::Move, ChangeOrigin::Local, "wario.png");
    change.moveDestination = "mario/wario.png";

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 0);
}

void TestChangeProcessor::testDetermineAndQueueActions_LocalRenameMissingFileIdSkipped() {
    m_actionQueue->clear();

    ChangeQueueItem change = makeChange(ChangeType::Rename, ChangeOrigin::Local, "wario.png");
    change.renameTo = "wario2.png";

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 0);
}

void TestChangeProcessor::testDetermineAndQueueActions_DuplicateSuppressed() {
    m_actionQueue->clear();

    ChangeQueueItem first =
        makeChange(ChangeType::Modify, ChangeOrigin::Local, "queue-dupe.txt", "queue-dupe-id");
    QSignalSpy skippedSpy(m_processor, &ChangeProcessor::changeSkipped);

    m_processor->determineAndQueueActions(first);
    QCOMPARE(m_actionQueue->count(), 1);
    QCOMPARE(skippedSpy.count(), 0);

    ChangeQueueItem duplicate =
        makeChange(ChangeType::Modify, ChangeOrigin::Local, "./queue-dupe.txt", "queue-dupe-id");
    m_processor->determineAndQueueActions(duplicate);

    QCOMPARE(m_actionQueue->count(), 1);
    QCOMPARE(skippedSpy.count(), 1);
    const QList<QVariant> firstSignalArgs = skippedSpy.takeFirst();
    QVERIFY(firstSignalArgs.at(1).toString().contains("duplicate"));
}

void TestChangeProcessor::testDetermineAndQueueActions_RemoteModifySameHashSkipped() {
    m_actionQueue->clear();

    ChangeQueueItem change =
        makeChange(ChangeType::Modify, ChangeOrigin::Remote, "remote-noop.txt", "remote-noop-id");
    change.remoteMd5 = md5Hex("same-content");
    change.localContentHash = md5Hex("same-content");

    m_processor->determineAndQueueActions(change);

    QCOMPARE(m_actionQueue->count(), 0);
}

QTEST_MAIN(TestChangeProcessor)
#include "TestChangeProcessor.moc"
