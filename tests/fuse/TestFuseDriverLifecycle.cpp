/**
 * @file TestFuseDriverLifecycle.cpp
 * @brief Targeted regression tests for FuseDriver dirty-file lifecycle helpers.
 */

#include <fcntl.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <functional>
#include <thread>

#include "api/GoogleDriveClient.h"
#include "fuse/FileCache.h"
#include "fuse/FuseDriver.h"
#include "fuse/MetadataCache.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncDatabase.h"

class FakeDriveClientFDL : public GoogleDriveClient {
    Q_OBJECT

   public:
    explicit FakeDriveClientFDL(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    QByteArray exportPayload = QByteArray("exported native doc");
    int exportCallCount = 0;
    QString lastExportFileId;
    QString lastExportMimeType;

    void downloadFile(const QString&, const QString&) override {}
    void exportFile(const QString& fileId, const QString& exportMimeType,
                    const QString& localPath) override {
        ++exportCallCount;
        lastExportFileId = fileId;
        lastExportMimeType = exportMimeType;

        QFileInfo fileInfo(localPath);
        QDir().mkpath(fileInfo.dir().absolutePath());

        QFile file(localPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(exportPayload), exportPayload.size());
        file.close();

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

class TestFuseDriverLifecycle : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testSaveMetadataEntry_UpdatesWarmRootListing();
    void testRemoveMetadataEntryFromCache_PrunesWarmRootListing();
    void testReconcileMovedMetadata_DropsStaleFolderSubtree();

    void testTruncateWithoutHandle_StagesDirtyFile();
    void testStageDirtyFileForUpload_RetargetsRemainingHandles();
    void testPauseSync_StagesDirtyFilesIntoPersistentStore();
    void testRegisterOpenFile_TracksWritableHandlesSeparately();
    void testNativeDocReportedSize_DoesNotMaterializeExportOnFirstStat();
    void testNativeDocExportLimitFailure_FallsBackToBrowserShortcutOverride();

   private:
    static FuseMetadata makeMetadata(const QString& fileId, const QString& path,
                                     const QString& parentId, bool isFolder,
                                     const QString& mimeType = QStringLiteral("text/plain"));
    static DriveFile makeDriveFile(const FuseMetadata& metadata);
    void seedCachedFile(const QString& fileId, const QString& path, const QByteArray& content);

    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientFDL* m_driveClient = nullptr;
    FuseDriver* m_driver = nullptr;
};

FuseMetadata TestFuseDriverLifecycle::makeMetadata(const QString& fileId, const QString& path,
                                                   const QString& parentId, bool isFolder,
                                                   const QString& mimeType) {
    FuseMetadata meta;
    meta.fileId = fileId;
    meta.path = path;
    meta.name = QFileInfo(path).fileName();
    meta.remoteName = meta.name;
    meta.parentId = parentId;
    meta.isFolder = isFolder;
    meta.size = isFolder ? 0 : 64;
    meta.mimeType = isFolder ? QStringLiteral("application/vnd.google-apps.folder") : mimeType;
    meta.createdTime = QDateTime::currentDateTimeUtc();
    meta.modifiedTime = QDateTime::currentDateTimeUtc();
    meta.cachedAt = QDateTime::currentDateTimeUtc();
    meta.lastAccessed = QDateTime::currentDateTimeUtc();
    return meta;
}

DriveFile TestFuseDriverLifecycle::makeDriveFile(const FuseMetadata& metadata) {
    DriveFile file;
    file.id = metadata.fileId;
    file.name = metadata.remoteName.isEmpty() ? metadata.name : metadata.remoteName;
    file.parents = {metadata.parentId};
    file.isFolder = metadata.isFolder;
    file.size = metadata.size;
    file.mimeType = metadata.mimeType;
    file.createdTime = metadata.createdTime;
    file.modifiedTime = metadata.modifiedTime;
    file.webViewLink = metadata.webViewLink;
    return file;
}

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
    if (m_driver) {
        m_driver->stopBackgroundWorkers();
    }
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

void TestFuseDriverLifecycle::seedCachedFile(const QString& fileId, const QString& path,
                                             const QByteArray& content) {
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

void TestFuseDriverLifecycle::testSaveMetadataEntry_UpdatesWarmRootListing() {
    QVERIFY(m_driver->initializeMetadataCache());

    QVERIFY(m_driver->m_metadataCache->replaceRemoteChildren(QStringLiteral("root"), {}).isEmpty());
    QVERIFY(m_driver->m_metadataCache->hasChildrenCached(QStringLiteral("/")));
    QCOMPARE(m_driver->m_metadataCache->getChildren(QStringLiteral("/")).size(), 0);

    const FuseMetadata createdFolder =
        makeMetadata(QStringLiteral("created-folder"), QStringLiteral("fresh-folder"),
                     QStringLiteral("root"), true);

    QVERIFY(m_driver->saveMetadataEntry(createdFolder));

    QVERIFY(m_driver->m_metadataCache->hasChildrenCached(QStringLiteral("/")));
    const QList<FuseFileMetadata> children =
        m_driver->m_metadataCache->getChildren(QStringLiteral("/"));
    QCOMPARE(children.size(), 1);
    QCOMPARE(children.first().fileId, createdFolder.fileId);
    QCOMPARE(children.first().path, createdFolder.path);
    QCOMPARE(m_db->getFuseMetadataByPath(createdFolder.path).fileId, createdFolder.fileId);
}

void TestFuseDriverLifecycle::testRemoveMetadataEntryFromCache_PrunesWarmRootListing() {
    QVERIFY(m_driver->initializeMetadataCache());

    const FuseMetadata existingFile = makeMetadata(
        QStringLiteral("cached-file"), QStringLiteral("report.txt"), QStringLiteral("root"), false);
    QVERIFY(m_driver->saveMetadataEntry(existingFile));
    m_driver->m_metadataCache->replaceRemoteChildren(QStringLiteral("root"),
                                                     {makeDriveFile(existingFile)});
    QVERIFY(m_driver->m_metadataCache->hasChildrenCached(QStringLiteral("/")));
    QCOMPARE(m_driver->m_metadataCache->getChildren(QStringLiteral("/")).size(), 1);

    QVERIFY(m_db->deleteFuseMetadata(existingFile.fileId));
    m_driver->removeMetadataEntryFromCache(existingFile);

    QVERIFY(m_driver->m_metadataCache->hasChildrenCached(QStringLiteral("/")));
    QCOMPARE(m_driver->m_metadataCache->getChildren(QStringLiteral("/")).size(), 0);
    QVERIFY(!m_driver->m_metadataCache->getMetadataByPath(existingFile.path).isValid());
}

void TestFuseDriverLifecycle::testReconcileMovedMetadata_DropsStaleFolderSubtree() {
    QVERIFY(m_driver->initializeMetadataCache());

    const FuseMetadata oldFolder = makeMetadata(QStringLiteral("folder-1"), QStringLiteral("alpha"),
                                                QStringLiteral("root"), true);
    const FuseMetadata childFile = makeMetadata(
        QStringLiteral("child-1"), QStringLiteral("alpha/note.txt"), oldFolder.fileId, false);

    QVERIFY(m_driver->saveMetadataEntry(oldFolder));
    m_driver->m_metadataCache->replaceRemoteChildren(QStringLiteral("root"),
                                                     {makeDriveFile(oldFolder)});
    m_driver->m_metadataCache->replaceRemoteChildren(oldFolder.fileId, {makeDriveFile(childFile)});

    QVERIFY(m_driver->m_metadataCache->hasChildrenCached(QStringLiteral("/")));
    QVERIFY(m_driver->m_metadataCache->hasChildrenCached(oldFolder.path));
    QVERIFY(m_driver->m_metadataCache->getMetadataByPath(childFile.path).isValid());

    FuseMetadata renamedFolder = oldFolder;
    renamedFolder.path = QStringLiteral("beta");
    renamedFolder.name = QStringLiteral("beta");
    renamedFolder.remoteName = QStringLiteral("beta");
    renamedFolder.cachedAt = QDateTime::currentDateTimeUtc();
    renamedFolder.lastAccessed = QDateTime::currentDateTimeUtc();

    QVERIFY(m_driver->reconcileMovedMetadata(oldFolder, renamedFolder));

    QCOMPARE(m_db->getFuseMetadataByPath(QStringLiteral("beta")).fileId, oldFolder.fileId);
    QCOMPARE(m_db->getFuseMetadataByPath(QStringLiteral("beta/note.txt")).fileId, childFile.fileId);

    QVERIFY(!m_driver->m_metadataCache->getMetadataByPath(QStringLiteral("alpha")).isValid());
    QVERIFY(
        !m_driver->m_metadataCache->getMetadataByPath(QStringLiteral("alpha/note.txt")).isValid());
    QVERIFY(
        !m_driver->m_metadataCache->getMetadataByPath(QStringLiteral("beta/note.txt")).isValid());

    const QList<FuseFileMetadata> rootChildren =
        m_driver->m_metadataCache->getChildren(QStringLiteral("/"));
    QCOMPARE(rootChildren.size(), 1);
    QCOMPARE(rootChildren.first().path, QStringLiteral("beta"));
    QVERIFY(!m_driver->m_metadataCache->hasChildrenCached(QStringLiteral("beta")));
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

void TestFuseDriverLifecycle::testPauseSync_StagesDirtyFilesIntoPersistentStore() {
    const QString fileId = QStringLiteral("pause-stage");
    const QString logicalPath = QStringLiteral("pause-stage.txt");

    seedCachedFile(fileId, logicalPath, QByteArray("pause me"));
    m_driver->fileCache()->markDirty(fileId, logicalPath);

    RuntimePauseController pauseController;
    m_driver->setPauseController(&pauseController);

    pauseController.requestManualPause();
    m_driver->pauseSync();

    const QString pendingPath = m_driver->fileCache()->getDirtyPathForFile(fileId);
    QVERIFY(QFile::exists(pendingPath));
    QVERIFY(!m_driver->fileCache()->isCached(fileId));

    QFile pendingFile(pendingPath);
    QVERIFY(pendingFile.open(QIODevice::ReadOnly));
    QCOMPARE(pendingFile.readAll(), QByteArray("pause me"));
    pendingFile.close();
}

void TestFuseDriverLifecycle::testRegisterOpenFile_TracksWritableHandlesSeparately() {
    const QString fileId = QStringLiteral("tracked_handle_modes");
    const QString logicalPath = QStringLiteral("tracked.txt");

    seedCachedFile(fileId, logicalPath, QByteArray("tracked"));

    const QString cachePath = m_driver->fileCache()->getCachePathForFile(fileId);
    const QByteArray encodedPath = QFile::encodeName(cachePath);

    FuseOpenFile readOnlyHandle;
    readOnlyHandle.fileId = fileId;
    readOnlyHandle.path = QStringLiteral("/") + logicalPath;
    readOnlyHandle.localFd = ::open(encodedPath.constData(), O_RDONLY);
    QVERIFY(readOnlyHandle.localFd >= 0);
    readOnlyHandle.writable = false;

    const uint64_t readOnlyFh = m_driver->registerOpenFile(readOnlyHandle);
    QVERIFY(m_driver->fileCache()->hasOpenHandles(fileId));
    QVERIFY(!m_driver->fileCache()->hasOpenWritableHandles(fileId));
    m_driver->unregisterOpenFile(readOnlyFh);

    FuseOpenFile writableHandle;
    writableHandle.fileId = fileId;
    writableHandle.path = QStringLiteral("/") + logicalPath;
    writableHandle.localFd = ::open(encodedPath.constData(), O_RDWR);
    QVERIFY(writableHandle.localFd >= 0);
    writableHandle.writable = true;

    const uint64_t writableFh = m_driver->registerOpenFile(writableHandle);
    QVERIFY(m_driver->fileCache()->hasOpenHandles(fileId));
    QVERIFY(m_driver->fileCache()->hasOpenWritableHandles(fileId));
    m_driver->unregisterOpenFile(writableFh);

    QVERIFY(!m_driver->fileCache()->hasOpenHandles(fileId));
    QVERIFY(!m_driver->fileCache()->hasOpenWritableHandles(fileId));
}

void TestFuseDriverLifecycle::testNativeDocReportedSize_DoesNotMaterializeExportOnFirstStat() {
    const QString fileId = QStringLiteral("native-doc-first-stat");
    const QByteArray payload =
        QByteArray("PK") + QByteArray::fromHex("0304") + QByteArray("fake-ods-payload");
    const QString exportMimeType = QStringLiteral("application/vnd.oasis.opendocument.spreadsheet");

    FuseMetadata meta;
    meta.fileId = fileId;
    meta.path = QStringLiteral("report.ods");
    meta.name = QStringLiteral("report.ods");
    meta.remoteName = QStringLiteral("report");
    meta.parentId = QStringLiteral("root");
    meta.isFolder = false;
    meta.size = 0;
    meta.mimeType = exportMimeType;
    meta.remoteMimeType = QStringLiteral("application/vnd.google-apps.spreadsheet");
    meta.createdTime = QDateTime::currentDateTimeUtc();
    meta.modifiedTime = QDateTime::currentDateTimeUtc();
    meta.cachedAt = QDateTime::currentDateTimeUtc();
    meta.lastAccessed = QDateTime::currentDateTimeUtc();

    m_driveClient->exportPayload = payload;

    QSignalSpy completedSpy(m_driver->fileCache(), &FileCache::downloadCompleted);
    QVERIFY(completedSpy.isValid());

    const qint64 reportedSize = m_driver->nativeDocReportedSize(meta, NativeDocMode::OpenDocument);

    QCOMPARE(reportedSize, qint64(0));
    QCOMPARE(completedSpy.count(), 0);
    QCOMPARE(m_driveClient->exportCallCount, 0);
    QVERIFY(!m_driver->fileCache()->isCached(fileId, exportMimeType));

    QString exportedPath;
    JoiningThread worker(
        [&]() { exportedPath = m_driver->fileCache()->getExportedPath(fileId, exportMimeType); });

    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    worker.join();

    QVERIFY(!exportedPath.isEmpty());
    QCOMPARE(m_driveClient->exportCallCount, 1);
    QCOMPARE(m_driveClient->lastExportFileId, fileId);
    QCOMPARE(m_driveClient->lastExportMimeType, exportMimeType);
    QVERIFY(m_driver->fileCache()->isCached(fileId, exportMimeType));

    const qint64 cachedSize = m_driver->nativeDocReportedSize(meta, NativeDocMode::OpenDocument);
    QCOMPARE(cachedSize, qint64(payload.size()));
    QCOMPARE(m_driveClient->exportCallCount, 1);
}

void TestFuseDriverLifecycle::testNativeDocExportLimitFailure_FallsBackToBrowserShortcutOverride() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "open-document");

    QVERIFY(m_driver->initializeMetadataCache());
    m_driver->startBackgroundWorkers();

    const QString fileId = QStringLiteral("native-doc-export-limit");

    FuseMetadata meta;
    meta.fileId = fileId;
    meta.path = QStringLiteral("report.odt");
    meta.name = QStringLiteral("report.odt");
    meta.remoteName = QStringLiteral("report");
    meta.parentId = QStringLiteral("root");
    meta.isFolder = false;
    meta.size = 0;
    meta.mimeType = QStringLiteral("application/vnd.google-apps.document");
    meta.remoteMimeType = QStringLiteral("application/vnd.google-apps.document");
    meta.webViewLink = QStringLiteral("https://docs.google.com/document/d/report/edit");
    meta.createdTime = QDateTime::currentDateTimeUtc();
    meta.modifiedTime = QDateTime::currentDateTimeUtc();
    meta.cachedAt = QDateTime::currentDateTimeUtc();
    meta.lastAccessed = QDateTime::currentDateTimeUtc();
    QVERIFY(m_db->saveFuseMetadata(meta));

    m_driver->fileCache()->downloadFailedDetailed(
        fileId, QStringLiteral("This file is too large to be exported."), 403);

    QTRY_COMPARE_WITH_TIMEOUT(m_db->getFuseMetadata(fileId).nativeDocModeOverride,
                              QString("browser-shortcut"), 2000);

    const FuseMetadata updated = m_db->getFuseMetadata(fileId);
    QCOMPARE(updated.path, QString("report.gdoc"));
    QCOMPARE(updated.name, QString("report.gdoc"));

    QVERIFY(m_db->getFuseMetadataByPath(QStringLiteral("report.odt")).fileId.isEmpty());
    QCOMPARE(m_db->getFuseMetadataByPath(QStringLiteral("report.gdoc")).fileId, fileId);
}

QTEST_MAIN(TestFuseDriverLifecycle)
#include "TestFuseDriverLifecycle.moc"