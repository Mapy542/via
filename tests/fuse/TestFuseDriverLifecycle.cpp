/**
 * @file TestFuseDriverLifecycle.cpp
 * @brief Targeted regression tests for FuseDriver dirty-file lifecycle helpers.
 */

#include <fcntl.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/GoogleDriveClient.h"
#include "fuse/FuseDriver.h"
#include "sync/SyncDatabase.h"

class FakeDriveClientFDL : public GoogleDriveClient {
    Q_OBJECT

   public:
    explicit FakeDriveClientFDL(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

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

class TestFuseDriverLifecycle : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testTruncateWithoutHandle_StagesDirtyFile();
    void testStageDirtyFileForUpload_RetargetsRemainingHandles();

   private:
    void seedCachedFile(const QString& fileId, const QString& path, const QByteArray& content);

    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientFDL* m_driveClient = nullptr;
    FuseDriver* m_driver = nullptr;
};

void TestFuseDriverLifecycle::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);
    qputenv("HOME", m_tempDir->path().toUtf8());

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());

    m_driveClient = new FakeDriveClientFDL(this);
    m_driver = new FuseDriver(m_driveClient, m_db, this);
    m_driver->setCacheDirectory(m_tempDir->path() + "/cache");
    QVERIFY(m_driver->initializeFileCache());
}

void TestFuseDriverLifecycle::cleanup() {
    delete m_driver;
    m_driver = nullptr;
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

void TestFuseDriverLifecycle::seedCachedFile(const QString& fileId, const QString& path, const QByteArray& content) {
    const QString cachePath = m_driver->fileCache()->getCachePathForFile(fileId);
    QVERIFY(!cachePath.isEmpty());
    QVERIFY(QDir().mkpath(QFileInfo(cachePath).dir().absolutePath()));

    QFile localFile(cachePath);
    QVERIFY(localFile.open(QIODevice::WriteOnly));
    QCOMPARE(localFile.write(content), content.size());
    localFile.close();

    QVERIFY(m_driver->fileCache()->recordCacheEntry(fileId, cachePath, content.size()));

    FuseMetadata meta;
    meta.fileId = fileId;
    meta.path = path;
    meta.name = QFileInfo(path).fileName();
    meta.remoteName = meta.name;
    meta.parentId = QStringLiteral("root");
    meta.isFolder = false;
    meta.size = content.size();
    meta.mimeType = QStringLiteral("text/plain");
    meta.createdTime = QDateTime::currentDateTimeUtc();
    meta.modifiedTime = QDateTime::currentDateTimeUtc();
    meta.cachedAt = QDateTime::currentDateTimeUtc();
    meta.lastAccessed = QDateTime::currentDateTimeUtc();
    QVERIFY(m_db->saveFuseMetadata(meta));
}

void TestFuseDriverLifecycle::testTruncateWithoutHandle_StagesDirtyFile() {
    const QString fileId = QStringLiteral("truncate_no_handle");
    const QString logicalPath = QStringLiteral("truncate.txt");

    seedCachedFile(fileId, logicalPath, QByteArray("abcdef"));

    QCOMPARE(m_driver->truncateWithoutHandle(fileId, 6, QStringLiteral("/") + logicalPath, 2), 0);

    QVERIFY(m_driver->fileCache()->isDirty(fileId));

    const QString pendingPath = m_driver->fileCache()->getDirtyPathForFile(fileId);
    QVERIFY(QFile::exists(pendingPath));
    QCOMPARE(QFileInfo(pendingPath).size(), qint64(2));

    const FuseMetadata stored = m_db->getFuseMetadata(fileId);
    QCOMPARE(stored.size, qint64(2));
}

void TestFuseDriverLifecycle::testStageDirtyFileForUpload_RetargetsRemainingHandles() {
    const QString fileId = QStringLiteral("retarget_handles");
    const QString logicalPath = QStringLiteral("retarget.txt");
    const QString fusePath = QStringLiteral("/") + logicalPath;

    seedCachedFile(fileId, logicalPath, QByteArray("hello"));
    m_driver->fileCache()->markDirty(fileId, logicalPath);

    const QString cachePath = m_driver->fileCache()->getCachePathForFile(fileId);
    const QByteArray encodedPath = QFile::encodeName(cachePath);

    FuseOpenFile releasingHandle;
    releasingHandle.fileId = fileId;
    releasingHandle.path = fusePath;
    releasingHandle.localFd = ::open(encodedPath.constData(), O_RDWR);
    QVERIFY(releasingHandle.localFd >= 0);
    releasingHandle.writable = true;
    releasingHandle.dirty = true;

    FuseOpenFile remainingHandle;
    remainingHandle.fileId = fileId;
    remainingHandle.path = fusePath;
    remainingHandle.localFd = ::open(encodedPath.constData(), O_RDWR);
    QVERIFY(remainingHandle.localFd >= 0);
    remainingHandle.writable = true;
    remainingHandle.dirty = false;

    const uint64_t releasingFh = m_driver->registerOpenFile(releasingHandle);
    const uint64_t remainingFh = m_driver->registerOpenFile(remainingHandle);

    QVERIFY(m_driver->stageDirtyFileForUpload(fileId, fusePath, releasingHandle.localFd));

    const QString pendingPath = m_driver->fileCache()->getDirtyPathForFile(fileId);
    QVERIFY(QFile::exists(pendingPath));

    const auto retargetedHandle = m_driver->getOpenFile(remainingFh);
    QVERIFY(retargetedHandle.has_value());
    QVERIFY(retargetedHandle->localFd >= 0);

    const QByteArray suffix("_more");
    QCOMPARE(::pwrite(retargetedHandle->localFd, suffix.constData(), suffix.size(), 5),
             static_cast<ssize_t>(suffix.size()));
    QVERIFY(::fsync(retargetedHandle->localFd) == 0);

    QFile pendingFile(pendingPath);
    QVERIFY(pendingFile.open(QIODevice::ReadOnly));
    QCOMPARE(pendingFile.readAll(), QByteArray("hello_more"));
    pendingFile.close();

    m_driver->unregisterOpenFile(releasingFh);
    m_driver->unregisterOpenFile(remainingFh);
}

QTEST_MAIN(TestFuseDriverLifecycle)
#include "TestFuseDriverLifecycle.moc"