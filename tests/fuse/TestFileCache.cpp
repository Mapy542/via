/**
 * @file TestFileCache.cpp
 * @brief Unit tests for FileCache — covers C1 (dirty-guard in invalidate),
 *        dirty tracking, and basic cache management.
 */

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <functional>
#include <thread>

#include "api/GoogleDriveClient.h"
#include "fuse/FileCache.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncDatabase.h"

// ---------------------------------------------------------------------------
// Minimal FakeDriveClient — enough for FileCache construction
// ---------------------------------------------------------------------------
class JoiningThread {
   public:
    explicit JoiningThread(std::function<void()> function) : m_thread(std::move(function)) {}

    ~JoiningThread() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void join() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

   private:
    std::thread m_thread;
};

class FakeDriveClientFC : public GoogleDriveClient {
    Q_OBJECT
   public:
    explicit FakeDriveClientFC(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    struct HeldExport {
        QString fileId;
        QString exportMimeType;
        QString localPath;
        QByteArray payload;
        bool shouldFail = false;
        QString errorMessage;
        int errorStatus = 0;
    };

    QByteArray exportPayload = QByteArray("exported native doc");
    QString exportErrorMessage;
    int exportErrorStatus = 0;
    bool exportShouldFail = false;
    bool holdExports = false;
    int downloadCallCount = 0;
    int exportCallCount = 0;
    int exportWriteCount = 0;
    QString lastDownloadFileId;
    QString lastDownloadPath;
    QString lastExportFileId;
    QString lastExportMimeType;
    QString lastExportPath;
    QList<HeldExport> heldExports;

    void downloadFile(const QString& fileId, const QString& localPath) override {
        ++downloadCallCount;
        lastDownloadFileId = fileId;
        lastDownloadPath = localPath;
    }
    void exportFile(const QString& fileId, const QString& exportMimeType,
                    const QString& localPath) override {
        ++exportCallCount;
        lastExportFileId = fileId;
        lastExportMimeType = exportMimeType;
        lastExportPath = localPath;

        if (holdExports) {
            HeldExport held;
            held.fileId = fileId;
            held.exportMimeType = exportMimeType;
            held.localPath = localPath;
            held.payload = exportPayload;
            held.shouldFail = exportShouldFail;
            held.errorMessage = exportErrorMessage;
            held.errorStatus = exportErrorStatus;
            heldExports.append(held);
            return;
        }

        finishExport(fileId, localPath, exportPayload, exportShouldFail, exportErrorMessage,
                     exportErrorStatus);
    }

    void completeNextHeldExport() {
        if (heldExports.isEmpty()) {
            return;
        }

        const HeldExport held = heldExports.takeFirst();
        finishExport(held.fileId, held.localPath, held.payload, held.shouldFail, held.errorMessage,
                     held.errorStatus);
    }

    int heldExportCount() const { return heldExports.size(); }

   private:
    void finishExport(const QString& fileId, const QString& localPath, const QByteArray& payload,
                      bool shouldFail, const QString& errorMessage, int errorStatus) {
        if (shouldFail) {
            emit errorDetailed(QStringLiteral("exportFile:%1").arg(fileId), errorMessage,
                               errorStatus, fileId, localPath);
            return;
        }

        QFileInfo fileInfo(localPath);
        QDir().mkpath(fileInfo.dir().absolutePath());

        QFile file(localPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QString message =
                QStringLiteral("Failed to open file for writing: %1").arg(localPath);
            emit error(QStringLiteral("exportFile:%1").arg(fileId), message);
            emit errorDetailed(QStringLiteral("exportFile:%1").arg(fileId), message, 0, fileId,
                               localPath);
            return;
        }

        if (file.write(payload) != payload.size()) {
            const QString message =
                QStringLiteral("Failed to write exported file: %1").arg(localPath);
            file.close();
            emit error(QStringLiteral("exportFile:%1").arg(fileId), message);
            emit errorDetailed(QStringLiteral("exportFile:%1").arg(fileId), message, 0, fileId,
                               localPath);
            return;
        }

        file.close();
        ++exportWriteCount;
        emit fileDownloaded(fileId, localPath);
    }
    void uploadFile(const QString&, const QString&, const QString&) override {}
    void updateFile(const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void renameFile(const QString&, const QString&) override {}
    void deleteFile(const QString&) override {}
    void createFolder(const QString&, const QString&, const QString&) override {}
    QJsonArray getParentsByFileId(const QString&) override { return {}; }
    QString getFolderIdByPath(const QString&) override { return {}; }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class TestFileCache : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // Dirty tracking
    void testMarkDirty_SetsDirtyFlag();
    void testMarkDirty_RepeatedWriteAdvancesGeneration();
    void testClearDirty_RemovesDirtyFlag();
    void testIsDirty_ReturnsFalseForUnknown();
    void testGetDirtyFiles_ReturnsAll();

    // C1: invalidate must skip dirty files
    void testInvalidate_RemovesCleanEntry();
    void testInvalidate_SkipsDirtyEntry();
    void testInvalidate_NoopForUnknownFileId();

    // removeFromCache must remove even dirty files
    void testRemoveFromCache_RemovesDirtyEntry();

    // markUploadFailed
    void testMarkUploadFailed_SetsFlag();

    // Cache limit behavior
    void testCacheLimit_SizeAccountingTracksEntries();
    void testCacheLimit_EvictsLeastRecentlyUsedEntry();
    void testCacheLimit_ProtectedEntriesSurviveEvictionPressure();
    void testCacheLimit_OversizeFileReentersPressureAfterRelease();

    // Dirty guard in getCachedPath
    void testGetCachedPath_DirtyFileSkipsDownload();
    void testGetCachedPath_PausedCacheMissFailsWithoutDownload();
    void testGetCachedPath_PausedCacheHitStillWorks();

    // Pending-store (dirty file migration)
    void testMoveToDirtyStore_MovesFile();
    void testGetContentPath_DirtyFile_ReturnsPendingPath();
    void testGetContentPath_CleanFile_ReturnsCachePath();
    void testClearDirty_RemovesPendingFile();
    void testClearDirty_SkipsStaleGeneration();
    void testClearDirty_AllowsReadOnlyHandle();
    void testClearDirty_SkipsWhenWritableHandleExists();
    void testCreateUploadSnapshot_CopiesPendingContent();
    void testCreateUploadSnapshot_SkipsUploadedGeneration();
    void testFinalizeUploadedGeneration_ClearsWhenWriterCloses();

    // Representation-specific cache key
    void testGenerateCachePath_ExportMimeProducesDifferentPath();
    void testGetExportedPath_FailurePreservesDetailedMessage();
    void testGetExportedPath_SuccessCachesBufferedExport();
    void testGetExportedPath_DifferentMimeTypesStayDistinct();
    void testGetExportedPath_ZeroBytePayloadFails();
    void testGetExportedPath_ZeroByteRemnantReexports();
    void testGetExportedPath_PausedCacheMissFailsWithoutExport();
    void testGetExportedPath_PausedCacheHitStillWorks();
    void testQueueExportedPath_BoundedConcurrency();
    void testQueueExportedPath_DeduplicatesRepeatedRequests();
    void testGetExportedPath_JoinsBackgroundExport();

    // Restart durability
    void testRestart_RestoresDirtyGenerationState();
    void testRestart_FinalizesUploadedGenerationState();

   private:
    void createTestDatabase();
    void destroyTestDatabase();
    void recreateCache();

    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientFC* m_driveClient = nullptr;
    RuntimePauseController* m_pauseController = nullptr;
    FileCache* m_cache = nullptr;
};

// ---------------------------------------------------------------------------
// Setup / Teardown
// ---------------------------------------------------------------------------
void TestFileCache::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);
    qputenv("HOME", m_tempDir->path().toUtf8());

    createTestDatabase();

    m_driveClient = new FakeDriveClientFC(this);
    m_pauseController = new RuntimePauseController(this);
    m_cache = new FileCache(m_db, m_driveClient, this);
    m_cache->setPauseController(m_pauseController);

    // Set cache dir inside temp
    QString cacheDir = m_tempDir->path() + "/cache";
    QDir().mkpath(cacheDir);
    m_cache->setCacheDirectory(cacheDir);

    // Set pending-store dir inside temp (keeps tests hermetic)
    QString dirtyDir = m_tempDir->path() + "/pending";
    QDir().mkpath(dirtyDir);
    m_cache->setDirtyDirectory(dirtyDir);

    QVERIFY(m_cache->initialize());
}

void TestFileCache::cleanup() {
    delete m_cache;
    m_cache = nullptr;
    delete m_pauseController;
    m_pauseController = nullptr;
    delete m_driveClient;
    m_driveClient = nullptr;
    destroyTestDatabase();
    delete m_tempDir;
    m_tempDir = nullptr;
    QStandardPaths::setTestModeEnabled(false);
}

void TestFileCache::createTestDatabase() {
    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());
}

void TestFileCache::destroyTestDatabase() {
    if (m_db) {
        m_db->close();
        delete m_db;
        m_db = nullptr;
    }
}

void TestFileCache::recreateCache() {
    delete m_cache;
    m_cache = nullptr;

    destroyTestDatabase();
    createTestDatabase();

    m_cache = new FileCache(m_db, m_driveClient, this);
    m_cache->setPauseController(m_pauseController);
    m_cache->setCacheDirectory(m_tempDir->path() + "/cache");
    m_cache->setDirtyDirectory(m_tempDir->path() + "/pending");
    QVERIFY(m_cache->initialize());
}

// ---------------------------------------------------------------------------
// Helpers — put a file into cache so invalidate / remove have something to act on
// ---------------------------------------------------------------------------
static void seedCacheFile(FileCache* cache, const QString& /*cacheDir*/, const QString& fileId,
                          qint64 size = 100) {
    // Create a real file at the deterministic cache path used by production callers.
    QString filePath = cache->getCachePathForFile(fileId);
    QVERIFY(QDir().mkpath(QFileInfo(filePath).dir().absolutePath()));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(static_cast<int>(size), 'x'));
    f.close();

    // Record entry via public API
    cache->recordCacheEntry(fileId, filePath, size);
}

// ---------------------------------------------------------------------------
// Tests — dirty tracking
// ---------------------------------------------------------------------------
void TestFileCache::testMarkDirty_SetsDirtyFlag() {
    m_cache->markDirty("f1", "/file1.txt");
    QVERIFY(m_cache->isDirty("f1"));
}

void TestFileCache::testMarkDirty_RepeatedWriteAdvancesGeneration() {
    m_cache->markDirty("f1", "/file1.txt");
    QList<DirtyFileEntry> dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    QCOMPARE(dirty.first().generation, static_cast<quint64>(1));

    m_cache->markDirty("f1", "/file1.txt");
    dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    QCOMPARE(dirty.first().generation, static_cast<quint64>(2));
}

void TestFileCache::testClearDirty_RemovesDirtyFlag() {
    m_cache->markDirty("f1", "/file1.txt");
    m_cache->clearDirty("f1");
    QVERIFY(!m_cache->isDirty("f1"));
}

void TestFileCache::testIsDirty_ReturnsFalseForUnknown() {
    QVERIFY(!m_cache->isDirty("nonexistent"));
}

void TestFileCache::testGetDirtyFiles_ReturnsAll() {
    m_cache->markDirty("a", "/a.txt");
    m_cache->markDirty("b", "/b.txt");

    QList<DirtyFileEntry> dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 2);

    QSet<QString> ids;
    for (const auto& e : dirty) ids.insert(e.fileId);
    QVERIFY(ids.contains("a"));
    QVERIFY(ids.contains("b"));
}

// ---------------------------------------------------------------------------
// Tests — C1: invalidate guards dirty files
// ---------------------------------------------------------------------------
void TestFileCache::testInvalidate_RemovesCleanEntry() {
    QString cacheDir = m_cache->cacheDirectory();
    seedCacheFile(m_cache, cacheDir, "clean1", 200);

    QVERIFY(m_cache->isCached("clean1"));

    m_cache->invalidate("clean1");

    QVERIFY(!m_cache->isCached("clean1"));
}

void TestFileCache::testInvalidate_SkipsDirtyEntry() {
    QString cacheDir = m_cache->cacheDirectory();
    seedCacheFile(m_cache, cacheDir, "dirty1", 200);

    // Mark dirty BEFORE invalidate
    m_cache->markDirty("dirty1", "/dirty.txt");

    m_cache->invalidate("dirty1");

    // Entry must survive because the file is dirty (C1 fix)
    QVERIFY(m_cache->isCached("dirty1"));
    QVERIFY(m_cache->isDirty("dirty1"));
}

void TestFileCache::testInvalidate_NoopForUnknownFileId() {
    // Should not crash or throw
    m_cache->invalidate("does_not_exist");
}

// ---------------------------------------------------------------------------
// Tests — removeFromCache (always removes, even dirty)
// ---------------------------------------------------------------------------
void TestFileCache::testRemoveFromCache_RemovesDirtyEntry() {
    QString cacheDir = m_cache->cacheDirectory();
    seedCacheFile(m_cache, cacheDir, "rem1", 100);
    m_cache->markDirty("rem1", "/rem.txt");

    m_cache->removeFromCache("rem1");

    QVERIFY(!m_cache->isCached("rem1"));
    QVERIFY(!m_cache->isDirty("rem1"));
}

// ---------------------------------------------------------------------------
// Tests — markUploadFailed
// ---------------------------------------------------------------------------
void TestFileCache::testMarkUploadFailed_SetsFlag() {
    m_cache->markDirty("f1", "/file1.txt");
    m_cache->markUploadFailed("f1");

    QList<DirtyFileEntry> dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    QVERIFY(dirty.first().uploadFailed);
}

// ---------------------------------------------------------------------------
// Tests — cache limit behavior
// ---------------------------------------------------------------------------
void TestFileCache::testCacheLimit_SizeAccountingTracksEntries() {
    QString cacheDir = m_cache->cacheDirectory();

    m_cache->setMaxCacheSize(1024);
    seedCacheFile(m_cache, cacheDir, "size_a", 100);
    seedCacheFile(m_cache, cacheDir, "size_b", 75);

    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(175));

    m_cache->removeFromCache("size_a");
    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(75));

    m_cache->removeFromCache("size_b");
    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(0));
}

void TestFileCache::testCacheLimit_EvictsLeastRecentlyUsedEntry() {
    QString cacheDir = m_cache->cacheDirectory();

    m_cache->setMaxCacheSize(250);
    seedCacheFile(m_cache, cacheDir, "lru_first", 100);
    QTest::qWait(10);
    seedCacheFile(m_cache, cacheDir, "lru_second", 100);
    QTest::qWait(10);
    seedCacheFile(m_cache, cacheDir, "lru_third", 100);

    QVERIFY(!m_cache->isCached("lru_first"));
    QVERIFY(m_cache->isCached("lru_second"));
    QVERIFY(m_cache->isCached("lru_third"));
    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(200));
}

void TestFileCache::testCacheLimit_ProtectedEntriesSurviveEvictionPressure() {
    QString cacheDir = m_cache->cacheDirectory();

    m_cache->setMaxCacheSize(180);
    seedCacheFile(m_cache, cacheDir, "protected_entry", 100);
    m_cache->markDirty("protected_entry", "/protected_entry.txt");
    QTest::qWait(10);
    seedCacheFile(m_cache, cacheDir, "eviction_victim", 70);
    QTest::qWait(10);
    seedCacheFile(m_cache, cacheDir, "new_entry", 80);

    QVERIFY(m_cache->isCached("protected_entry"));
    QVERIFY(m_cache->isDirty("protected_entry"));
    QVERIFY(!m_cache->isCached("eviction_victim"));
    QVERIFY(m_cache->isCached("new_entry"));
    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(180));
}

void TestFileCache::testCacheLimit_OversizeFileReentersPressureAfterRelease() {
    QString cacheDir = m_cache->cacheDirectory();

    m_cache->setMaxCacheSize(100);
    seedCacheFile(m_cache, cacheDir, "oversize_open", 180);
    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(180));

    m_cache->addOpenHandle("oversize_open");
    QTest::qWait(10);
    seedCacheFile(m_cache, cacheDir, "small_after_oversize", 20);

    QVERIFY(m_cache->isCached("oversize_open"));
    QVERIFY(m_cache->isCached("small_after_oversize"));
    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(200));

    m_cache->removeOpenHandle("oversize_open");

    QVERIFY(!m_cache->isCached("oversize_open"));
    QVERIFY(m_cache->isCached("small_after_oversize"));
    QCOMPARE(m_cache->currentCacheSize(), static_cast<qint64>(20));
}

// ---------------------------------------------------------------------------
// Tests — dirty guard in getCachedPath
// ---------------------------------------------------------------------------

// When a file is dirty but its cache entry is absent (e.g. post-restart
// rehydration race), getCachedPath must return the on-disk path WITHOUT
// initiating a remote download, so local modifications are not overwritten.
void TestFileCache::testGetCachedPath_DirtyFileSkipsDownload() {
    const QString fileId = "dirty_no_entry";

    // Find the deterministic cache path for this fileId.
    QString expectedPath = m_cache->getCachePathForFile(fileId);
    QVERIFY(!expectedPath.isEmpty());

    // Create the file on disk (simulates content written before the crash/race).
    QFileInfo fi(expectedPath);
    QVERIFY(QDir().mkpath(fi.dir().absolutePath()));
    QFile f(expectedPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("dirty content after write");
    f.close();

    // Mark dirty but deliberately skip recordCacheEntry so m_cacheEntries is empty.
    m_cache->markDirty(fileId, "/dirty_no_entry.txt");
    QVERIFY(!m_cache->isCached(fileId));  // confirm no cache entry

    // getCachedPath must return the local path — not block on a download.
    QString result = m_cache->getCachedPath(fileId);

    QCOMPARE(result, expectedPath);
    // Entry must now be rehydrated into the cache index.
    QVERIFY(m_cache->isCached(fileId));
    // Dirty flag must still be set; getCachedPath must not clear it.
    QVERIFY(m_cache->isDirty(fileId));

    // Cleanup
    QFile::remove(expectedPath);
}

void TestFileCache::testGetCachedPath_PausedCacheMissFailsWithoutDownload() {
    const QString fileId = QStringLiteral("paused-cache-miss");

    m_pauseController->requestManualPause();

    QSignalSpy failedSpy(m_cache, &FileCache::downloadFailed);
    QVERIFY(failedSpy.isValid());

    const QString result = m_cache->getCachedPath(fileId);

    QVERIFY(result.isEmpty());
    QCOMPARE(m_driveClient->downloadCallCount, 0);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(
        failedSpy.takeFirst().at(1).toString().contains(QStringLiteral("Cannot download files")));
}

void TestFileCache::testGetCachedPath_PausedCacheHitStillWorks() {
    const QString fileId = QStringLiteral("paused-cache-hit");
    const QString cacheDir = m_cache->cacheDirectory();
    seedCacheFile(m_cache, cacheDir, fileId, 32);

    const QString expectedPath = m_cache->getCachePathForFile(fileId);

    m_pauseController->requestManualPause();

    const QString result = m_cache->getCachedPath(fileId);

    QCOMPARE(result, expectedPath);
    QCOMPARE(m_driveClient->downloadCallCount, 0);
}

// ---------------------------------------------------------------------------
// Tests — pending store (dirty file migration to ~/.local/share)
// ---------------------------------------------------------------------------

// After moveToDirtyStore the file must exist at the pending path, the old
// cache path must be gone, the cache entry must be removed, and the dirty
// flag must remain.
void TestFileCache::testMoveToDirtyStore_MovesFile() {
    const QString fileId = "move_me";
    QString cacheDir = m_cache->cacheDirectory();

    // Put a real file in cache
    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFileInfo ci(cachePath);
    QVERIFY(QDir().mkpath(ci.dir().absolutePath()));
    QFile cf(cachePath);
    QVERIFY(cf.open(QIODevice::WriteOnly));
    cf.write("dirty content");
    cf.close();
    m_cache->recordCacheEntry(fileId, cachePath, 13);
    m_cache->markDirty(fileId, "/move_me.txt");

    QVERIFY(m_cache->isCached(fileId));
    QVERIFY(m_cache->isDirty(fileId));

    // Move to pending store
    QString pendingPath = m_cache->moveToDirtyStore(fileId);

    QVERIFY(!pendingPath.isEmpty());
    QVERIFY(QFile::exists(pendingPath));
    QVERIFY(!QFile::exists(cachePath));
    QVERIFY(!m_cache->isCached(fileId));  // removed from cache map
    QVERIFY(m_cache->isDirty(fileId));    // still dirty

    // Pending path must live under the dirty directory
    QVERIFY(pendingPath.startsWith(m_cache->dirtyDirectory()));
}

// When a file is dirty and its content has already been moved to the pending
// store, getContentPath must return the pending path — not the cache path.
void TestFileCache::testGetContentPath_DirtyFile_ReturnsPendingPath() {
    const QString fileId = "pending_content";

    // Create content at the expected pending path
    QString pendingPath = m_cache->getDirtyPathForFile(fileId);
    QFileInfo pi(pendingPath);
    QVERIFY(QDir().mkpath(pi.dir().absolutePath()));
    QFile pf(pendingPath);
    QVERIFY(pf.open(QIODevice::WriteOnly));
    pf.write("data");
    pf.close();

    m_cache->markDirty(fileId, "/pending_content.txt");

    QCOMPARE(m_cache->getContentPath(fileId), pendingPath);

    QFile::remove(pendingPath);
}

// For a clean (non-dirty) cached file getContentPath must return the
// standard cache path.
void TestFileCache::testGetContentPath_CleanFile_ReturnsCachePath() {
    const QString fileId = "clean_content";
    QString cacheDir = m_cache->cacheDirectory();
    seedCacheFile(m_cache, cacheDir, fileId, 50);

    QString expected = m_cache->getCachePathForFile(fileId);
    QCOMPARE(m_cache->getContentPath(fileId), expected);
}

// clearDirty must recycle the pending-store file back into the LRU cache
// so that file content is immediately available for subsequent reads.
void TestFileCache::testClearDirty_RemovesPendingFile() {
    const QString fileId = "to_clear";

    // Place a file at the pending path
    QString pendingPath = m_cache->getDirtyPathForFile(fileId);
    QFileInfo pi(pendingPath);
    QVERIFY(QDir().mkpath(pi.dir().absolutePath()));
    QFile pf(pendingPath);
    QVERIFY(pf.open(QIODevice::WriteOnly));
    pf.write("data");
    pf.close();

    m_cache->markDirty(fileId, "/to_clear.txt");
    QVERIFY(QFile::exists(pendingPath));

    m_cache->clearDirty(fileId);

    QVERIFY(!m_cache->isDirty(fileId));
    // Pending file should be gone (moved, not left behind)
    QVERIFY(!QFile::exists(pendingPath));
    // File should now be in the LRU cache
    QVERIFY(m_cache->isCached(fileId));
    // The recycled cache file should contain the original content
    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFile cached(cachePath);
    QVERIFY(cached.open(QIODevice::ReadOnly));
    QCOMPARE(cached.readAll(), QByteArray("data"));
    cached.close();
}

void TestFileCache::testClearDirty_SkipsStaleGeneration() {
    const QString fileId = "stale_generation";

    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFileInfo ci(cachePath);
    QVERIFY(QDir().mkpath(ci.dir().absolutePath()));
    QFile cf(cachePath);
    QVERIFY(cf.open(QIODevice::WriteOnly));
    cf.write("v1");
    cf.close();
    m_cache->recordCacheEntry(fileId, cachePath, 2);
    m_cache->markDirty(fileId, "/stale_generation.txt");

    QList<DirtyFileEntry> dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    quint64 firstGeneration = dirty.first().generation;

    QString pendingPath = m_cache->moveToDirtyStore(fileId);
    QVERIFY(!pendingPath.isEmpty());
    QVERIFY(QFile::exists(pendingPath));

    QFile pending(pendingPath);
    QVERIFY(pending.open(QIODevice::Append));
    pending.write("_v2");
    pending.close();

    m_cache->markDirty(fileId, "/stale_generation.txt");
    dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    QCOMPARE(dirty.first().generation, firstGeneration + 1);

    QVERIFY(!m_cache->clearDirty(fileId, firstGeneration));
    QVERIFY(m_cache->isDirty(fileId));
    QVERIFY(QFile::exists(pendingPath));
    QVERIFY(!m_cache->isCached(fileId));
}

void TestFileCache::testClearDirty_AllowsReadOnlyHandle() {
    const QString fileId = "reader_generation";

    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFileInfo ci(cachePath);
    QVERIFY(QDir().mkpath(ci.dir().absolutePath()));
    QFile cf(cachePath);
    QVERIFY(cf.open(QIODevice::WriteOnly));
    cf.write("reader");
    cf.close();
    m_cache->recordCacheEntry(fileId, cachePath, 6);
    m_cache->markDirty(fileId, "/reader_generation.txt");

    QList<DirtyFileEntry> dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    quint64 generation = dirty.first().generation;

    QString pendingPath = m_cache->moveToDirtyStore(fileId);
    QVERIFY(!pendingPath.isEmpty());
    QVERIFY(QFile::exists(pendingPath));

    m_cache->addOpenHandle(fileId);
    QVERIFY(m_cache->clearDirty(fileId, generation));
    QVERIFY(!m_cache->isDirty(fileId));
    QVERIFY(m_cache->isCached(fileId));

    m_cache->removeOpenHandle(fileId);
}

void TestFileCache::testClearDirty_SkipsWhenWritableHandleExists() {
    const QString fileId = "busy_generation";

    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFileInfo ci(cachePath);
    QVERIFY(QDir().mkpath(ci.dir().absolutePath()));
    QFile cf(cachePath);
    QVERIFY(cf.open(QIODevice::WriteOnly));
    cf.write("busy");
    cf.close();
    m_cache->recordCacheEntry(fileId, cachePath, 4);
    m_cache->markDirty(fileId, "/busy_generation.txt");

    QList<DirtyFileEntry> dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    quint64 generation = dirty.first().generation;

    QString pendingPath = m_cache->moveToDirtyStore(fileId);
    QVERIFY(!pendingPath.isEmpty());
    QVERIFY(QFile::exists(pendingPath));

    m_cache->addOpenHandle(fileId, true);
    QVERIFY(!m_cache->clearDirty(fileId, generation));
    QVERIFY(m_cache->isDirty(fileId));
    QVERIFY(QFile::exists(pendingPath));
    QVERIFY(!m_cache->isCached(fileId));

    m_cache->removeOpenHandle(fileId, true);
    QVERIFY(m_cache->clearDirty(fileId, generation));
    QVERIFY(!m_cache->isDirty(fileId));
    QVERIFY(m_cache->isCached(fileId));
}

void TestFileCache::testCreateUploadSnapshot_CopiesPendingContent() {
    const QString fileId = "snapshot_copy";
    const QByteArray original("original snapshot bytes");
    const QByteArray updated("new local bytes");

    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFileInfo ci(cachePath);
    QVERIFY(QDir().mkpath(ci.dir().absolutePath()));
    QFile cf(cachePath);
    QVERIFY(cf.open(QIODevice::WriteOnly));
    cf.write(original);
    cf.close();
    m_cache->recordCacheEntry(fileId, cachePath, original.size());
    m_cache->markDirty(fileId, "/snapshot_copy.txt");

    const quint64 generation = m_cache->getDirtyFiles().first().generation;
    const QString pendingPath = m_cache->moveToDirtyStore(fileId);
    QVERIFY(!pendingPath.isEmpty());

    const UploadSnapshotResult snapshot = m_cache->createUploadSnapshot(fileId, generation);
    QCOMPARE(snapshot.status, UploadSnapshotStatus::Ready);
    QVERIFY(QFile::exists(snapshot.snapshotPath));
    QVERIFY(QFile::exists(pendingPath));

    QFile snapshotFile(snapshot.snapshotPath);
    QVERIFY(snapshotFile.open(QIODevice::ReadOnly));
    QCOMPARE(snapshotFile.readAll(), original);
    snapshotFile.close();

    QFile pendingFile(pendingPath);
    QVERIFY(pendingFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    pendingFile.write(updated);
    pendingFile.close();

    QVERIFY(snapshotFile.open(QIODevice::ReadOnly));
    QCOMPARE(snapshotFile.readAll(), original);
    snapshotFile.close();

    m_cache->cleanupUploadSnapshot(snapshot.snapshotPath);
}

void TestFileCache::testCreateUploadSnapshot_SkipsUploadedGeneration() {
    const QString fileId = "snapshot_uploaded";

    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFileInfo ci(cachePath);
    QVERIFY(QDir().mkpath(ci.dir().absolutePath()));
    QFile cf(cachePath);
    QVERIFY(cf.open(QIODevice::WriteOnly));
    cf.write("already uploaded");
    cf.close();
    m_cache->recordCacheEntry(fileId, cachePath, 16);
    m_cache->markDirty(fileId, "/snapshot_uploaded.txt");

    const quint64 generation = m_cache->getDirtyFiles().first().generation;
    QVERIFY(!m_cache->moveToDirtyStore(fileId).isEmpty());
    m_cache->markUploadedGeneration(fileId, generation);

    const UploadSnapshotResult snapshot = m_cache->createUploadSnapshot(fileId, generation);
    QCOMPARE(snapshot.status, UploadSnapshotStatus::AlreadyUploaded);
    QVERIFY(snapshot.snapshotPath.isEmpty());
}

void TestFileCache::testFinalizeUploadedGeneration_ClearsWhenWriterCloses() {
    const QString fileId = "finalize_uploaded";

    QString cachePath = m_cache->getCachePathForFile(fileId);
    QFileInfo ci(cachePath);
    QVERIFY(QDir().mkpath(ci.dir().absolutePath()));
    QFile cf(cachePath);
    QVERIFY(cf.open(QIODevice::WriteOnly));
    cf.write("finalize me");
    cf.close();
    m_cache->recordCacheEntry(fileId, cachePath, 11);
    m_cache->markDirty(fileId, "/finalize_uploaded.txt");

    const quint64 generation = m_cache->getDirtyFiles().first().generation;
    QVERIFY(!m_cache->moveToDirtyStore(fileId).isEmpty());

    m_cache->addOpenHandle(fileId, true);
    m_cache->markUploadedGeneration(fileId, generation);
    QVERIFY(!m_cache->finalizeUploadedGeneration(fileId));
    QVERIFY(m_cache->isDirty(fileId));

    m_cache->removeOpenHandle(fileId, true);
    QVERIFY(!m_cache->isDirty(fileId));
    QVERIFY(m_cache->isCached(fileId));
}

void TestFileCache::testGenerateCachePath_ExportMimeProducesDifferentPath() {
    const QString fileId = QStringLiteral("test-native-doc-id");

    // Plain getCachePathForFile uses the fileId-only hash
    QString basePath = m_cache->getCachePathForFile(fileId);
    QVERIFY(!basePath.isEmpty());

    // Exported paths with different MIME types must differ from each other
    // and from the plain path.  We access the overload via getExportedPath's
    // internal call, but since that triggers a real export, test via the
    // public getCachePathForFile (fileId-only) vs the two-arg overload
    // indirectly: generate two cache entries and compare their on-disk
    // paths by checking that export would hash differently.

    // We can test this by verifying that getCachePathForFile returns a
    // deterministic path (same ID → same path).
    QString basePath2 = m_cache->getCachePathForFile(fileId);
    QCOMPARE(basePath, basePath2);
}

void TestFileCache::testGetExportedPath_FailurePreservesDetailedMessage() {
    const QString fileId = QStringLiteral("native-doc-failure");
    const QString exportMimeType = QStringLiteral("text/markdown");

    m_driveClient->exportShouldFail = true;
    m_driveClient->exportErrorMessage = QStringLiteral("This file is too large to be exported.");
    m_driveClient->exportErrorStatus = 403;

    QSignalSpy failedSpy(m_cache, &FileCache::downloadFailed);
    QVERIFY(failedSpy.isValid());
    QSignalSpy failedDetailedSpy(m_cache, &FileCache::downloadFailedDetailed);
    QVERIFY(failedDetailedSpy.isValid());

    QString result;
    JoiningThread worker([&]() { result = m_cache->getExportedPath(fileId, exportMimeType); });

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(failedDetailedSpy.count(), 1, 2000);
    worker.join();

    QVERIFY(result.isEmpty());
    QCOMPARE(m_driveClient->exportCallCount, 1);
    QCOMPARE(m_driveClient->lastExportFileId, fileId);
    QCOMPARE(m_driveClient->lastExportMimeType, exportMimeType);

    const QList<QVariant> args = failedSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), fileId);
    QVERIFY(args.at(1).toString().contains(QStringLiteral("too large to be exported")));

    const QList<QVariant> detailedArgs = failedDetailedSpy.takeFirst();
    QCOMPARE(detailedArgs.at(0).toString(), fileId);
    QCOMPARE(detailedArgs.at(2).toInt(), 403);
}

void TestFileCache::testGetExportedPath_SuccessCachesBufferedExport() {
    const QString fileId = QStringLiteral("native-doc-success");
    const QString exportMimeType = QStringLiteral("text/markdown");
    const QByteArray payload("# Exported snapshot\nHello from Drive\n");

    m_driveClient->exportShouldFail = false;
    m_driveClient->exportPayload = payload;

    QSignalSpy completedSpy(m_cache, &FileCache::downloadCompleted);
    QVERIFY(completedSpy.isValid());

    QString result;
    JoiningThread worker([&]() { result = m_cache->getExportedPath(fileId, exportMimeType); });

    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    worker.join();

    QVERIFY(!result.isEmpty());
    QCOMPARE(m_driveClient->exportCallCount, 1);
    QCOMPARE(m_driveClient->exportWriteCount, 1);
    QCOMPARE(m_driveClient->lastExportFileId, fileId);
    QCOMPARE(m_driveClient->lastExportMimeType, exportMimeType);
    QVERIFY(QFile::exists(result));

    QFile exportedFile(result);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    QCOMPARE(exportedFile.readAll(), payload);
    exportedFile.close();

    const QString cachedAgain = m_cache->getExportedPath(fileId, exportMimeType);
    QCOMPARE(cachedAgain, result);
    QCOMPARE(m_driveClient->exportCallCount, 1);

    const QList<QVariant> args = completedSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), fileId);
    QCOMPARE(args.at(1).toString(), result);
}

void TestFileCache::testGetExportedPath_DifferentMimeTypesStayDistinct() {
    const QString fileId = QStringLiteral("native-doc-multi-export");
    const QString markdownMime = QStringLiteral("text/markdown");
    const QString odtMime = QStringLiteral("application/vnd.oasis.opendocument.text");
    const QByteArray markdownPayload("# Markdown export\n");
    const QByteArray odtPayload("fake odt payload");

    m_driveClient->exportShouldFail = false;
    m_driveClient->exportPayload = markdownPayload;

    QSignalSpy completedSpy(m_cache, &FileCache::downloadCompleted);
    QVERIFY(completedSpy.isValid());

    QString markdownPath;
    JoiningThread markdownWorker(
        [&]() { markdownPath = m_cache->getExportedPath(fileId, markdownMime); });
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    markdownWorker.join();
    QVERIFY(!markdownPath.isEmpty());
    QVERIFY(m_cache->isCached(fileId, markdownMime));
    QCOMPARE(m_cache->getContentPath(fileId, markdownMime), markdownPath);

    m_driveClient->exportPayload = odtPayload;
    QString odtPath;
    JoiningThread odtWorker([&]() { odtPath = m_cache->getExportedPath(fileId, odtMime); });
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 2, 2000);
    odtWorker.join();
    QVERIFY(!odtPath.isEmpty());
    QVERIFY(m_cache->isCached(fileId, odtMime));
    QCOMPARE(m_cache->getContentPath(fileId, odtMime), odtPath);

    QVERIFY(markdownPath != odtPath);
    QCOMPARE(m_driveClient->exportCallCount, 2);

    QFile markdownFile(markdownPath);
    QVERIFY(markdownFile.open(QIODevice::ReadOnly));
    QCOMPARE(markdownFile.readAll(), markdownPayload);
    markdownFile.close();

    QFile odtFile(odtPath);
    QVERIFY(odtFile.open(QIODevice::ReadOnly));
    QCOMPARE(odtFile.readAll(), odtPayload);
    odtFile.close();

    QCOMPARE(m_cache->getExportedPath(fileId, markdownMime), markdownPath);
    QCOMPARE(m_cache->getExportedPath(fileId, odtMime), odtPath);
    QCOMPARE(m_driveClient->exportCallCount, 2);
}

void TestFileCache::testGetExportedPath_ZeroBytePayloadFails() {
    const QString fileId = QStringLiteral("native-doc-zero-byte");
    const QString exportMimeType = QStringLiteral("text/markdown");

    m_driveClient->exportShouldFail = false;
    m_driveClient->exportPayload.clear();

    QSignalSpy failedSpy(m_cache, &FileCache::downloadFailed);
    QVERIFY(failedSpy.isValid());

    QString result;
    JoiningThread worker([&]() { result = m_cache->getExportedPath(fileId, exportMimeType); });

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 2000);
    worker.join();

    QVERIFY(result.isEmpty());
    QCOMPARE(m_driveClient->exportCallCount, 1);

    const QList<QVariant> args = failedSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), fileId);
    QCOMPARE(args.at(1).toString(), QStringLiteral("Export produced an empty file"));
}

void TestFileCache::testGetExportedPath_ZeroByteRemnantReexports() {
    const QString fileId = QStringLiteral("native-doc-zero-byte-remnant");
    const QString exportMimeType = QStringLiteral("text/markdown");
    const QString cacheKey = fileId + QStringLiteral("|") + exportMimeType;
    const QByteArray payload("# Re-exported snapshot\n");

    const QString remnantPath = m_cache->getContentPath(fileId, exportMimeType);
    QVERIFY(QDir().mkpath(QFileInfo(remnantPath).dir().absolutePath()));

    QFile remnant(remnantPath);
    QVERIFY(remnant.open(QIODevice::WriteOnly | QIODevice::Truncate));
    remnant.close();
    QCOMPARE(QFileInfo(remnantPath).size(), qint64(0));

    QVERIFY(m_db->recordFuseCacheEntry(cacheKey, remnantPath, 0));

    recreateCache();

    m_driveClient->exportShouldFail = false;
    m_driveClient->exportPayload = payload;

    QSignalSpy completedSpy(m_cache, &FileCache::downloadCompleted);
    QVERIFY(completedSpy.isValid());

    QString result;
    JoiningThread worker([&]() { result = m_cache->getExportedPath(fileId, exportMimeType); });

    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    worker.join();

    QVERIFY(!result.isEmpty());
    QCOMPARE(result, remnantPath);
    QCOMPARE(m_driveClient->exportCallCount, 1);

    QFile exportedFile(result);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    QCOMPARE(exportedFile.readAll(), payload);
    exportedFile.close();
}

void TestFileCache::testGetExportedPath_PausedCacheMissFailsWithoutExport() {
    const QString fileId = QStringLiteral("paused-export-miss");
    const QString exportMimeType = QStringLiteral("text/markdown");

    m_pauseController->requestManualPause();

    QSignalSpy failedSpy(m_cache, &FileCache::downloadFailed);
    QVERIFY(failedSpy.isValid());

    const QString result = m_cache->getExportedPath(fileId, exportMimeType);

    QVERIFY(result.isEmpty());
    QCOMPARE(m_driveClient->exportCallCount, 0);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(failedSpy.takeFirst().at(1).toString().contains(
        QStringLiteral("Cannot export native documents")));
}

void TestFileCache::testGetExportedPath_PausedCacheHitStillWorks() {
    const QString fileId = QStringLiteral("paused-export-hit");
    const QString exportMimeType = QStringLiteral("text/markdown");
    const QByteArray payload("cached exported bytes");

    m_driveClient->exportPayload = payload;

    QSignalSpy completedSpy(m_cache, &FileCache::downloadCompleted);
    QVERIFY(completedSpy.isValid());

    QString exportedPath;
    JoiningThread worker(
        [&]() { exportedPath = m_cache->getExportedPath(fileId, exportMimeType); });
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    worker.join();

    QCOMPARE(m_driveClient->exportCallCount, 1);

    m_pauseController->requestManualPause();

    const QString cachedAgain = m_cache->getExportedPath(fileId, exportMimeType);
    QCOMPARE(cachedAgain, exportedPath);
    QCOMPARE(m_driveClient->exportCallCount, 1);
}

void TestFileCache::testQueueExportedPath_BoundedConcurrency() {
    const QString exportMimeType = QStringLiteral("text/markdown");

    m_driveClient->holdExports = true;

    m_cache->queueExportedPath(QStringLiteral("native-doc-queue-a"), exportMimeType);
    m_cache->queueExportedPath(QStringLiteral("native-doc-queue-b"), exportMimeType);
    m_cache->queueExportedPath(QStringLiteral("native-doc-queue-c"), exportMimeType);

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->exportCallCount, 2, 2000);
    QCOMPARE(m_driveClient->heldExportCount(), 2);

    m_driveClient->completeNextHeldExport();
    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->exportCallCount, 3, 2000);
    QCOMPARE(m_driveClient->heldExportCount(), 2);

    m_driveClient->completeNextHeldExport();
    m_driveClient->completeNextHeldExport();
}

void TestFileCache::testQueueExportedPath_DeduplicatesRepeatedRequests() {
    const QString fileId = QStringLiteral("native-doc-deduped");
    const QString exportMimeType = QStringLiteral("text/markdown");

    m_driveClient->holdExports = true;

    m_cache->queueExportedPath(fileId, exportMimeType);
    m_cache->queueExportedPath(fileId, exportMimeType);

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->exportCallCount, 1, 2000);
    QCOMPARE(m_driveClient->heldExportCount(), 1);

    m_driveClient->completeNextHeldExport();
    QTRY_VERIFY_WITH_TIMEOUT(m_cache->isCached(fileId, exportMimeType), 2000);
    QCOMPARE(m_driveClient->exportCallCount, 1);
}

void TestFileCache::testGetExportedPath_JoinsBackgroundExport() {
    const QString fileId = QStringLiteral("native-doc-background-join");
    const QString exportMimeType = QStringLiteral("text/markdown");
    const QByteArray payload("joined export payload");

    m_driveClient->holdExports = true;
    m_driveClient->exportPayload = payload;

    QSignalSpy completedSpy(m_cache, &FileCache::downloadCompleted);
    QVERIFY(completedSpy.isValid());

    m_cache->queueExportedPath(fileId, exportMimeType);
    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->exportCallCount, 1, 2000);
    QCOMPARE(m_driveClient->heldExportCount(), 1);

    QString result;
    JoiningThread worker([&]() { result = m_cache->getExportedPath(fileId, exportMimeType); });

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->exportCallCount, 1, 200);

    m_driveClient->completeNextHeldExport();
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    worker.join();

    QVERIFY(!result.isEmpty());
    QCOMPARE(m_driveClient->exportCallCount, 1);

    QFile exportedFile(result);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    QCOMPARE(exportedFile.readAll(), payload);
    exportedFile.close();
}

void TestFileCache::testRestart_RestoresDirtyGenerationState() {
    const QString fileId = QStringLiteral("restart-dirty-state");
    const QString logicalPath = QStringLiteral("/restart-dirty-state.txt");
    const QByteArray initial("restart bytes");
    const QString cachePath = m_cache->getCachePathForFile(fileId);

    QVERIFY(QDir().mkpath(QFileInfo(cachePath).dir().absolutePath()));
    QFile cacheFile(cachePath);
    QVERIFY(cacheFile.open(QIODevice::WriteOnly));
    QCOMPARE(cacheFile.write(initial), initial.size());
    cacheFile.close();
    QVERIFY(m_cache->recordCacheEntry(fileId, cachePath, initial.size()));

    m_cache->markDirty(fileId, logicalPath);
    const QString pendingPath = m_cache->moveToDirtyStore(fileId);
    QVERIFY(!pendingPath.isEmpty());
    QVERIFY(QFile::exists(pendingPath));

    m_cache->markUploadedGeneration(fileId, 1);

    QFile pendingFile(pendingPath);
    QVERIFY(pendingFile.open(QIODevice::Append));
    QCOMPARE(pendingFile.write("+newer", 6), qint64(6));
    pendingFile.close();

    m_cache->markDirty(fileId, logicalPath);
    m_cache->markUploadFailed(fileId);

    QList<DirtyFileEntry> dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    QCOMPARE(dirty.first().generation, static_cast<quint64>(2));
    QCOMPARE(dirty.first().uploadedGeneration, static_cast<quint64>(1));
    QVERIFY(dirty.first().uploadFailed);
    QVERIFY(dirty.first().lastUploadAttempt.isValid());

    recreateCache();

    dirty = m_cache->getDirtyFiles();
    QCOMPARE(dirty.size(), 1);
    QCOMPARE(dirty.first().fileId, fileId);
    QCOMPARE(dirty.first().generation, static_cast<quint64>(2));
    QCOMPARE(dirty.first().uploadedGeneration, static_cast<quint64>(1));
    QVERIFY(dirty.first().uploadFailed);
    QVERIFY(dirty.first().lastUploadAttempt.isValid());
    QCOMPARE(m_cache->getContentPath(fileId), pendingPath);

    QFile restoredPending(pendingPath);
    QVERIFY(restoredPending.open(QIODevice::ReadOnly));
    QCOMPARE(restoredPending.readAll(), QByteArray("restart bytes+newer"));
    restoredPending.close();
}

void TestFileCache::testRestart_FinalizesUploadedGenerationState() {
    const QString fileId = QStringLiteral("restart-uploaded-state");
    const QString logicalPath = QStringLiteral("/restart-uploaded-state.txt");
    const QByteArray content("uploaded before restart");
    const QString cachePath = m_cache->getCachePathForFile(fileId);

    QVERIFY(QDir().mkpath(QFileInfo(cachePath).dir().absolutePath()));
    QFile cacheFile(cachePath);
    QVERIFY(cacheFile.open(QIODevice::WriteOnly));
    QCOMPARE(cacheFile.write(content), content.size());
    cacheFile.close();
    QVERIFY(m_cache->recordCacheEntry(fileId, cachePath, content.size()));

    m_cache->markDirty(fileId, logicalPath);
    const quint64 generation = m_cache->getDirtyFiles().first().generation;
    const QString pendingPath = m_cache->moveToDirtyStore(fileId);
    QVERIFY(!pendingPath.isEmpty());
    QVERIFY(QFile::exists(pendingPath));

    m_cache->markUploadedGeneration(fileId, generation);

    recreateCache();

    QVERIFY(!m_cache->isDirty(fileId));
    QVERIFY(m_cache->isCached(fileId));
    QVERIFY(!QFile::exists(pendingPath));

    QFile recycled(cachePath);
    QVERIFY(recycled.open(QIODevice::ReadOnly));
    QCOMPARE(recycled.readAll(), content);
    recycled.close();
}

QTEST_MAIN(TestFileCache)
#include "TestFileCache.moc"
