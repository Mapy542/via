/**
 * @file TestMetadataCache.cpp
 * @brief Unit tests for MetadataCache — verifies in-memory + DB-backed
 *        metadata CRUD after the C2/L2 refactor (all DB access now goes
 *        through SyncDatabase).
 */

#include <QDir>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/GoogleDriveClient.h"
#include "fuse/MetadataCache.h"
#include "sync/SyncDatabase.h"

// ---------------------------------------------------------------------------
// Minimal FakeDriveClient — enough for MetadataCache construction
// ---------------------------------------------------------------------------
class FakeDriveClientMC : public GoogleDriveClient {
    Q_OBJECT
   public:
    explicit FakeDriveClientMC(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    void downloadFile(const QString&, const QString&) override {}
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
class TestMetadataCache : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // Basic in-memory operations
    void testSetAndGetByPath();
    void testGetByFileId();
    void testRemoveMetadata();
    void testClearAll();
    void testSetMetadataBatch();
    void testGetChildren();

    // Persistence: survives reinitialisation from DB
    void testPersistence_SurvivesReinit();
    void testReplaceRemoteChildren_DisambiguatesDuplicatesWithFileIdSuffix();
    void testReplaceRemoteChildren_DisambiguatesDuplicatesWithNumericSuffix();
    void testUpsertRemoteMetadata_DisambiguatesAgainstExistingSibling();
    void testDuplicateFolderAliasesKeepDistinctChildren();
    void testDuplicateAliasesPersistAcrossReinit();

    // Edge cases
    void testGetByPath_UnknownReturnsInvalid();
    void testGetByFileId_UnknownReturnsInvalid();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientMC* m_driveClient = nullptr;
    MetadataCache* m_cache = nullptr;

    static FuseFileMetadata makeFile(const QString& id, const QString& path,
                                     const QString& parentId = "root", qint64 size = 100);
    static FuseFileMetadata makeFolder(const QString& id, const QString& path,
                                       const QString& parentId = "root");
    static DriveFile makeRemoteEntry(const QString& id, const QString& name,
                                     const QString& parentId = "root", bool isFolder = false,
                                     qint64 size = 100);
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
FuseFileMetadata TestMetadataCache::makeFile(const QString& id, const QString& path,
                                             const QString& parentId, qint64 size) {
    FuseFileMetadata m;
    m.fileId = id;
    m.path = path;
    m.name = path.mid(path.lastIndexOf('/') + 1);
    m.remoteName = m.name;
    m.parentId = parentId;
    m.isFolder = false;
    m.size = size;
    m.mimeType = "text/plain";
    m.createdTime = QDateTime::currentDateTimeUtc();
    m.modifiedTime = QDateTime::currentDateTimeUtc();
    m.cachedAt = QDateTime::currentDateTimeUtc();
    return m;
}

FuseFileMetadata TestMetadataCache::makeFolder(const QString& id, const QString& path,
                                               const QString& parentId) {
    FuseFileMetadata m;
    m.fileId = id;
    m.path = path;
    m.name = path.mid(path.lastIndexOf('/') + 1);
    m.remoteName = m.name;
    m.parentId = parentId;
    m.isFolder = true;
    m.size = 0;
    m.mimeType = "application/vnd.google-apps.folder";
    m.createdTime = QDateTime::currentDateTimeUtc();
    m.modifiedTime = QDateTime::currentDateTimeUtc();
    m.cachedAt = QDateTime::currentDateTimeUtc();
    return m;
}

DriveFile TestMetadataCache::makeRemoteEntry(const QString& id, const QString& name,
                                             const QString& parentId, bool isFolder, qint64 size) {
    DriveFile file;
    file.id = id;
    file.name = name;
    file.parents = {parentId};
    file.isFolder = isFolder;
    file.size = isFolder ? 0 : size;
    file.mimeType = isFolder ? QStringLiteral("application/vnd.google-apps.folder")
                             : QStringLiteral("text/plain");
    file.createdTime = QDateTime::currentDateTimeUtc();
    file.modifiedTime = QDateTime::currentDateTimeUtc();
    return file;
}

// ---------------------------------------------------------------------------
// Setup / Teardown
// ---------------------------------------------------------------------------
void TestMetadataCache::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);
    qputenv("HOME", m_tempDir->path().toUtf8());

    QSettings settings;
    settings.setValue("sync/duplicateNameStrategy", "file-id-suffix");

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());

    m_driveClient = new FakeDriveClientMC(this);
    m_cache = new MetadataCache(m_db, m_driveClient, this);
    QVERIFY(m_cache->initialize());
    m_cache->setRootFolderId("root");
}

void TestMetadataCache::cleanup() {
    delete m_cache;
    m_cache = nullptr;
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

// ---------------------------------------------------------------------------
// Tests — CRUD
// ---------------------------------------------------------------------------
void TestMetadataCache::testSetAndGetByPath() {
    auto f = makeFile("id1", "docs/readme.txt");
    m_cache->setMetadata(f);

    auto got = m_cache->getMetadataByPath("docs/readme.txt");
    QVERIFY(got.isValid());
    QCOMPARE(got.fileId, QString("id1"));
    QCOMPARE(got.size, qint64(100));
}

void TestMetadataCache::testGetByFileId() {
    auto f = makeFile("id2", "data.bin");
    m_cache->setMetadata(f);

    auto got = m_cache->getMetadataByFileId("id2");
    QVERIFY(got.isValid());
    QCOMPARE(got.path, QString("data.bin"));
}

void TestMetadataCache::testRemoveMetadata() {
    auto f = makeFile("id3", "temp.txt");
    m_cache->setMetadata(f);
    QVERIFY(m_cache->getMetadataByPath("temp.txt").isValid());

    m_cache->removeByPath("temp.txt");
    QVERIFY(!m_cache->getMetadataByPath("temp.txt").isValid());
}

void TestMetadataCache::testClearAll() {
    m_cache->setMetadata(makeFile("a", "a.txt"));
    m_cache->setMetadata(makeFile("b", "b.txt"));

    m_cache->clearAll();

    QVERIFY(!m_cache->getMetadataByPath("a.txt").isValid());
    QVERIFY(!m_cache->getMetadataByPath("b.txt").isValid());
}

void TestMetadataCache::testSetMetadataBatch() {
    QList<FuseFileMetadata> batch;
    batch << makeFile("x1", "x/1.txt", "xdir") << makeFile("x2", "x/2.txt", "xdir");

    m_cache->setMetadataBatch(batch);

    QVERIFY(m_cache->getMetadataByPath("x/1.txt").isValid());
    QVERIFY(m_cache->getMetadataByPath("x/2.txt").isValid());
}

void TestMetadataCache::testGetChildren() {
    auto dir = makeFolder("dir1", "photos");
    auto c1 = makeFile("p1", "photos/a.jpg", "dir1");
    auto c2 = makeFile("p2", "photos/b.jpg", "dir1");
    auto outer = makeFile("o1", "other.txt");

    m_cache->setMetadata(dir);
    m_cache->setMetadata(c1);
    m_cache->setMetadata(c2);
    m_cache->setMetadata(outer);

    auto children = m_cache->getChildren("photos");
    QCOMPARE(children.size(), 2);

    QSet<QString> ids;
    for (const auto& c : children) ids.insert(c.fileId);
    QVERIFY(ids.contains("p1"));
    QVERIFY(ids.contains("p2"));
}

// ---------------------------------------------------------------------------
// Persistence — recreate cache from DB
// ---------------------------------------------------------------------------
void TestMetadataCache::testPersistence_SurvivesReinit() {
    auto f = makeFile("persist1", "saved.txt");
    m_cache->setMetadata(f);

    // Destroy in-memory cache
    delete m_cache;

    // Re-create and re-init from the same database
    m_cache = new MetadataCache(m_db, m_driveClient, this);
    QVERIFY(m_cache->initialize());

    auto got = m_cache->getMetadataByPath("saved.txt");
    QVERIFY(got.isValid());
    QCOMPARE(got.fileId, QString("persist1"));
}

void TestMetadataCache::testReplaceRemoteChildren_DisambiguatesDuplicatesWithFileIdSuffix() {
    QList<DriveFile> files;
    files << makeRemoteEntry("idA", "report.txt") << makeRemoteEntry("idB", "report.txt");

    const QList<FuseFileMetadata> stored = m_cache->replaceRemoteChildren("root", files);

    QCOMPARE(stored.size(), 2);
    QCOMPARE(m_cache->getPathByFileId("idA"), QString("report.txt"));
    QCOMPARE(m_cache->getPathByFileId("idB"), QString("report_idB.txt"));

    const QList<FuseFileMetadata> children = m_cache->getChildren("/");
    QCOMPARE(children.size(), 2);
}

void TestMetadataCache::testReplaceRemoteChildren_DisambiguatesDuplicatesWithNumericSuffix() {
    QSettings settings;
    settings.setValue("sync/duplicateNameStrategy", "numeric-suffix");

    delete m_cache;
    m_cache = new MetadataCache(m_db, m_driveClient, this);
    QVERIFY(m_cache->initialize());
    m_cache->setRootFolderId("root");

    QList<DriveFile> files;
    files << makeRemoteEntry("idA", "report.txt") << makeRemoteEntry("idB", "report.txt");

    const QList<FuseFileMetadata> stored = m_cache->replaceRemoteChildren("root", files);

    QCOMPARE(stored.size(), 2);
    QCOMPARE(m_cache->getPathByFileId("idA"), QString("report.txt"));
    QCOMPARE(m_cache->getPathByFileId("idB"), QString("report (1).txt"));
}

void TestMetadataCache::testUpsertRemoteMetadata_DisambiguatesAgainstExistingSibling() {
    QList<DriveFile> initial;
    initial << makeRemoteEntry("idA", "report.txt");
    m_cache->replaceRemoteChildren("root", initial);

    const FuseFileMetadata stored =
        m_cache->upsertRemoteMetadata(makeRemoteEntry("idB", "report.txt"));

    QVERIFY(stored.isValid());
    QCOMPARE(stored.path, QString("report_idB.txt"));
    QCOMPARE(m_cache->getPathByFileId("idB"), QString("report_idB.txt"));
}

void TestMetadataCache::testDuplicateFolderAliasesKeepDistinctChildren() {
    QList<DriveFile> folders;
    folders << makeRemoteEntry("folderA", "dup", "root", true)
            << makeRemoteEntry("folderB", "dup", "root", true);

    m_cache->replaceRemoteChildren("root", folders);

    QList<DriveFile> leftChildren;
    leftChildren << makeRemoteEntry("childA", "inside.txt", "folderA");
    QList<DriveFile> rightChildren;
    rightChildren << makeRemoteEntry("childB", "inside.txt", "folderB");

    m_cache->replaceRemoteChildren("folderA", leftChildren);
    m_cache->replaceRemoteChildren("folderB", rightChildren);

    QCOMPARE(m_cache->getPathByFileId("folderA"), QString("dup"));
    QCOMPARE(m_cache->getPathByFileId("folderB"), QString("dup_folderB"));
    QCOMPARE(m_cache->getPathByFileId("childA"), QString("dup/inside.txt"));
    QCOMPARE(m_cache->getPathByFileId("childB"), QString("dup_folderB/inside.txt"));
}

void TestMetadataCache::testDuplicateAliasesPersistAcrossReinit() {
    QList<DriveFile> initial;
    initial << makeRemoteEntry("idA", "report.txt") << makeRemoteEntry("idB", "report.txt");
    m_cache->replaceRemoteChildren("root", initial);

    QList<DriveFile> refreshed;
    refreshed << makeRemoteEntry("idB", "report.txt");
    m_cache->replaceRemoteChildren("root", refreshed);

    delete m_cache;
    m_cache = new MetadataCache(m_db, m_driveClient, this);
    QVERIFY(m_cache->initialize());
    m_cache->setRootFolderId("root");

    QCOMPARE(m_cache->getPathByFileId("idB"), QString("report_idB.txt"));

    const QList<FuseFileMetadata> children = m_cache->getChildren("/");
    QCOMPARE(children.size(), 1);
    QCOMPARE(children.first().path, QString("report_idB.txt"));
}

// ---------------------------------------------------------------------------
// Edge cases — misses
// ---------------------------------------------------------------------------
void TestMetadataCache::testGetByPath_UnknownReturnsInvalid() {
    QVERIFY(!m_cache->getMetadataByPath("nonexistent").isValid());
}

void TestMetadataCache::testGetByFileId_UnknownReturnsInvalid() {
    QVERIFY(!m_cache->getMetadataByFileId("no_such_id").isValid());
}

QTEST_MAIN(TestMetadataCache)
#include "TestMetadataCache.moc"
