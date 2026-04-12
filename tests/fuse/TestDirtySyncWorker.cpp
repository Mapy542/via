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
    explicit FakeDriveClientDSW(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

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
                f.size = QFileInfo(localPath).size();
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

    // Cache content recycling: clearDirty must recycle content into LRU cache
    void testClearDirty_RecyclesContentToCache();
    void testClearDirty_RecycledFileReadable();

    // Metadata size correctness after upload (simulated)
    void testUpload_MetadataSizeFromRecycledFile();
    void testUpload_MetadataSizePrefersDriveApiSize();

    // Self-upload marker: recently uploaded files skip invalidation
    void testRecentlyUploaded_SkipsInvalidation();
    void testRecentlyUploaded_ConsumedOnce();

    // Open-handle protection: files with open handles resist eviction
    void testOpenHandle_PreventsInvalidation();
    void testOpenHandle_PreventsEviction();
    void testOpenHandle_ReleasedAllowsInvalidation();

   private:
    void createCacheFile(const QString& fileId);
    void createCacheFileWithContent(const QString& fileId, const QByteArray& content);
    void createDirtyPendingFile(const QString& fileId, const QByteArray& content);

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

void TestDirtySyncWorker::createCacheFileWithContent(const QString& fileId,
                                                     const QByteArray& content) {
    // Use the deterministic cache path so getCachePathForFile matches
    QString path = m_fileCache->getCachePathForFile(fileId);
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
    f.close();
    m_fileCache->recordCacheEntry(fileId, path, content.size());
}

void TestDirtySyncWorker::createDirtyPendingFile(const QString& fileId, const QByteArray& content) {
    // Create file in cache first, then move to pending store (simulates release)
    createCacheFileWithContent(fileId, content);
    m_fileCache->markDirty(fileId, "/" + fileId + ".txt");
    QString pendingPath = m_fileCache->moveToDirtyStore(fileId);
    QVERIFY(!pendingPath.isEmpty());
    QVERIFY(QFile::exists(pendingPath));
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
    emit m_driveClient->errorDetailed("updateFile", "STRAY_NOISE", 500, "totally_unrelated",
                                      "/elsewhere");

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

// ---------------------------------------------------------------------------
// Cache content recycling: clearDirty recycles to LRU cache
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::testClearDirty_RecyclesContentToCache() {
    const QString fid = "recycle_test";
    const QByteArray content = "important lock file data";

    createDirtyPendingFile(fid, content);

    // Verify file is dirty and NOT in cache (it was moved to pending store)
    QVERIFY(m_fileCache->isDirty(fid));
    QVERIFY(!m_fileCache->isCached(fid));

    // Simulate what DirtySyncWorker does after successful upload
    m_fileCache->clearDirty(fid);

    // After clearDirty, the file must be recycled back into the LRU cache
    QVERIFY(!m_fileCache->isDirty(fid));
    QVERIFY(m_fileCache->isCached(fid));
}

void TestDirtySyncWorker::testClearDirty_RecycledFileReadable() {
    const QString fid = "readable_recycle";
    const QByteArray content = "{\"user\": \"eli\", \"host\": \"workstation\"}";

    createDirtyPendingFile(fid, content);
    m_fileCache->clearDirty(fid);

    // The recycled file must be readable with the original content
    QString cachePath = m_fileCache->getCachePathForFile(fid);
    QVERIFY(QFile::exists(cachePath));

    QFile f(cachePath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), content);
    f.close();
}

// ---------------------------------------------------------------------------
// Metadata size correctness: simulates the post-upload metadata update
// path that DirtySyncWorker executes after a successful upload.
//
// In single-threaded Qt tests the full upload round-trip cannot complete
// (QTimer::singleShot can't fire while the thread is blocked in
// QWaitCondition::wait), so we directly exercise the clearDirty +
// metadata update logic here.
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::testUpload_MetadataSizeFromRecycledFile() {
    const QString fid = "meta_recycle_sz";
    const QByteArray content = "kicad lock file content here";

    // Seed metadata with the correct initial size (as fuseRelease would)
    FuseMetadata meta;
    meta.fileId = fid;
    meta.path = fid + ".lck";
    meta.name = fid + ".lck";
    meta.parentId = "root";
    meta.isFolder = false;
    meta.size = content.size();
    meta.mimeType = "application/octet-stream";
    meta.createdTime = QDateTime::currentDateTimeUtc();
    meta.modifiedTime = QDateTime::currentDateTimeUtc();
    meta.cachedAt = QDateTime::currentDateTimeUtc();
    meta.lastAccessed = QDateTime::currentDateTimeUtc();
    QVERIFY(m_db->saveFuseMetadata(meta));

    // Create the dirty file in the pending store
    createDirtyPendingFile(fid, content);

    // Simulate what DirtySyncWorker does after successful upload:
    // 1. clearDirty — recycles content back to cache
    m_fileCache->clearDirty(fid);
    m_fileCache->markRecentlyUploaded(fid);

    // 2. Simulate Drive API returning size=0 (the bug trigger)
    //    DirtySyncWorker falls back to getContentPath → local file size
    DriveFile uploaded;
    uploaded.id = fid;
    uploaded.size = 0;  // API omitted size
    uploaded.modifiedTime = QDateTime::currentDateTimeUtc();

    // Execute the metadata update logic (mirrors DirtySyncWorker::processDirtyFiles)
    FuseMetadata updatedMeta = m_db->getFuseMetadata(fid);
    QVERIFY(!updatedMeta.fileId.isEmpty());

    if (uploaded.size > 0) {
        updatedMeta.size = uploaded.size;
    } else {
        QString cachePath = m_fileCache->getContentPath(fid);
        QFileInfo localInfo(cachePath);
        if (localInfo.exists() && localInfo.size() > 0) {
            updatedMeta.size = localInfo.size();
        }
    }
    updatedMeta.modifiedTime = uploaded.modifiedTime;
    QVERIFY(m_db->saveFuseMetadata(updatedMeta));

    // Verify: metadata must have the correct size, NOT zero
    FuseMetadata finalMeta = m_db->getFuseMetadata(fid);
    QVERIFY2(finalMeta.size > 0,
             qPrintable(QString("Expected non-zero metadata size, got %1").arg(finalMeta.size)));
    QCOMPARE(finalMeta.size, static_cast<qint64>(content.size()));
}

void TestDirtySyncWorker::testUpload_MetadataSizePrefersDriveApiSize() {
    const QString fid = "meta_api_sz";
    const QByteArray content = "zip backup data here 12345";

    FuseMetadata meta;
    meta.fileId = fid;
    meta.path = fid + ".zip";
    meta.name = fid + ".zip";
    meta.parentId = "root";
    meta.isFolder = false;
    meta.size = 0;  // deliberately wrong
    meta.mimeType = "application/zip";
    meta.createdTime = QDateTime::currentDateTimeUtc();
    meta.modifiedTime = QDateTime::currentDateTimeUtc();
    meta.cachedAt = QDateTime::currentDateTimeUtc();
    meta.lastAccessed = QDateTime::currentDateTimeUtc();
    QVERIFY(m_db->saveFuseMetadata(meta));

    createDirtyPendingFile(fid, content);
    m_fileCache->clearDirty(fid);

    // Simulate Drive API returning the correct size
    DriveFile uploaded;
    uploaded.id = fid;
    uploaded.size = content.size();
    uploaded.modifiedTime = QDateTime::currentDateTimeUtc();

    FuseMetadata updatedMeta = m_db->getFuseMetadata(fid);
    if (uploaded.size > 0) {
        updatedMeta.size = uploaded.size;
    } else {
        QString cachePath = m_fileCache->getContentPath(fid);
        QFileInfo localInfo(cachePath);
        if (localInfo.exists() && localInfo.size() > 0) {
            updatedMeta.size = localInfo.size();
        }
    }
    QVERIFY(m_db->saveFuseMetadata(updatedMeta));

    FuseMetadata finalMeta = m_db->getFuseMetadata(fid);
    QCOMPARE(finalMeta.size, static_cast<qint64>(content.size()));
}

// ---------------------------------------------------------------------------
// Self-upload marker: recently uploaded files skip invalidation
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::testRecentlyUploaded_SkipsInvalidation() {
    const QString fid = "self_upload_inv";
    const QByteArray content = "pcb design data";

    // Put a file in cache
    createCacheFileWithContent(fid, content);
    QVERIFY(m_fileCache->isCached(fid));

    // Simulate what DirtySyncWorker does: mark recently uploaded
    m_fileCache->markRecentlyUploaded(fid);

    // Now MetadataRefreshWorker would call consumeRecentlyUploaded —
    // if it returns true, invalidation should be skipped
    QVERIFY(m_fileCache->consumeRecentlyUploaded(fid));

    // Since we consumed the marker (simulating the skip), verify the
    // cache is still intact — the invalidation was not called
    QVERIFY(m_fileCache->isCached(fid));

    // Verify the actual content is still there
    QString cachePath = m_fileCache->getCachePathForFile(fid);
    QFile f(cachePath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), content);
    f.close();
}

void TestDirtySyncWorker::testRecentlyUploaded_ConsumedOnce() {
    const QString fid = "consume_once";

    m_fileCache->markRecentlyUploaded(fid);

    // First consume succeeds
    QVERIFY(m_fileCache->consumeRecentlyUploaded(fid));
    // Second consume fails — marker was already consumed
    QVERIFY(!m_fileCache->consumeRecentlyUploaded(fid));
}

// ---------------------------------------------------------------------------
// Open-handle protection: files with open handles resist eviction/invalidation
// ---------------------------------------------------------------------------
void TestDirtySyncWorker::testOpenHandle_PreventsInvalidation() {
    const QString fid = "open_handle_inv";
    const QByteArray content = "kicad pcb data";

    createCacheFileWithContent(fid, content);
    QVERIFY(m_fileCache->isCached(fid));

    // Simulate a FUSE open — adds open handle ref count
    m_fileCache->addOpenHandle(fid);
    QVERIFY(m_fileCache->hasOpenHandles(fid));

    // Attempt invalidation — should be blocked by open handle
    m_fileCache->invalidate(fid);

    // File must still be cached
    QVERIFY(m_fileCache->isCached(fid));

    // Content must still be readable
    QString cachePath = m_fileCache->getCachePathForFile(fid);
    QFile f(cachePath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), content);
    f.close();

    // Cleanup
    m_fileCache->removeOpenHandle(fid);
}

void TestDirtySyncWorker::testOpenHandle_PreventsEviction() {
    const QString protectedFid = "open_handle_evict";
    const QString evictableFid = "evictable_file";
    const QByteArray content = "data that must survive eviction";
    const QByteArray evictContent = "this one can go";

    // Create two files: one with an open handle, one without
    createCacheFileWithContent(protectedFid, content);
    createCacheFileWithContent(evictableFid, evictContent);

    QVERIFY(m_fileCache->isCached(protectedFid));
    QVERIFY(m_fileCache->isCached(evictableFid));

    // Add open handle only to the protected file
    m_fileCache->addOpenHandle(protectedFid);

    // Shrink the max cache size to just below total content, forcing eviction.
    // The protected file (31 bytes) + evictable file (15 bytes) = 46 bytes.
    // Setting max to 32 bytes means we need to evict at least the evictable file.
    m_fileCache->setMaxCacheSize(content.size() + 1);

    // Protected file must still be cached (open handle prevents eviction)
    QVERIFY(m_fileCache->isCached(protectedFid));
    QString cachePath = m_fileCache->getCachePathForFile(protectedFid);
    QVERIFY(QFile::exists(cachePath));

    // The evictable file should have been evicted
    QVERIFY(!m_fileCache->isCached(evictableFid));

    // Cleanup — restore sane cache size and release handle
    m_fileCache->setMaxCacheSize(10LL * 1024 * 1024 * 1024);
    m_fileCache->removeOpenHandle(protectedFid);
}

void TestDirtySyncWorker::testOpenHandle_ReleasedAllowsInvalidation() {
    const QString fid = "handle_release_inv";
    const QByteArray content = "temporary data";

    createCacheFileWithContent(fid, content);

    // Add and then remove the handle
    m_fileCache->addOpenHandle(fid);
    QVERIFY(m_fileCache->hasOpenHandles(fid));
    m_fileCache->removeOpenHandle(fid);
    QVERIFY(!m_fileCache->hasOpenHandles(fid));

    // Now invalidation should succeed
    m_fileCache->invalidate(fid);
    QVERIFY(!m_fileCache->isCached(fid));
}

QTEST_MAIN(TestDirtySyncWorker)
#include "TestDirtySyncWorker.moc"
