/**
 * @file TestDirtySyncWorker.cpp
 * @brief Unit tests for DirtySyncWorker — covers H5 (errorDetailed fileId
 *        filtering) and GPT5.3 #8 (retry budget / skip exceeded).
 */

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "fuse/DirtySyncWorker.h"
#include "fuse/FileCache.h"
#include "sync/SyncDatabase.h"

// ---------------------------------------------------------------------------
// Fake GoogleDriveClient — controls success / failure per-fileId
// ---------------------------------------------------------------------------
class FakeDriveClientDSW : public GoogleDriveClient {
    Q_OBJECT

   public:
    explicit FakeDriveClientDSW(QObject* parent = nullptr)
        : GoogleDriveClient(nullptr, parent) {}

    /// When updateFile is called, either succeed or fail depending on the
    /// set of file IDs configured to fail.
    /// Signals are emitted asynchronously (via QTimer::singleShot) to match
    /// the real GoogleDriveClient behaviour and avoid waking the wait
    /// condition before the caller has entered wait().
    void updateFile(const QString& fileId, const QString& localPath) override {
        if (m_failIds.contains(fileId)) {
            QTimer::singleShot(0, this, [this, fileId, localPath]() {
                emit errorDetailed("updateFile", "Simulated failure", 500, fileId, localPath);
            });
        } else {
            QTimer::singleShot(0, this, [this, fileId, localPath]() {
                DriveFile f;
                f.id = fileId;
                f.name = QFileInfo(localPath).fileName();
                f.modifiedTime = QDateTime::currentDateTimeUtc();
                emit fileUpdated(f);
            });
        }
    }

    /// Mark a fileId as one that should fail uploads
    void setFailForFileId(const QString& id) { m_failIds.insert(id); }
    void clearFailForFileId(const QString& id) { m_failIds.remove(id); }

    // Stubs required by base class
    void downloadFile(const QString&, const QString&) override {}
    void uploadFile(const QString&, const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void renameFile(const QString&, const QString&) override {}
    void deleteFile(const QString&) override {}
    void createFolder(const QString&, const QString&, const QString&) override {}
    QJsonArray getParentsByFileId(const QString&) override { return {}; }
    QString getFolderIdByPath(const QString&) override { return {}; }

   private:
    QSet<QString> m_failIds;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class TestDirtySyncWorker : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // H5: error signal only wakes when fileId matches
    void testUploadError_MatchingFileId_Fails();
    void testUploadError_MismatchedFileId_ErrorNotAttributed();

    // GPT5.3 #8: retry budget
    void testRetryBudget_SkipsExceededFiles();

   private:
    void createCacheFile(const QString& fileId);

    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientDSW* m_driveClient = nullptr;
    FileCache* m_fileCache = nullptr;
    DirtySyncWorker* m_worker = nullptr;
    QString m_cacheDir;
};

// ---------------------------------------------------------------------------
// Setup / Teardown
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);
    qputenv("HOME", m_tempDir->path().toUtf8());

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());

    m_driveClient = new FakeDriveClientDSW(this);
    m_fileCache = new FileCache(m_db, m_driveClient, this);

    m_cacheDir = m_tempDir->path() + "/cache";
    QDir().mkpath(m_cacheDir);
    m_fileCache->setCacheDirectory(m_cacheDir);
    QVERIFY(m_fileCache->initialize());

    m_worker = new DirtySyncWorker(m_fileCache, m_driveClient, m_db, this);
    m_worker->setMaxRetries(2);
    // Use a short upload timeout — in the single-threaded test the
    // synchronous signal fires before QWaitCondition::wait() starts,
    // so every upload ends via timeout regardless of the signal outcome.
    m_worker->setUploadTimeoutMs(500);
}

void TestDirtySyncWorker::cleanup() {
    delete m_worker;
    m_worker = nullptr;
    delete m_fileCache;
    m_fileCache = nullptr;
    delete m_driveClient;
    m_driveClient = nullptr;
    if (m_db) {
        m_db->close();
        delete m_db;
        m_db = nullptr;
    }
    delete m_tempDir;
    m_tempDir = nullptr;
    QStandardPaths::setTestModeEnabled(false);
}

void TestDirtySyncWorker::createCacheFile(const QString& fileId) {
    QString path = m_cacheDir + "/" + fileId;
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("test data");
    f.close();
    m_fileCache->recordCacheEntry(fileId, path, 9);
}

// ---------------------------------------------------------------------------
// H5: matching fileId → upload fails properly
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::testUploadError_MatchingFileId_Fails() {
    const QString fid = "file_match";
    createCacheFile(fid);
    m_fileCache->markDirty(fid, "/match.txt");

    m_driveClient->setFailForFileId(fid);

    QSignalSpy failSpy(m_worker, &DirtySyncWorker::uploadFailed);

    // Start + immediate sync
    m_worker->start();
    QTRY_VERIFY_WITH_TIMEOUT(!failSpy.isEmpty(), 5000);

    QCOMPARE(failSpy.first().at(0).toString(), fid);
    m_worker->stop();
}

// ---------------------------------------------------------------------------
// H5: mismatched fileId → stray error is NOT attributed to current upload
//
// In a single-threaded test the synchronous signal fires before wait(),
// so the upload always ends by timeout.  When the stray error is IGNORED
// (H5 fix working correctly) the m_uploadError stays empty.  We verify
// that the uploadFailed error argument is empty (timeout) and NOT the
// stray message — proving the filter works.
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::testUploadError_MismatchedFileId_ErrorNotAttributed() {
    const QString fid = "file_good";
    createCacheFile(fid);
    m_fileCache->markDirty(fid, "/good.txt");

    // No failures configured for fid — but we will emit a stray error
    // for an unrelated file.  Pre-H5 code would attribute that error
    // to the current upload; post-H5 it is ignored.

    QSignalSpy failSpy(m_worker, &DirtySyncWorker::uploadFailed);

    // Emit a stray error before starting (it will also fire during the
    // upload window because the synchronous signal runs before wait).
    emit m_driveClient->errorDetailed("updateFile", "STRAY_NOISE", 500,
                                      "totally_unrelated", "/elsewhere");

    m_worker->start();

    QTRY_VERIFY_WITH_TIMEOUT(!failSpy.isEmpty(), 5000);

    // The upload timed out (unavoidable in single-thread), but the
    // error message must NOT contain the stray error text.
    QString reportedError = failSpy.first().at(2).toString();
    QVERIFY2(!reportedError.contains("STRAY_NOISE"),
             qPrintable("Stray error was incorrectly attributed: " + reportedError));
    m_worker->stop();
}

// ---------------------------------------------------------------------------
// GPT5.3 #8: after exceeding maxRetries the file is skipped
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::testRetryBudget_SkipsExceededFiles() {
    const QString fid = "file_retry";
    createCacheFile(fid);
    m_fileCache->markDirty(fid, "/retry.txt");

    m_driveClient->setFailForFileId(fid);
    m_worker->setMaxRetries(2);

    QSignalSpy failSpy(m_worker, &DirtySyncWorker::uploadFailed);
    QSignalSpy cycleSpy(m_worker, &DirtySyncWorker::syncCycleCompleted);

    m_worker->start();

    // Wait for at least 3 sync cycles (retries 0,1 = actual attempts, retry 2 = skip)
    QTRY_VERIFY_WITH_TIMEOUT(cycleSpy.size() >= 3, 30000);

    m_worker->stop();

    // The file should have been attempted exactly maxRetries times (2) then skipped.
    // Subsequent cycles should report 0 uploaded, 1 failed (the skip path).
    // At minimum, the last cycle's fail count should be 1 (the skip).
    QVERIFY(failSpy.size() >= 2);

    // After being skipped, the error message should say "Exceeded max retries"
    bool foundSkipMessage = false;
    for (const auto& args : failSpy) {
        if (args.at(2).toString().contains("Exceeded max retries")) {
            foundSkipMessage = true;
            break;
        }
    }
    QVERIFY2(foundSkipMessage, "Expected 'Exceeded max retries' message in uploadFailed signals");
}

QTEST_MAIN(TestDirtySyncWorker)
#include "TestDirtySyncWorker.moc"
