/**
 * @file TestCacheMaintenance.cpp
 * @brief Unit tests for restart-gated FUSE cache maintenance.
 */

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "sync/SyncDatabase.h"
#include "utils/CacheMaintenance.h"

class TestCacheMaintenance : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testPurgeFuseRepresentationCache_ClearsEvictableState();
    void testPurgeFuseRepresentationCache_RemovesSymlinkLeavesOnly();
    void testPurgeFuseRepresentationCache_StopsWhenDatabaseClearFails();

   private:
    void writeFile(const QString& path, const QByteArray& contents = QByteArray("x"));

    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
};

void TestCacheMaintenance::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);
    qputenv("HOME", m_tempDir->path().toUtf8());

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());
}

void TestCacheMaintenance::cleanup() {
    if (m_db) {
        m_db->close();
        delete m_db;
        m_db = nullptr;
    }

    QStandardPaths::setTestModeEnabled(false);

    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestCacheMaintenance::writeFile(const QString& path, const QByteArray& contents) {
    QVERIFY(QDir().mkpath(QFileInfo(path).path()));

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(contents), contents.size());
    file.close();
}

void TestCacheMaintenance::testPurgeFuseRepresentationCache_ClearsEvictableState() {
    const QString cacheRoot = m_tempDir->path() + "/cache-root";
    const QString pendingRoot = m_tempDir->path() + "/persistent/Via/pending";
    const QString pendingSnapshotPath = pendingRoot + "/snapshots/dirty-file-1";
    const QString pendingFilePath = pendingRoot + "/dirty-file";
    const QString cacheFilePath = cacheRoot + "/Via/files/cached.bin";

    writeFile(cacheFilePath, QByteArray("cached-data"));
    writeFile(cacheRoot + "/metadata/index.json", QByteArray("stale-metadata"));
    writeFile(pendingFilePath, QByteArray("dirty-data"));
    writeFile(pendingSnapshotPath, QByteArray("snapshot-data"));

    FuseMetadata meta;
    meta.fileId = "REPRESENTATION_ID";
    meta.path = "/docs/report.txt";
    meta.name = "report.txt";
    meta.parentId = "PARENT_ID";
    meta.isFolder = false;
    meta.size = 128;
    meta.mimeType = "text/plain";
    meta.cachedAt = QDateTime::currentDateTime();
    meta.createdTime = QDateTime::currentDateTime();
    meta.modifiedTime = QDateTime::currentDateTime();
    meta.lastAccessed = QDateTime::currentDateTime();

    QVERIFY(m_db->saveFuseMetadata(meta));
    QVERIFY(m_db->recordFuseCacheEntry("REPRESENTATION_ID", cacheFilePath, 128));
    QVERIFY(m_db->markFuseDirty("DIRTY_ID", pendingFilePath));
    QVERIFY(m_db->setFuseSyncState("cursor", "abc123"));

    QVERIFY(CacheMaintenance::purgeFuseRepresentationCache(cacheRoot, *m_db));

    QDir cacheDir(cacheRoot);
    QVERIFY(cacheDir.exists());
    QVERIFY(cacheDir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());

    QVERIFY(QFile::exists(pendingFilePath));
    QVERIFY(QFile::exists(pendingSnapshotPath));

    QVERIFY(m_db->getFuseMetadata("REPRESENTATION_ID").fileId.isEmpty());
    QVERIFY(m_db->getFuseCacheEntries().isEmpty());
    QVERIFY(m_db->getFuseSyncState("cursor").isEmpty());

    const QList<FuseDirtyFile> dirtyFiles = m_db->getFuseDirtyFiles();
    bool foundDirtyFile = false;
    for (const FuseDirtyFile& dirtyFile : dirtyFiles) {
        if (dirtyFile.fileId == "DIRTY_ID") {
            foundDirtyFile = true;
            QCOMPARE(dirtyFile.path, pendingFilePath);
        }
    }
    QVERIFY(foundDirtyFile);
}

void TestCacheMaintenance::testPurgeFuseRepresentationCache_RemovesSymlinkLeavesOnly() {
    const QString cacheRoot = m_tempDir->path() + "/cache-root-symlinks";
    const QString externalRoot = m_tempDir->path() + "/external-root";
    const QString externalFilePath = externalRoot + "/outside.txt";
    const QString externalDirPath = externalRoot + "/outside-dir";
    const QString externalNestedPath = externalDirPath + "/nested.txt";
    const QString fileLinkPath = cacheRoot + "/outside-link.txt";
    const QString dirLinkPath = cacheRoot + "/outside-dir-link";

    QVERIFY(QDir().mkpath(cacheRoot));
    QVERIFY(QDir().mkpath(externalDirPath));
    writeFile(externalFilePath, QByteArray("outside-file"));
    writeFile(externalNestedPath, QByteArray("outside-dir-file"));

    if (!QFile::link(externalFilePath, fileLinkPath) ||
        !QFile::link(externalDirPath, dirLinkPath)) {
        QSKIP("Symlink creation not supported");
    }

    QVERIFY(m_db->recordFuseCacheEntry("LINK_ID", fileLinkPath, 12));
    QVERIFY(m_db->setFuseSyncState("cursor", "abc123"));

    QVERIFY(CacheMaintenance::purgeFuseRepresentationCache(cacheRoot, *m_db));

    QDir cacheDir(cacheRoot);
    QVERIFY(cacheDir.exists());
    QVERIFY(cacheDir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());

    QVERIFY(QFile::exists(externalFilePath));
    QVERIFY(QFile::exists(externalNestedPath));
    QVERIFY(!QFileInfo::exists(fileLinkPath));
    QVERIFY(!QFileInfo::exists(dirLinkPath));

    QVERIFY(m_db->getFuseCacheEntries().isEmpty());
    QVERIFY(m_db->getFuseSyncState("cursor").isEmpty());
}

void TestCacheMaintenance::testPurgeFuseRepresentationCache_StopsWhenDatabaseClearFails() {
    const QString cacheRoot = m_tempDir->path() + "/cache-root-failure";
    const QString cacheFilePath = cacheRoot + "/Via/files/cached.bin";

    writeFile(cacheFilePath, QByteArray("cached-data"));

    SyncDatabase closedDb;
    QVERIFY(!CacheMaintenance::purgeFuseRepresentationCache(cacheRoot, closedDb));
    QVERIFY(QFile::exists(cacheFilePath));
}

QTEST_MAIN(TestCacheMaintenance)
#include "TestCacheMaintenance.moc"