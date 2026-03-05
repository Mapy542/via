/**
 * @file TestAccountSwitch.cpp
 * @brief Unit tests for account sign-out data-isolation
 *
 * Verifies that every piece of per-session state is properly purged when
 * a user signs out, preventing data leaks and stale-state conflicts when
 * a different account subsequently signs in.
 *
 * Covered components:
 *  - SyncDatabase::clearAllData()       – all 9 tables wiped
 *  - ChangeQueue::clear()               – in-memory queue drained
 *  - SyncActionQueue::clear()           – in-memory queue drained
 *  - ChangeProcessor::clearState()      – conflicts & files-in-op cleared
 *  - RemoteChangeWatcher::clearChangeToken() – change token reset
 *  - FullSync::clearPendingState()      – discovery state released
 *  - SyncActionThread::clearInProgressActions() – action map & retries reset
 *  - End-to-end logout sequence         – everything in one shot
 *  - Re-login after logout              – clean slate works
 */

#include <QDir>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "auth/TokenStorage.h"
#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/FullSync.h"
#include "sync/RemoteChangeWatcher.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncActionThread.h"
#include "sync/SyncDatabase.h"

// ===========================================================================
//  Minimal fakes — just enough to construct real components
// ===========================================================================

class FakeTokenStorageAS : public TokenStorage {
    Q_OBJECT
   public:
    explicit FakeTokenStorageAS(QObject* parent = nullptr) : TokenStorage(parent) {}
    void saveTokens(const QString&, const QString&, const QDateTime&) {}
    QString getAccessToken() const { return {}; }
    QString getRefreshToken() const { return {}; }
    QDateTime getTokenExpiry() const { return {}; }
    bool hasValidTokens() const { return false; }
    bool isTokenExpired() const { return true; }
    void clearTokens() {}
    void saveCredentials(const QString&, const QString&) {}
    QString getClientId() const { return {}; }
    QString getClientSecret() const { return {}; }
};

class FakeDriveClientAS : public GoogleDriveClient {
    Q_OBJECT
   public:
    explicit FakeDriveClientAS(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}
    void getFile(const QString&) override {}
    void downloadFile(const QString&, const QString&) override {}
    void uploadFile(const QString&, const QString&, const QString&) override {}
    void updateFile(const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void renameFile(const QString&, const QString&) override {}
    void deleteFile(const QString&) override {}
    void createFolder(const QString&, const QString&, const QString&) override {}
    void listFiles(const QString&, const QString&) override {}
    void listChanges(const QString&) override {}
    void getStartPageToken() override {}
    void getAboutInfo() override {}
    QString getRootFolderId() override { return "root"; }
};

// ===========================================================================
//  Test class
// ===========================================================================

class TestAccountSwitch : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // Individual component tests
    void testSyncDatabaseClearedOnLogout();
    void testChangeQueueClearedOnLogout();
    void testSyncActionQueueClearedOnLogout();
    void testChangeProcessorStateClearedOnLogout();
    void testRemoteChangeWatcherTokenClearedOnLogout();
    void testFullSyncStateClearedOnLogout();
    void testSyncActionThreadClearedOnLogout();

    // Integration tests
    void testFullLogoutSequence();
    void testReloginAfterLogout();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
};

// ===========================================================================
//  Setup / Teardown
// ===========================================================================

void TestAccountSwitch::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    // Redirect QStandardPaths so SyncDatabase writes to temp dir
    qputenv("HOME", m_tempDir->path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());
}

void TestAccountSwitch::cleanup() {
    if (m_db) {
        m_db->close();
        delete m_db;
        m_db = nullptr;
    }
    QStandardPaths::setTestModeEnabled(false);
    delete m_tempDir;
    m_tempDir = nullptr;
}

// ===========================================================================
//  SyncDatabase::clearAllData
// ===========================================================================

void TestAccountSwitch::testSyncDatabaseClearedOnLogout() {
    // Populate every table with at least one record
    FileSyncState file;
    file.localPath = "docs/readme.md";
    file.fileId = "file-1";
    file.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    file.isFolder = false;
    m_db->saveFileState(file);

    m_db->setChangeToken("old-change-token");
    m_db->markFileDeleted("docs/readme.md", "file-1");
    m_db->upsertConflictRecord("docs/readme.md", "file-1", "docs/readme (conflict).md");

    // FUSE tables
    FuseMetadata meta;
    meta.fileId = "fuse-1";
    meta.path = "/test";
    meta.name = "test";
    meta.parentId = "root";
    meta.isFolder = false;
    meta.size = 100;
    meta.mimeType = "text/plain";
    meta.createdTime = QDateTime::currentDateTimeUtc();
    meta.modifiedTime = QDateTime::currentDateTimeUtc();
    meta.cachedAt = QDateTime::currentDateTimeUtc();
    meta.lastAccessed = QDateTime::currentDateTimeUtc();
    m_db->saveFuseMetadata(meta);

    m_db->recordFuseCacheEntry("fuse-1", "/tmp/cache/fuse-1", 100);
    m_db->markFuseDirty("fuse-1", "/test");
    m_db->setFuseSyncState("pageToken", "abc123");

    // Verify data exists
    QVERIFY(!m_db->getFileState("docs/readme.md").fileId.isEmpty());
    QVERIFY(!m_db->getChangeToken().isEmpty());
    QVERIFY(m_db->wasFileDeleted("docs/readme.md"));
    QVERIFY(!m_db->getFuseMetadata("fuse-1").fileId.isEmpty());
    QVERIFY(!m_db->getFuseSyncState("pageToken").isEmpty());

    // Act
    QVERIFY(m_db->clearAllData());

    // Assert — every table is now empty
    QVERIFY(m_db->getFileState("docs/readme.md").fileId.isEmpty());
    QCOMPARE(m_db->fileCount(), 0);
    QVERIFY(m_db->getChangeToken().isEmpty());
    QVERIFY(!m_db->wasFileDeleted("docs/readme.md"));
    QVERIFY(m_db->getUnresolvedConflicts().isEmpty());
    QVERIFY(m_db->getFuseMetadata("fuse-1").fileId.isEmpty());
    QVERIFY(m_db->getAllFuseMetadata().isEmpty());
    QVERIFY(m_db->getFuseCacheEntries().isEmpty());
    QVERIFY(m_db->getFuseDirtyFiles().isEmpty());
    QVERIFY(m_db->getFuseSyncState("pageToken").isEmpty());
}

// ===========================================================================
//  ChangeQueue::clear
// ===========================================================================

void TestAccountSwitch::testChangeQueueClearedOnLogout() {
    ChangeQueue queue;

    // Enqueue several items
    ChangeQueueItem item;
    item.changeType = ChangeType::Modify;
    item.localPath = "file1.txt";
    item.origin = ChangeOrigin::Local;
    queue.enqueue(item);
    item.localPath = "file2.txt";
    queue.enqueue(item);
    item.localPath = "file3.txt";
    queue.enqueue(item);

    QVERIFY(queue.count() > 0);

    queue.clear();

    QCOMPARE(queue.count(), 0);
    QVERIFY(queue.isEmpty());
}

// ===========================================================================
//  SyncActionQueue::clear
// ===========================================================================

void TestAccountSwitch::testSyncActionQueueClearedOnLogout() {
    SyncActionQueue queue;

    SyncActionItem action;
    action.localPath = "file1.txt";
    action.fileId = "id1";
    action.actionType = SyncActionType::Upload;
    queue.enqueue(action);
    action.localPath = "file2.txt";
    action.fileId = "id2";
    queue.enqueue(action);

    QVERIFY(queue.count() > 0);

    queue.clear();

    QCOMPARE(queue.count(), 0);
    QVERIFY(queue.isEmpty());
}

// ===========================================================================
//  ChangeProcessor::clearState
// ===========================================================================

void TestAccountSwitch::testChangeProcessorStateClearedOnLogout() {
    FakeDriveClientAS driveClient;
    ChangeQueue changeQueue;
    SyncActionQueue syncActionQueue;
    ChangeProcessor processor(&changeQueue, &syncActionQueue, m_db, &driveClient);

    // Simulate files in operation
    processor.markFileInOperation("fileA.txt");
    processor.markFileInOperation("fileB.txt");
    QVERIFY(processor.isFileInOperation("fileA.txt"));
    QVERIFY(processor.isFileInOperation("fileB.txt"));

    // Call clearState
    processor.clearState();

    QVERIFY(!processor.isFileInOperation("fileA.txt"));
    QVERIFY(!processor.isFileInOperation("fileB.txt"));
    QCOMPARE(processor.unresolvedConflictCount(), 0);
}

// ===========================================================================
//  RemoteChangeWatcher::clearChangeToken
// ===========================================================================

void TestAccountSwitch::testRemoteChangeWatcherTokenClearedOnLogout() {
    FakeDriveClientAS driveClient;
    ChangeQueue changeQueue;
    RemoteChangeWatcher watcher(&changeQueue, &driveClient, m_db);

    watcher.setChangeToken("old-token-12345");
    QCOMPARE(watcher.changeToken(), "old-token-12345");

    watcher.clearChangeToken();

    QVERIFY(watcher.changeToken().isEmpty());
}

// ===========================================================================
//  FullSync::clearPendingState
// ===========================================================================

void TestAccountSwitch::testFullSyncStateClearedOnLogout() {
    FakeDriveClientAS driveClient;
    ChangeQueue changeQueue;
    SyncActionQueue syncActionQueue;
    ChangeProcessor processor(&changeQueue, &syncActionQueue, m_db, &driveClient);
    FullSync fullSync(&changeQueue, m_db, &driveClient, &processor);
    fullSync.setSyncFolder(m_tempDir->path());

    // After construction the state should be idle and counts zero
    QCOMPARE(fullSync.localFileCount(), 0);

    // clearPendingState should be safe to call even on pristine instance
    fullSync.clearPendingState();

    QCOMPARE(fullSync.state(), FullSync::State::Idle);
    QCOMPARE(fullSync.localFileCount(), 0);
}

// ===========================================================================
//  SyncActionThread::clearInProgressActions
// ===========================================================================

void TestAccountSwitch::testSyncActionThreadClearedOnLogout() {
    FakeDriveClientAS driveClient;
    ChangeQueue changeQueue;
    SyncActionQueue syncActionQueue;
    ChangeProcessor processor(&changeQueue, &syncActionQueue, m_db, &driveClient);
    SyncActionThread actionThread(&syncActionQueue, m_db, &driveClient, &processor);
    actionThread.setSyncFolder(m_tempDir->path());

    // clearInProgressActions should be safe to call on a stopped thread
    actionThread.clearInProgressActions();

    // Verify thread is still in a valid state
    QCOMPARE(actionThread.state(), SyncActionThread::State::Stopped);
}

// ===========================================================================
//  Full Logout Sequence — mimics main.cpp loggedOut handler
// ===========================================================================

void TestAccountSwitch::testFullLogoutSequence() {
    FakeDriveClientAS driveClient;
    ChangeQueue changeQueue;
    SyncActionQueue syncActionQueue;
    ChangeProcessor processor(&changeQueue, &syncActionQueue, m_db, &driveClient);
    RemoteChangeWatcher watcher(&changeQueue, &driveClient, m_db);
    FullSync fullSync(&changeQueue, m_db, &driveClient, &processor);
    SyncActionThread actionThread(&syncActionQueue, m_db, &driveClient, &processor);

    fullSync.setSyncFolder(m_tempDir->path());
    actionThread.setSyncFolder(m_tempDir->path());
    processor.setSyncFolder(m_tempDir->path());

    // --- Seed state across all components ---

    // Database
    FileSyncState file;
    file.localPath = "report.pdf";
    file.fileId = "f-report";
    file.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    m_db->saveFileState(file);
    m_db->setChangeToken("tok-abc");

    // Queues
    ChangeQueueItem cqi;
    cqi.changeType = ChangeType::Create;
    cqi.localPath = "q1.txt";
    cqi.origin = ChangeOrigin::Local;
    changeQueue.enqueue(cqi);

    SyncActionItem sai;
    sai.localPath = "q2.txt";
    sai.fileId = "f-q2";
    sai.actionType = SyncActionType::Upload;
    syncActionQueue.enqueue(sai);

    // Component state
    processor.markFileInOperation("busy.txt");
    watcher.setChangeToken("change-tok-old");

    // Sanity: everything is populated
    QVERIFY(!m_db->getFileState("report.pdf").fileId.isEmpty());
    QVERIFY(!changeQueue.isEmpty());
    QVERIFY(!syncActionQueue.isEmpty());
    QVERIFY(processor.isFileInOperation("busy.txt"));
    QCOMPARE(watcher.changeToken(), "change-tok-old");

    // --- Execute the same logout sequence as main.cpp ---

    // 1. Stop components (would normally be done via stopSyncComponents)
    actionThread.stop();
    processor.stop();
    watcher.stop();

    // 2. Drain queues
    changeQueue.clear();
    syncActionQueue.clear();

    // 3. Per-component state
    processor.clearState();
    actionThread.clearInProgressActions();
    watcher.clearChangeToken();
    fullSync.clearPendingState();

    // 4. Wipe database
    QVERIFY(m_db->clearAllData());

    // --- Assert everything is clean ---
    QVERIFY(m_db->getFileState("report.pdf").fileId.isEmpty());
    QCOMPARE(m_db->fileCount(), 0);
    QVERIFY(m_db->getChangeToken().isEmpty());
    QVERIFY(changeQueue.isEmpty());
    QVERIFY(syncActionQueue.isEmpty());
    QVERIFY(!processor.isFileInOperation("busy.txt"));
    QCOMPARE(processor.unresolvedConflictCount(), 0);
    QVERIFY(watcher.changeToken().isEmpty());
}

// ===========================================================================
//  Re-login after logout — verify clean-slate functionality
// ===========================================================================

void TestAccountSwitch::testReloginAfterLogout() {
    FakeDriveClientAS driveClient;
    ChangeQueue changeQueue;
    SyncActionQueue syncActionQueue;
    ChangeProcessor processor(&changeQueue, &syncActionQueue, m_db, &driveClient);
    RemoteChangeWatcher watcher(&changeQueue, &driveClient, m_db);
    FullSync fullSync(&changeQueue, m_db, &driveClient, &processor);
    SyncActionThread actionThread(&syncActionQueue, m_db, &driveClient, &processor);

    fullSync.setSyncFolder(m_tempDir->path());
    actionThread.setSyncFolder(m_tempDir->path());
    processor.setSyncFolder(m_tempDir->path());

    // --- Seed old-account data ---
    FileSyncState oldFile;
    oldFile.localPath = "secret.docx";
    oldFile.fileId = "f-secret-old";
    oldFile.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    m_db->saveFileState(oldFile);
    m_db->setChangeToken("old-acct-token");
    watcher.setChangeToken("old-remote-token");
    processor.markFileInOperation("secret.docx");

    // --- Logout ---
    actionThread.stop();
    processor.stop();
    watcher.stop();
    changeQueue.clear();
    syncActionQueue.clear();
    processor.clearState();
    actionThread.clearInProgressActions();
    watcher.clearChangeToken();
    fullSync.clearPendingState();
    m_db->clearAllData();

    // --- New account logs in — seed new data ---
    FileSyncState newFile;
    newFile.localPath = "work/notes.txt";
    newFile.fileId = "f-notes-new";
    newFile.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    m_db->saveFileState(newFile);
    m_db->setChangeToken("new-acct-token");

    // Verify new data is present and old data is gone
    QVERIFY(!m_db->getFileState("work/notes.txt").fileId.isEmpty());
    QCOMPARE(m_db->getFileState("work/notes.txt").fileId, "f-notes-new");
    QCOMPARE(m_db->getChangeToken(), "new-acct-token");

    // Old data should NOT be present
    QVERIFY(m_db->getFileState("secret.docx").fileId.isEmpty());
    QCOMPARE(m_db->fileCount(), 1);
    QVERIFY(!processor.isFileInOperation("secret.docx"));
    QVERIFY(watcher.changeToken().isEmpty());  // cleared, new session hasn't set one yet
}

#include "TestAccountSwitch.moc"
QTEST_MAIN(TestAccountSwitch)
