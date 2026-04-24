/**
 * @file TestMetadataRefreshWorker.cpp
 * @brief Unit tests for MetadataRefreshWorker display-path logging payloads
 */

#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/GoogleDriveClient.h"
#include "fuse/MetadataCache.h"
#include "fuse/MetadataRefreshWorker.h"
#include "sync/SyncDatabase.h"

class FakeDriveClientMRW : public GoogleDriveClient {
    Q_OBJECT

   public:
    explicit FakeDriveClientMRW(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    void downloadFile(const QString&, const QString&) override {}
    void uploadFile(const QString&, const QString&, const QString&) override {}
    void updateFile(const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void renameFile(const QString&, const QString&) override {}
    void deleteFile(const QString&) override {}
    void createFolder(const QString&, const QString&, const QString&) override {}
    QJsonArray getParentsByFileId(const QString&) override { return {}; }
    QString getFolderIdByPath(const QString&) override { return {}; }

    void emitChangesBatch(const QList<DriveChange>& changes,
                          const QString& nextToken = QStringLiteral("next-token")) {
        emit changesReceived(changes, nextToken, false);
    }
};

class TestMetadataRefreshWorker : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testCreatedChange_EmitsResolvedPath();
    void testModifiedChange_EmitsResolvedPath();
    void testDeletedChange_EmitsStoredPathBeforeRemoval();
    void testCreatedChange_FallsBackToFileNameWhenPathUnavailable();

   private:
    static FuseFileMetadata makeFile(const QString& id, const QString& path,
                                     const QString& parentId = QStringLiteral("root"),
                                     qint64 size = 100);
    static FuseFileMetadata makeFolder(const QString& id, const QString& path,
                                       const QString& parentId = QStringLiteral("root"));
    static DriveChange makeChange(const QString& changeId, const QString& fileId,
                                  const QString& name, const QString& parentId,
                                  bool removed = false);

    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientMRW* m_driveClient = nullptr;
    MetadataCache* m_cache = nullptr;
    MetadataRefreshWorker* m_worker = nullptr;
};

FuseFileMetadata TestMetadataRefreshWorker::makeFile(const QString& id, const QString& path,
                                                     const QString& parentId, qint64 size) {
    FuseFileMetadata metadata;
    metadata.fileId = id;
    metadata.path = path;
    metadata.name = path.mid(path.lastIndexOf('/') + 1);
    metadata.remoteName = metadata.name;
    metadata.parentId = parentId;
    metadata.isFolder = false;
    metadata.size = size;
    metadata.mimeType = QStringLiteral("text/plain");
    metadata.createdTime = QDateTime::currentDateTimeUtc();
    metadata.modifiedTime = QDateTime::currentDateTimeUtc();
    metadata.cachedAt = QDateTime::currentDateTimeUtc();
    return metadata;
}

FuseFileMetadata TestMetadataRefreshWorker::makeFolder(const QString& id, const QString& path,
                                                       const QString& parentId) {
    FuseFileMetadata metadata;
    metadata.fileId = id;
    metadata.path = path;
    metadata.name = path.mid(path.lastIndexOf('/') + 1);
    metadata.remoteName = metadata.name;
    metadata.parentId = parentId;
    metadata.isFolder = true;
    metadata.size = 0;
    metadata.mimeType = QStringLiteral("application/vnd.google-apps.folder");
    metadata.createdTime = QDateTime::currentDateTimeUtc();
    metadata.modifiedTime = QDateTime::currentDateTimeUtc();
    metadata.cachedAt = QDateTime::currentDateTimeUtc();
    return metadata;
}

DriveChange TestMetadataRefreshWorker::makeChange(const QString& changeId, const QString& fileId,
                                                  const QString& name, const QString& parentId,
                                                  bool removed) {
    DriveFile file;
    file.id = fileId;
    file.name = name;
    file.parents = {parentId};
    file.mimeType = QStringLiteral("text/plain");
    file.ownedByMe = true;
    file.modifiedTime = QDateTime::currentDateTimeUtc();
    file.createdTime = QDateTime::currentDateTimeUtc();
    file.size = 123;

    DriveChange change;
    change.changeId = changeId;
    change.fileId = fileId;
    change.time = QDateTime::currentDateTimeUtc();
    change.removed = removed;
    change.file = file;
    return change;
}

void TestMetadataRefreshWorker::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);
    qputenv("HOME", m_tempDir->path().toUtf8());

    QSettings settings;
    settings.setValue(QStringLiteral("sync/duplicateNameStrategy"),
                      QStringLiteral("file-id-suffix"));

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());

    m_driveClient = new FakeDriveClientMRW(this);
    m_cache = new MetadataCache(m_db, m_driveClient, this);
    QVERIFY(m_cache->initialize());
    m_cache->setRootFolderId(QStringLiteral("root"));

    m_worker = new MetadataRefreshWorker(m_cache, nullptr, m_db, m_driveClient, this);
}

void TestMetadataRefreshWorker::cleanup() {
    delete m_worker;
    m_worker = nullptr;
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

void TestMetadataRefreshWorker::testCreatedChange_EmitsResolvedPath() {
    m_cache->setMetadata(makeFolder(QStringLiteral("folder-1"), QStringLiteral("Projects")));

    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-created"), QStringLiteral("file-created"),
                    QStringLiteral("report.txt"), QStringLiteral("folder-1"))});

    QCOMPARE(changeSpy.count(), 1);
    const QList<QVariant> args = changeSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("Projects/report.txt"));
    QCOMPARE(args.at(1).toString(), QStringLiteral("created"));
}

void TestMetadataRefreshWorker::testModifiedChange_EmitsResolvedPath() {
    m_cache->setMetadata(makeFolder(QStringLiteral("folder-1"), QStringLiteral("Projects")));
    m_cache->setMetadata(makeFile(QStringLiteral("file-modified"),
                                  QStringLiteral("Projects/report.txt"),
                                  QStringLiteral("folder-1")));

    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-modified"), QStringLiteral("file-modified"),
                    QStringLiteral("report.txt"), QStringLiteral("folder-1"))});

    QCOMPARE(changeSpy.count(), 1);
    const QList<QVariant> args = changeSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("Projects/report.txt"));
    QCOMPARE(args.at(1).toString(), QStringLiteral("modified"));
}

void TestMetadataRefreshWorker::testDeletedChange_EmitsStoredPathBeforeRemoval() {
    m_cache->setMetadata(makeFolder(QStringLiteral("folder-1"), QStringLiteral("Projects")));
    m_cache->setMetadata(makeFile(QStringLiteral("file-deleted"),
                                  QStringLiteral("Projects/report.txt"),
                                  QStringLiteral("folder-1")));

    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-deleted"), QStringLiteral("file-deleted"),
                    QStringLiteral("report.txt"), QStringLiteral("folder-1"), true)});

    QCOMPARE(changeSpy.count(), 1);
    const QList<QVariant> args = changeSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("Projects/report.txt"));
    QCOMPARE(args.at(1).toString(), QStringLiteral("deleted"));
    QVERIFY(!m_cache->getMetadataByFileId(QStringLiteral("file-deleted")).isValid());
}

void TestMetadataRefreshWorker::testCreatedChange_FallsBackToFileNameWhenPathUnavailable() {
    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-fallback"), QStringLiteral("file-fallback"),
                    QStringLiteral("orphan.txt"), QStringLiteral("missing-parent"))});

    QCOMPARE(changeSpy.count(), 1);
    const QList<QVariant> args = changeSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("orphan.txt"));
    QCOMPARE(args.at(1).toString(), QStringLiteral("created"));
}

QTEST_MAIN(TestMetadataRefreshWorker)
#include "TestMetadataRefreshWorker.moc"