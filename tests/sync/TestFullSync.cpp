/**
 * @file TestFullSync.cpp
 * @brief Unit tests for FullSync subsystem
 *
 * Tests cover: lifecycle / state machine, local scan, remote tree building,
 * orphan handling, ignore patterns, skip logic, progress signals,
 * cancellation, and error handling.
 */

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/FileFilter.h"
#include "sync/FullSync.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncDatabase.h"
#include "sync/SyncSettings.h"

// ---------------------------------------------------------------------------
//  Fake Google Drive client for FullSync tests
// ---------------------------------------------------------------------------
class FakeDriveClientForFS : public GoogleDriveClient {
    Q_OBJECT

   public:
    explicit FakeDriveClientForFS(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    // Pre-loaded file list pages
    struct FilePage {
        QList<DriveFile> files;
        QString nextPageToken;  // empty on last page
    };
    QList<FilePage> filePages;
    int listFilesCallCount = 0;

    void listFiles(const QString& /*folderId*/ = "root",
                   const QString& pageToken = QString()) override {
        ++listFilesCallCount;
        int pageIdx = 0;
        if (!pageToken.isEmpty()) {
            pageIdx = pageToken.toInt();
        }
        QTimer::singleShot(0, this, [this, pageIdx]() {
            if (pageIdx >= 0 && pageIdx < filePages.size()) {
                emit filesListed(filePages[pageIdx].files, filePages[pageIdx].nextPageToken);
            } else {
                emit filesListed({}, QString());
            }
        });
    }

    QString getRootFolderId() override { return m_rootFolderId; }
    void setRootFolderId(const QString& id) { m_rootFolderId = id; }

    // Stubs for virtual methods we don't test here
    void getFile(const QString&) override {}
    void downloadFile(const QString&, const QString&) override {}
    void uploadFile(const QString&, const QString&, const QString&) override {}
    void updateFile(const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void renameFile(const QString&, const QString&) override {}
    void deleteFile(const QString&) override {}
    void createFolder(const QString&, const QString&, const QString&) override {}
    void listChanges(const QString&) override {}
    void getStartPageToken() override {}
    void getAboutInfo() override {}

    void emitError(const QString& op, const QString& msg) { emit error(op, msg); }

   private:
    QString m_rootFolderId = "root-id-123";
};

// ---------------------------------------------------------------------------
//  Helper to build DriveFile
// ---------------------------------------------------------------------------
static DriveFile makeFile(const QString& id, const QString& name, const QString& parentId,
                          bool isFolder = false, bool trashed = false, bool ownedByMe = true,
                          const QString& mimeType = "text/plain") {
    DriveFile f;
    f.id = id;
    f.name = name;
    f.parents = {parentId};
    f.isFolder = isFolder;
    f.trashed = trashed;
    f.ownedByMe = ownedByMe;
    f.mimeType = mimeType;
    f.modifiedTime = QDateTime::currentDateTimeUtc();
    return f;
}

// ---------------------------------------------------------------------------
//  Test class
// ---------------------------------------------------------------------------
class TestFullSync : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // State machine / lifecycle
    void testInitialState();
    void testStartTransitionsToScanning();
    void testCompletesWhenLocalOnly();
    void testCompletesWithRemoteFiles();
    void testDoubleStartIgnored();

    // Local scan
    void testLocalScanQueuesFiles();
    void testLocalScanQueuesSubdirectories();
    void testLocalScanEmptyFolder();

    // Remote tree building
    void testRemoteTreeSingleFile();
    void testRemoteTreeNestedFolder();
    void testRemoteMultiPageFetch();

    // Orphan handling
    void testOrphansNotQueued();
    void testOrphanCountReported();

    // Ignore patterns / skip logic (FileFilter)
    void testIgnoresHiddenFiles();
    void testIgnoresTmpExtension();
    void testFileFilterSkipsGoogleDoc();
    void testFileFilterSkipsSharedFile();
    void testFileFilterAllowsNormalFile();
    void testFileFilterAllowsTrashedFile();

    // Progress signals
    void testProgressSignalsEmitted();
    void testCompletedSignalCountsCorrect();

    // Cancellation
    void testCancelStopsSync();
    void testCancelIdempotent();

    // Error handling
    void testDriveErrorTransitionsToError();
    void testDriveErrorIgnoredWhenIdle();

    // Sync folder validation
    void testStartWithoutSyncFolderEmitsError();
    void testStartWithMissingSyncFolderEmitsError();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    ChangeQueue* m_changeQueue = nullptr;
    SyncActionQueue* m_syncActionQueue = nullptr;
    SyncDatabase* m_syncDatabase = nullptr;
    FakeDriveClientForFS* m_driveClient = nullptr;
    ChangeProcessor* m_changeProcessor = nullptr;
    FullSync* m_fullSync = nullptr;
    QByteArray m_originalHome;
};

void TestFullSync::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    m_originalHome = qgetenv("HOME");
    qputenv("HOME", m_tempDir->path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    m_syncDatabase = new SyncDatabase();
    QVERIFY(m_syncDatabase->initialize());

    m_changeQueue = new ChangeQueue();
    m_syncActionQueue = new SyncActionQueue();
    m_driveClient = new FakeDriveClientForFS();

    m_changeProcessor =
        new ChangeProcessor(m_changeQueue, m_syncActionQueue, m_syncDatabase, m_driveClient);

    m_fullSync = new FullSync(m_changeQueue, m_syncDatabase, m_driveClient, m_changeProcessor);
}

void TestFullSync::cleanup() {
    delete m_fullSync;
    m_fullSync = nullptr;
    delete m_changeProcessor;
    m_changeProcessor = nullptr;
    delete m_driveClient;
    m_driveClient = nullptr;
    delete m_syncActionQueue;
    m_syncActionQueue = nullptr;
    delete m_changeQueue;
    m_changeQueue = nullptr;
    delete m_syncDatabase;
    m_syncDatabase = nullptr;

    qputenv("HOME", m_originalHome);
    QStandardPaths::setTestModeEnabled(false);

    delete m_tempDir;
    m_tempDir = nullptr;
}

// ---------------------------------------------------------------------------
//  State machine / lifecycle
// ---------------------------------------------------------------------------
void TestFullSync::testInitialState() {
    QCOMPARE(m_fullSync->state(), FullSync::State::Idle);
    QVERIFY(!m_fullSync->isRunning());
}

void TestFullSync::testStartTransitionsToScanning() {
    // Create a sync folder
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy stateSpy(m_fullSync, &FullSync::stateChanged);
    m_fullSync->fullSyncLocal();

    // Should transition to ScanningLocal
    QVERIFY(stateSpy.count() >= 1);
    auto firstState = stateSpy.first().first().value<FullSync::State>();
    QCOMPARE(firstState, FullSync::State::ScanningLocal);
}

void TestFullSync::testCompletesWhenLocalOnly() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSyncLocal();

    QTRY_VERIFY(completedSpy.count() >= 1);
    QCOMPARE(m_fullSync->state(), FullSync::State::Complete);
}

void TestFullSync::testCompletesWithRemoteFiles() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    // Set up a simple file list response (one page, no more)
    FakeDriveClientForFS::FilePage page;
    page.files.append(makeFile("f1", "doc.txt", "root-id-123"));
    page.nextPageToken = "";
    m_driveClient->filePages.append(page);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSync();  // full sync (local + remote)

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 2000);
    QCOMPARE(m_fullSync->state(), FullSync::State::Complete);
}

void TestFullSync::testDoubleStartIgnored() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    // Start first sync
    m_fullSync->fullSyncLocal();

    // Second start while running should be ignored (no crash)
    m_fullSync->fullSyncLocal();

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    QTRY_VERIFY(completedSpy.count() >= 1);
}

// ---------------------------------------------------------------------------
//  Local scan
// ---------------------------------------------------------------------------
void TestFullSync::testLocalScanQueuesFiles() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);

    // Create 3 files
    for (int i = 0; i < 3; ++i) {
        QFile f(syncDir + "/file" + QString::number(i) + ".txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("content");
        f.close();
    }

    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSyncLocal();

    QTRY_VERIFY(completedSpy.count() >= 1);

    // All 3 files should be in the queue
    int count = 0;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        QCOMPARE(item.origin, ChangeOrigin::Local);
        QCOMPARE(item.changeType, ChangeType::Modify);
        ++count;
    }
    QCOMPARE(count, 3);
}

void TestFullSync::testLocalScanQueuesSubdirectories() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir + "/subdir");

    QFile f(syncDir + "/subdir/nested.txt");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("data");
    f.close();

    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSyncLocal();

    QTRY_VERIFY(completedSpy.count() >= 1);

    // Should have 2 items: the subdir + the file
    int count = 0;
    bool foundDir = false;
    bool foundFile = false;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.localPath == "subdir") foundDir = true;
        if (item.localPath == "subdir/nested.txt") foundFile = true;
        ++count;
    }
    QCOMPARE(count, 2);
    QVERIFY(foundDir);
    QVERIFY(foundFile);
}

void TestFullSync::testLocalScanEmptyFolder() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);

    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSyncLocal();

    QTRY_VERIFY(completedSpy.count() >= 1);

    // Local count should be 0
    auto args = completedSpy.first();
    QCOMPARE(args.at(0).toInt(), 0);  // localCount
    QVERIFY(m_changeQueue->isEmpty());
}

// ---------------------------------------------------------------------------
//  Remote tree building
// ---------------------------------------------------------------------------
void TestFullSync::testRemoteTreeSingleFile() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    FakeDriveClientForFS::FilePage page;
    page.files.append(makeFile("f1", "report.txt", "root-id-123"));
    page.nextPageToken = "";
    m_driveClient->filePages.append(page);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSync();

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 2000);

    // Check that the remote file was queued
    bool foundRemote = false;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.origin == ChangeOrigin::Remote && item.localPath == "report.txt") {
            foundRemote = true;
            QCOMPARE(item.fileId, QString("f1"));
        }
    }
    QVERIFY(foundRemote);
}

void TestFullSync::testRemoteTreeNestedFolder() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    // Build a folder → file hierarchy
    FakeDriveClientForFS::FilePage page;
    page.files.append(makeFile("folder-1", "docs", "root-id-123", /*isFolder=*/true));
    page.files.append(makeFile("f2", "readme.md", "folder-1"));
    page.nextPageToken = "";
    m_driveClient->filePages.append(page);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSync();

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 2000);

    // Nested file should be queued with correct relative path
    bool foundNested = false;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.localPath == "docs/readme.md") {
            foundNested = true;
            QCOMPARE(item.origin, ChangeOrigin::Remote);
        }
    }
    QVERIFY(foundNested);
}

void TestFullSync::testRemoteMultiPageFetch() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    // Page 0
    FakeDriveClientForFS::FilePage page0;
    page0.files.append(makeFile("f1", "file1.txt", "root-id-123"));
    page0.nextPageToken = "1";  // next page index
    m_driveClient->filePages.append(page0);

    // Page 1 (final)
    FakeDriveClientForFS::FilePage page1;
    page1.files.append(makeFile("f2", "file2.txt", "root-id-123"));
    page1.nextPageToken = "";
    m_driveClient->filePages.append(page1);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSync();

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 2000);

    // Both files should be queued
    int remoteCount = 0;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.origin == ChangeOrigin::Remote) ++remoteCount;
    }
    QCOMPARE(remoteCount, 2);
}

// ---------------------------------------------------------------------------
//  Orphan handling
// ---------------------------------------------------------------------------
void TestFullSync::testOrphansNotQueued() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    // Create a file with an unknown parent (orphan)
    FakeDriveClientForFS::FilePage page;
    page.files.append(makeFile("orphan-1", "lost.txt", "unknown-parent-id"));
    page.nextPageToken = "";
    m_driveClient->filePages.append(page);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSync();

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 2000);

    // Orphan should NOT be queued
    bool foundOrphan = false;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.fileId == "orphan-1") foundOrphan = true;
    }
    QVERIFY(!foundOrphan);
}

void TestFullSync::testOrphanCountReported() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    FakeDriveClientForFS::FilePage page;
    // One real file + one orphan
    page.files.append(makeFile("f1", "real.txt", "root-id-123"));
    page.files.append(makeFile("orphan-1", "orphan.txt", "nonexistent-parent"));
    page.nextPageToken = "";
    m_driveClient->filePages.append(page);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSync();

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 2000);

    // remoteCount includes orphans (known TODO in FullSync.cpp:437)
    // The orphan file is still counted but never queued
    auto args = completedSpy.first();
    int remoteCount = args.at(1).toInt();
    QCOMPARE(remoteCount, 2);  // both counted, but orphan not queued

    // Verify orphan was NOT actually queued
    bool foundOrphan = false;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.fileId == "orphan-1") foundOrphan = true;
    }
    QVERIFY(!foundOrphan);
}

// ---------------------------------------------------------------------------
//  Ignore patterns / FileFilter
// ---------------------------------------------------------------------------
void TestFullSync::testIgnoresHiddenFiles() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);

    // Create a hidden file (starts with .)
    QFile f(syncDir + "/.hidden");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("secret");
    f.close();

    // Also a normal file
    QFile f2(syncDir + "/visible.txt");
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("hello");
    f2.close();

    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSyncLocal();

    QTRY_VERIFY(completedSpy.count() >= 1);

    // Hidden file should be ignored (.*  pattern)
    bool foundHidden = false;
    bool foundVisible = false;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.localPath == ".hidden") foundHidden = true;
        if (item.localPath == "visible.txt") foundVisible = true;
    }
    QVERIFY(!foundHidden);
    QVERIFY(foundVisible);
}

void TestFullSync::testIgnoresTmpExtension() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);

    QFile f(syncDir + "/scratch.tmp");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("temp");
    f.close();

    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSyncLocal();

    QTRY_VERIFY(completedSpy.count() >= 1);

    bool foundTmp = false;
    while (!m_changeQueue->isEmpty()) {
        ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.localPath == "scratch.tmp") foundTmp = true;
    }
    QVERIFY(!foundTmp);
}

void TestFullSync::testFileFilterSkipsGoogleDoc() {
    SyncSettings settings = SyncSettings::load();
    DriveFile googleDoc;
    googleDoc.mimeType = "application/vnd.google-apps.document";
    googleDoc.ownedByMe = true;
    QVERIFY(FileFilter::shouldSkipRemoteFile(googleDoc, settings));
}

void TestFullSync::testFileFilterSkipsSharedFile() {
    SyncSettings settings = SyncSettings::load();
    DriveFile shared;
    shared.mimeType = "text/plain";
    shared.ownedByMe = false;
    QVERIFY(FileFilter::shouldSkipRemoteFile(shared, settings));
}

void TestFullSync::testFileFilterAllowsNormalFile() {
    SyncSettings settings = SyncSettings::load();
    DriveFile normal;
    normal.mimeType = "text/plain";
    normal.ownedByMe = true;
    QVERIFY(!FileFilter::shouldSkipRemoteFile(normal, settings));
}

void TestFullSync::testFileFilterAllowsTrashedFile() {
    SyncSettings settings = SyncSettings::load();
    DriveFile trashed;
    trashed.mimeType = "text/plain";
    trashed.ownedByMe = true;
    trashed.trashed = true;
    // Trashed files are NOT skipped - we need to detect deletions
    QVERIFY(!FileFilter::shouldSkipRemoteFile(trashed, settings));
}

// ---------------------------------------------------------------------------
//  Progress signals
// ---------------------------------------------------------------------------
void TestFullSync::testProgressSignalsEmitted() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);

    QFile f(syncDir + "/data.txt");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello");
    f.close();

    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy progressSpy(m_fullSync, &FullSync::progressUpdated);
    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSyncLocal();

    QTRY_VERIFY(completedSpy.count() >= 1);
    QVERIFY(progressSpy.count() >= 1);

    // Last progress signal should indicate completion
    auto lastArgs = progressSpy.last();
    QVERIFY(lastArgs.at(0).toString().contains("complete", Qt::CaseInsensitive));
}

void TestFullSync::testCompletedSignalCountsCorrect() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);

    // Create 2 local files
    for (int i = 0; i < 2; ++i) {
        QFile f(syncDir + "/local" + QString::number(i) + ".txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
    }

    // Set up 1 remote file
    FakeDriveClientForFS::FilePage page;
    page.files.append(makeFile("r1", "remote.txt", "root-id-123"));
    page.nextPageToken = "";
    m_driveClient->filePages.append(page);

    m_fullSync->setSyncFolder(syncDir);

    QSignalSpy completedSpy(m_fullSync, &FullSync::completed);
    m_fullSync->fullSync();

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.count() >= 1, 2000);

    auto args = completedSpy.first();
    int localCount = args.at(0).toInt();
    int remoteCount = args.at(1).toInt();
    QCOMPARE(localCount, 2);
    QCOMPARE(remoteCount, 1);
}

// ---------------------------------------------------------------------------
//  Cancellation
// ---------------------------------------------------------------------------
void TestFullSync::testCancelStopsSync() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    m_fullSync->fullSyncLocal();
    m_fullSync->cancel();

    QCOMPARE(m_fullSync->state(), FullSync::State::Idle);
    QVERIFY(!m_fullSync->isRunning());
}

void TestFullSync::testCancelIdempotent() {
    // Calling cancel on idle should be harmless
    m_fullSync->cancel();
    QCOMPARE(m_fullSync->state(), FullSync::State::Idle);

    // Cancel twice while running
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);
    m_fullSync->fullSyncLocal();
    m_fullSync->cancel();
    m_fullSync->cancel();  // second cancel should be harmless
    QCOMPARE(m_fullSync->state(), FullSync::State::Idle);
}

// ---------------------------------------------------------------------------
//  Error handling
// ---------------------------------------------------------------------------
void TestFullSync::testDriveErrorTransitionsToError() {
    QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);
    m_fullSync->setSyncFolder(syncDir);

    // Set up empty remote file list so we reach FetchingRemote state
    FakeDriveClientForFS::FilePage page;
    page.nextPageToken = "";
    m_driveClient->filePages.append(page);

    m_fullSync->fullSync();

    // Wait until we're in FetchingRemote state
    QTRY_COMPARE_WITH_TIMEOUT(m_fullSync->state(), FullSync::State::FetchingRemote, 1000);

    QSignalSpy errorSpy(m_fullSync, &FullSync::error);

    // Emit a listFiles error from fake client
    m_driveClient->emitError("listFiles", "quota exceeded");

    QTRY_VERIFY(errorSpy.count() >= 1);
    QCOMPARE(m_fullSync->state(), FullSync::State::Error);
}

void TestFullSync::testDriveErrorIgnoredWhenIdle() {
    QSignalSpy errorSpy(m_fullSync, &FullSync::error);

    // Error while idle should be ignored
    m_driveClient->emitError("listFiles", "random error");

    QCoreApplication::processEvents();
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(m_fullSync->state(), FullSync::State::Idle);
}

// ---------------------------------------------------------------------------
//  Sync folder validation
// ---------------------------------------------------------------------------
void TestFullSync::testStartWithoutSyncFolderEmitsError() {
    QSignalSpy errorSpy(m_fullSync, &FullSync::error);
    m_fullSync->fullSync();

    QTRY_VERIFY(errorSpy.count() >= 1);
    QVERIFY(errorSpy.first().first().toString().contains("sync folder", Qt::CaseInsensitive));
}

void TestFullSync::testStartWithMissingSyncFolderEmitsError() {
    m_fullSync->setSyncFolder("/nonexistent/path/12345");

    QSignalSpy errorSpy(m_fullSync, &FullSync::error);
    m_fullSync->fullSync();

    QTRY_VERIFY(errorSpy.count() >= 1);
    QVERIFY(errorSpy.first().first().toString().contains("does not exist", Qt::CaseInsensitive));
}

QTEST_MAIN(TestFullSync)
#include "TestFullSync.moc"
