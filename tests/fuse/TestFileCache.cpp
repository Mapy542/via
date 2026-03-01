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

#include "api/GoogleDriveClient.h"
#include "fuse/FileCache.h"
#include "sync/SyncDatabase.h"

// ---------------------------------------------------------------------------
// Minimal FakeDriveClient — enough for FileCache construction
// ---------------------------------------------------------------------------
class FakeDriveClientFC : public GoogleDriveClient {
    Q_OBJECT
   public:
    explicit FakeDriveClientFC(QObject* parent = nullptr)
        : GoogleDriveClient(nullptr, parent) {}

    void downloadFile(const QString& /*fileId*/, const QString& /*localPath*/) override {}
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

   private:
    void createTestDatabase();
    void destroyTestDatabase();

    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientFC* m_driveClient = nullptr;
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
    m_cache = new FileCache(m_db, m_driveClient, this);

    // Set cache dir inside temp
    QString cacheDir = m_tempDir->path() + "/cache";
    QDir().mkpath(cacheDir);
    m_cache->setCacheDirectory(cacheDir);
    QVERIFY(m_cache->initialize());
}

void TestFileCache::cleanup() {
    delete m_cache;
    m_cache = nullptr;
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

// ---------------------------------------------------------------------------
// Helpers — put a file into cache so invalidate / remove have something to act on
// ---------------------------------------------------------------------------
static void seedCacheFile(FileCache* cache, const QString& cacheDir,
                          const QString& fileId, qint64 size = 100) {
    // Create a real file on disk inside the cache directory
    QString filePath = cacheDir + "/" + fileId;
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

QTEST_MAIN(TestFileCache)
#include "TestFileCache.moc"
