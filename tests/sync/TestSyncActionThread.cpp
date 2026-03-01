/**
 * @file TestSyncActionThread.cpp
 * @brief Unit tests for SyncActionThread wake and execution behavior
 */

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncActionThread.h"
#include "sync/SyncDatabase.h"

class FakeGoogleDriveClient : public GoogleDriveClient {
    Q_OBJECT

   public:
    struct InjectedError {
        QString errorMsg;
        int httpStatus = 0;
        int remaining = 1;
    };

    struct UploadCall {
        QString localPath;
        QString parentId;
        QString fileName;
    };

    struct MoveCall {
        QString fileId;
        QString newParentId;
        QString oldParentId;
    };

    struct RenameCall {
        QString fileId;
        QString newName;
    };

    struct DeleteCall {
        QString fileId;
    };

    struct DownloadCall {
        QString fileId;
        QString localPath;
    };

    struct FolderCall {
        QString name;
        QString parentId;
        QString localPath;
    };

    explicit FakeGoogleDriveClient(QObject* parent = nullptr)
        : GoogleDriveClient(nullptr, parent) {}

    void setFolderIdForPath(const QString& path, const QString& id) {
        QString normalized = QDir::cleanPath(path);
        if (normalized == ".") {
            normalized.clear();
        }
        m_folderIdByPath.insert(normalized, id);
        m_folderPathById.insert(id, normalized);
    }

    void setParentForFileId(const QString& fileId, const QString& parentId) {
        m_parentByFileId.insert(fileId, parentId);
    }

    void injectOperationError(const QString& operation, const QString& errorMsg, int httpStatus,
                              int remaining = 1) {
        InjectedError injected;
        injected.errorMsg = errorMsg;
        injected.httpStatus = httpStatus;
        injected.remaining = remaining;
        m_injectedErrors.insert(operation, injected);
    }

    QString lastUploadedFileId() const { return m_lastUploadedFileId; }
    UploadCall lastUploadCall() const { return m_lastUploadCall; }
    MoveCall lastMoveCall() const { return m_lastMoveCall; }
    RenameCall lastRenameCall() const { return m_lastRenameCall; }
    DeleteCall lastDeleteCall() const { return m_lastDeleteCall; }
    DownloadCall lastDownloadCall() const { return m_lastDownloadCall; }
    FolderCall lastFolderCall() const { return m_lastFolderCall; }
    int uploadCallCount() const { return m_uploadCallCount; }
    int folderCallCount() const { return m_folderCallCount; }

    void downloadFile(const QString& fileId, const QString& localPath) override {
        m_lastDownloadCall = {fileId, localPath};
        QFileInfo info(localPath);
        QDir dir(info.dir());
        dir.mkpath(".");
        QFile file(localPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("data");
            file.close();
        }
        emit fileDownloaded(fileId, localPath);
    }

    void uploadFile(const QString& localPath, const QString& parentId,
                    const QString& fileName) override {
        ++m_uploadCallCount;
        m_lastUploadCall = {localPath, parentId, fileName};
        if (emitInjectedError("uploadFile", QString(), localPath)) {
            return;
        }

        DriveFile file;
        file.id = nextId();
        file.name = fileName.isEmpty() ? QFileInfo(localPath).fileName() : fileName;
        file.modifiedTime = QDateTime::currentDateTimeUtc();
        m_lastUploadedFileId = file.id;
        emit fileUploadedDetailed(file, localPath);
        emit fileUploaded(file);
    }

    void updateFile(const QString& fileId, const QString& localPath) override {
        DriveFile file;
        file.id = fileId;
        file.name = QFileInfo(localPath).fileName();
        file.modifiedTime = QDateTime::currentDateTimeUtc();
        emit fileUpdated(file);
    }

    void moveFile(const QString& fileId, const QString& newParentId,
                  const QString& oldParentId) override {
        m_lastMoveCall = {fileId, newParentId, oldParentId};
        emit fileMoved(fileId);
        DriveFile file;
        file.id = fileId;
        file.modifiedTime = QDateTime::currentDateTimeUtc();
        emit fileMovedDetailed(file);
    }

    void renameFile(const QString& fileId, const QString& newName) override {
        m_lastRenameCall = {fileId, newName};
        emit fileRenamed(fileId);
        DriveFile file;
        file.id = fileId;
        file.name = newName;
        file.modifiedTime = QDateTime::currentDateTimeUtc();
        emit fileRenamedDetailed(file);
    }

    void deleteFile(const QString& fileId) override {
        m_lastDeleteCall = {fileId};
        emit fileDeleted(fileId);
    }

    void createFolder(const QString& name, const QString& parentId,
                      const QString& localPath) override {
        ++m_folderCallCount;
        m_lastFolderCall = {name, parentId, localPath};
        if (emitInjectedError("createFolder", QString(), localPath)) {
            return;
        }

        DriveFile folder;
        folder.id = nextId();
        folder.name = name;
        folder.isFolder = true;
        folder.modifiedTime = QDateTime::currentDateTimeUtc();
        setFolderIdForPath(localPath, folder.id);
        emit folderCreatedDetailed(folder, localPath);
        emit folderCreated(folder);
    }

    QJsonArray getParentsByFileId(const QString& fileId) override {
        QJsonArray parents;
        QString parentId = m_parentByFileId.value(fileId);
        if (!parentId.isEmpty()) {
            parents.append(parentId);
        }
        return parents;
    }

    QString getFolderIdByPath(const QString& folderPath) override {
        QString normalized = QDir::cleanPath(folderPath);
        if (normalized == ".") {
            normalized.clear();
        }
        return m_folderIdByPath.value(normalized);
    }

   private:
    QString nextId() { return QString("fake-%1").arg(++m_nextId); }

    bool emitInjectedError(const QString& operation, const QString& fileId,
                           const QString& localPath) {
        if (!m_injectedErrors.contains(operation)) {
            return false;
        }

        InjectedError injected = m_injectedErrors.value(operation);
        emit errorDetailed(operation, injected.errorMsg, injected.httpStatus, fileId, localPath);

        injected.remaining -= 1;
        if (injected.remaining <= 0) {
            m_injectedErrors.remove(operation);
        } else {
            m_injectedErrors.insert(operation, injected);
        }

        return true;
    }

    int m_nextId = 0;
    int m_uploadCallCount = 0;
    int m_folderCallCount = 0;
    QString m_lastUploadedFileId;
    UploadCall m_lastUploadCall;
    MoveCall m_lastMoveCall;
    RenameCall m_lastRenameCall;
    DeleteCall m_lastDeleteCall;
    DownloadCall m_lastDownloadCall;
    FolderCall m_lastFolderCall;
    QHash<QString, QString> m_folderIdByPath;
    QHash<QString, QString> m_folderPathById;
    QHash<QString, QString> m_parentByFileId;
    QHash<QString, InjectedError> m_injectedErrors;
};

class TestSyncActionThread : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testWakeOnItemsAvailable();
    void testUploadFile();
    void testUploadFolder();
    void testUploadFolder_SkipsCreateWhenDbMappingExists();
    void testUploadFolder_DoesNotAdoptPathMatchWithoutDbMapping();
    void testUploadFolder_RecoversFromStaleParentId();
    void testUploadFile_DeferredParentDeduplicatesPendingParentCreate();
    void testUploadFile_RetriesAfterAuthFailure();
    void testUploadFile_RetriesTransientFailure();
    void testUploadFile_StopsAfterRetryBudget();
    void testDownloadFile();
    void testDownloadFolder();
    void testDeleteLocal();
    void testDeleteLocalFolderMarksDescendants();
    void testMoveLocal();
    void testMoveLocal_UpdatesMetadataOnDestination();
    void testMoveLocal_UsesActionFileIdWhenSourceMappingMissing();
    void testRenameLocal();
    void testRenameLocal_UsesActionFileIdWhenSourceMappingMissing();
    void testDeleteRemoteById();
    void testDeleteRemoteFromDb();
    void testDeleteRemoteFolderMarksDescendants();
    void testMoveRemote();
    void testMoveRemoteToRootUpdatesDbPath();
    void testMoveRemoteToRootKeepsExactPathWhenLocalExists();
    void testMoveRemoteDefersMissingParent();
    void testRenameRemote();
    void testBadInputFuzzing();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    SyncActionQueue* m_queue = nullptr;
    SyncActionThread* m_thread = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeGoogleDriveClient* m_drive = nullptr;
    QByteArray m_originalHome;

    QString createFile(const QString& relPath, const QByteArray& data = "data");
    void enqueueAndWait(const SyncActionItem& action, int expectedCompleted = 1,
                        int expectedFailed = 0);
};

void TestSyncActionThread::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    m_originalHome = qgetenv("HOME");
    qputenv("HOME", m_tempDir->path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());

    m_queue = new SyncActionQueue();
    m_drive = new FakeGoogleDriveClient();
    m_thread = new SyncActionThread(m_queue, m_db, m_drive, nullptr);
    m_thread->setSyncFolder(m_tempDir->path());
}

void TestSyncActionThread::cleanup() {
    if (m_thread) {
        m_thread->stop();
    }

    delete m_thread;
    m_thread = nullptr;

    delete m_drive;
    m_drive = nullptr;

    delete m_queue;
    m_queue = nullptr;

    if (m_db) {
        m_db->close();
        delete m_db;
        m_db = nullptr;
    }

    QStandardPaths::setTestModeEnabled(false);
    qputenv("HOME", m_originalHome);

    delete m_tempDir;
    m_tempDir = nullptr;
}

QString TestSyncActionThread::createFile(const QString& relPath, const QByteArray& data) {
    QString absPath = m_tempDir->filePath(relPath);
    QDir dir(QFileInfo(absPath).dir());
    dir.mkpath(".");
    QFile file(absPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(data);
        file.close();
    }
    return absPath;
}

void TestSyncActionThread::enqueueAndWait(const SyncActionItem& action, int expectedCompleted,
                                          int expectedFailed) {
    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);

    m_queue->enqueue(action);

    QTRY_COMPARE(completedSpy.count(), expectedCompleted);
    QCOMPARE(failedSpy.count(), expectedFailed);
}

void TestSyncActionThread::testWakeOnItemsAvailable() {
    QString relPath = "wake.txt";
    QString absPath = m_tempDir->filePath(relPath);

    QFile file(absPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("data") > 0);
    file.close();

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteLocal;
    action.localPath = relPath;

    m_queue->enqueue(action);

    QTRY_COMPARE(completedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!QFile::exists(absPath));
}

void TestSyncActionThread::testUploadFile() {
    QString relPath = "upload.txt";
    createFile(relPath, "upload");

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = relPath;
    action.isFolder = false;

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastUploadCall().localPath, m_tempDir->filePath(relPath));
    QVERIFY(!m_drive->lastUploadedFileId().isEmpty());
    QCOMPARE(m_db->getFileId(relPath), m_drive->lastUploadedFileId());
}

void TestSyncActionThread::testUploadFolder() {
    QString relPath = "folderA";
    QDir(m_tempDir->path()).mkpath(relPath);

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = relPath;
    action.isFolder = true;

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastFolderCall().localPath, relPath);
    QVERIFY(!m_db->getFileId(relPath).isEmpty());
}

void TestSyncActionThread::testUploadFolder_SkipsCreateWhenDbMappingExists() {
    QString relPath = "mappedFolder";
    QDir(m_tempDir->path()).mkpath(relPath);
    m_db->setFileId(relPath, "mapped-folder-id");

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = relPath;
    action.isFolder = true;

    enqueueAndWait(action);

    QCOMPARE(m_drive->folderCallCount(), 0);
    QCOMPARE(m_db->getFileId(relPath), QString("mapped-folder-id"));
}

void TestSyncActionThread::testUploadFolder_DoesNotAdoptPathMatchWithoutDbMapping() {
    QString relPath = "nonCanonicalFolder";
    QDir(m_tempDir->path()).mkpath(relPath);
    m_drive->setFolderIdForPath(relPath, "preexisting-remote-id");

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = relPath;
    action.isFolder = true;

    enqueueAndWait(action);

    QCOMPARE(m_drive->folderCallCount(), 1);
    const QString mappedId = m_db->getFileId(relPath);
    QVERIFY(!mappedId.isEmpty());
    QVERIFY(mappedId != QString("preexisting-remote-id"));
}

void TestSyncActionThread::testUploadFolder_RecoversFromStaleParentId() {
    QDir(m_tempDir->path()).mkpath("parent/newFolder");

    m_db->setFileId("parent", "stale-parent-id");
    m_drive->setFolderIdForPath("parent", "fresh-parent-id");
    m_drive->injectOperationError("createFolder", "File not found: stale-parent-id", 404, 1);

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = "parent/newFolder";
    action.isFolder = true;

    enqueueAndWait(action);

    QCOMPARE(m_drive->folderCallCount(), 2);
    QCOMPARE(m_drive->lastFolderCall().parentId, QString("fresh-parent-id"));
    QCOMPARE(m_db->getFileId("parent"), QString("fresh-parent-id"));
    QVERIFY(!m_db->getFileId("parent/newFolder").isEmpty());
}

void TestSyncActionThread::testUploadFile_DeferredParentDeduplicatesPendingParentCreate() {
    createFile("parent/child.txt", "upload");

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);

    SyncActionItem childAction;
    childAction.actionType = SyncActionType::Upload;
    childAction.localPath = "parent/child.txt";
    childAction.isFolder = false;

    SyncActionItem parentAction;
    parentAction.actionType = SyncActionType::Upload;
    parentAction.localPath = "parent";
    parentAction.isFolder = true;

    m_queue->enqueue(childAction);
    m_queue->enqueue(parentAction);

    QTRY_COMPARE(completedSpy.count(), 2);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(m_drive->folderCallCount(), 1);
    QCOMPARE(m_drive->uploadCallCount(), 1);
}

void TestSyncActionThread::testUploadFile_RetriesAfterAuthFailure() {
    QString relPath = "upload/auth_retry.txt";
    createFile(relPath, "upload");
    m_db->setFileId("upload", "upload-parent-id");

    m_drive->injectOperationError("uploadFile", "Unauthorized", 401, 1);

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);
    QSignalSpy refreshSpy(m_thread, &SyncActionThread::tokenRefreshRequested);

    m_thread->start();
    QTest::qWait(10);

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = relPath;
    action.isFolder = false;

    m_queue->enqueue(action);

    QTRY_COMPARE(completedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(refreshSpy.count() >= 1);
    QCOMPARE(m_drive->uploadCallCount(), 2);
}

void TestSyncActionThread::testUploadFile_RetriesTransientFailure() {
    QString relPath = "upload/transient_retry.txt";
    createFile(relPath, "upload");
    m_db->setFileId("upload", "upload-parent-id");

    m_drive->injectOperationError("uploadFile", "HTTP2 GOAWAY stream error", 0, 1);

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = relPath;
    action.isFolder = false;

    m_queue->enqueue(action);

    QTRY_COMPARE(completedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(m_drive->uploadCallCount(), 2);
}

void TestSyncActionThread::testUploadFile_StopsAfterRetryBudget() {
    QString relPath = "upload/transient_retry_budget.txt";
    createFile(relPath, "upload");
    m_db->setFileId("upload", "upload-parent-id");

    m_drive->injectOperationError("uploadFile", "HTTP2 GOAWAY stream error", 0, 10);

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = relPath;
    action.isFolder = false;

    m_queue->enqueue(action);

    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 0);
    QCOMPARE(m_drive->uploadCallCount(), 4);
}

void TestSyncActionThread::testDownloadFile() {
    QString relPath = "downloads/file.txt";
    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = relPath;
    action.fileId = "remote-file-1";

    enqueueAndWait(action);
    QVERIFY(QFile::exists(m_tempDir->filePath(relPath)));
}

void TestSyncActionThread::testDownloadFolder() {
    QString relPath = "downloads/folder";
    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = relPath;
    action.fileId = "remote-folder-1";
    action.isFolder = true;

    enqueueAndWait(action);
    QVERIFY(QDir(m_tempDir->filePath(relPath)).exists());
}

void TestSyncActionThread::testDeleteLocal() {
    QString relPath = "delete/local.txt";
    createFile(relPath, "delete");
    m_db->setFileId(relPath, "local-id-1");

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteLocal;
    action.localPath = relPath;

    enqueueAndWait(action);
    QVERIFY(!QFile::exists(m_tempDir->filePath(relPath)));
}

void TestSyncActionThread::testDeleteLocalFolderMarksDescendants() {
    createFile("parent/sub/file.txt", "delete");
    m_db->setFileId("parent", "parent-id");
    m_db->setFileId("parent/sub", "sub-id");
    m_db->setFileId("parent/sub/file.txt", "file-id");

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteLocal;
    action.localPath = "parent";

    enqueueAndWait(action);

    QVERIFY(m_db->wasFileDeleted("parent"));
    QVERIFY(m_db->wasFileDeleted("parent/sub"));
    QVERIFY(m_db->wasFileDeleted("parent/sub/file.txt"));
}

void TestSyncActionThread::testMoveLocal() {
    QString relPath = "move/source.txt";
    createFile(relPath, "move");
    m_db->setFileId(relPath, "local-id-2");

    SyncActionItem action;
    action.actionType = SyncActionType::MoveLocal;
    action.localPath = relPath;
    action.moveDestination = "move/dest.txt";

    enqueueAndWait(action);
    QVERIFY(QFile::exists(m_tempDir->filePath("move/dest.txt")));
    QCOMPARE(m_db->getLocalPath("local-id-2"), QString("move/dest.txt"));
}

void TestSyncActionThread::testMoveLocal_UpdatesMetadataOnDestination() {
    QString relPath = "move/meta-source.txt";
    createFile(relPath, "move");
    m_db->setFileId(relPath, "local-id-meta");

    SyncActionItem action;
    action.actionType = SyncActionType::MoveLocal;
    action.localPath = relPath;
    action.fileId = "local-id-meta";
    action.moveDestination = "move/meta-dest.txt";
    action.modifiedTime = QDateTime::currentDateTimeUtc().addSecs(-120);

    enqueueAndWait(action);

    QFileInfo movedInfo(m_tempDir->filePath("move/meta-dest.txt"));
    QVERIFY(movedInfo.exists());

    const qint64 actual = movedInfo.lastModified().toUTC().toSecsSinceEpoch();
    const qint64 expected = action.modifiedTime.toUTC().toSecsSinceEpoch();
    QVERIFY(qAbs(actual - expected) <= 2);
}

void TestSyncActionThread::testMoveLocal_UsesActionFileIdWhenSourceMappingMissing() {
    QString relPath = "move/stale-source.txt";
    createFile(relPath, "move");

    m_db->setFileId("other/source.txt", "stale-id-2");

    SyncActionItem action;
    action.actionType = SyncActionType::MoveLocal;
    action.localPath = relPath;
    action.fileId = "stale-id-2";
    action.moveDestination = "move/new-location.txt";

    enqueueAndWait(action);
    QVERIFY(QFile::exists(m_tempDir->filePath("move/new-location.txt")));
    QCOMPARE(m_db->getLocalPath("stale-id-2"), QString("move/new-location.txt"));
}

void TestSyncActionThread::testRenameLocal() {
    QString relPath = "rename/source.txt";
    createFile(relPath, "rename");
    m_db->setFileId(relPath, "local-id-3");

    SyncActionItem action;
    action.actionType = SyncActionType::RenameLocal;
    action.localPath = relPath;
    action.renameTo = "renamed.txt";

    enqueueAndWait(action);
    QVERIFY(QFile::exists(m_tempDir->filePath("rename/renamed.txt")));
    QCOMPARE(m_db->getLocalPath("local-id-3"), QString("rename/renamed.txt"));
}

void TestSyncActionThread::testRenameLocal_UsesActionFileIdWhenSourceMappingMissing() {
    QString relPath = "rename/stale-source.txt";
    createFile(relPath, "rename");

    m_db->setFileId("rename/other-source.txt", "stale-id-3");

    SyncActionItem action;
    action.actionType = SyncActionType::RenameLocal;
    action.localPath = relPath;
    action.fileId = "stale-id-3";
    action.renameTo = "stale-renamed.txt";

    enqueueAndWait(action);
    QVERIFY(QFile::exists(m_tempDir->filePath("rename/stale-renamed.txt")));
    QCOMPARE(m_db->getLocalPath("stale-id-3"), QString("rename/stale-renamed.txt"));
}

void TestSyncActionThread::testDeleteRemoteById() {
    SyncActionItem action;
    action.actionType = SyncActionType::DeleteRemote;
    action.fileId = "remote-id-1";

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastDeleteCall().fileId, QString("remote-id-1"));
}

void TestSyncActionThread::testDeleteRemoteFromDb() {
    QString relPath = "remote/delete.txt";
    m_db->setFileId(relPath, "remote-id-2");

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteRemote;
    action.localPath = relPath;

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastDeleteCall().fileId, QString("remote-id-2"));
}

void TestSyncActionThread::testDeleteRemoteFolderMarksDescendants() {
    FileSyncState folder;
    folder.localPath = "folder";
    folder.fileId = "folder-id";
    folder.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    folder.isFolder = true;

    FileSyncState childFile;
    childFile.localPath = "folder/child.txt";
    childFile.fileId = "child-id";
    childFile.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    childFile.isFolder = false;

    FileSyncState childFolder;
    childFolder.localPath = "folder/sub";
    childFolder.fileId = "sub-id";
    childFolder.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    childFolder.isFolder = true;

    FileSyncState grandChild;
    grandChild.localPath = "folder/sub/grand.txt";
    grandChild.fileId = "grand-id";
    grandChild.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    grandChild.isFolder = false;

    m_db->saveFileState(folder);
    m_db->saveFileState(childFile);
    m_db->saveFileState(childFolder);
    m_db->saveFileState(grandChild);

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteRemote;
    action.fileId = "folder-id";
    action.localPath = "folder";

    enqueueAndWait(action);

    QVERIFY(m_db->wasFileDeleted("folder"));
    QVERIFY(m_db->wasFileDeleted("folder/child.txt"));
    QVERIFY(m_db->wasFileDeleted("folder/sub"));
    QVERIFY(m_db->wasFileDeleted("folder/sub/grand.txt"));
}

void TestSyncActionThread::testMoveRemote() {
    QString relPath = "remote/move.txt";
    m_db->setFileId(relPath, "remote-id-3");
    m_db->setFileId("dest", "parent-id-1");
    m_drive->setParentForFileId("remote-id-3", "old-parent");

    SyncActionItem action;
    action.actionType = SyncActionType::MoveRemote;
    action.localPath = relPath;
    action.fileId = "remote-id-3";
    action.moveDestination = "dest";

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastMoveCall().newParentId, QString("parent-id-1"));
    QCOMPARE(m_drive->lastMoveCall().oldParentId, QString("old-parent"));
    QCOMPARE(m_db->getLocalPath("remote-id-3"), QString("dest/move.txt"));
}

void TestSyncActionThread::testMoveRemoteToRootUpdatesDbPath() {
    QString relPath = "mario/wario.png";
    m_db->setFileId(relPath, "remote-id-root-1");
    m_drive->setParentForFileId("remote-id-root-1", "parent-id-mario");

    SyncActionItem action;
    action.actionType = SyncActionType::MoveRemote;
    action.localPath = relPath;
    action.fileId = "remote-id-root-1";
    action.moveDestination = QString();

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastMoveCall().newParentId, QString("root"));
    QCOMPARE(m_drive->lastMoveCall().oldParentId, QString("parent-id-mario"));
    QCOMPARE(m_db->getLocalPath("remote-id-root-1"), QString("wario.png"));
}

void TestSyncActionThread::testMoveRemoteToRootKeepsExactPathWhenLocalExists() {
    createFile("wario.png", "already-local");

    QString relPath = "mario/wario.png";
    m_db->setFileId(relPath, "remote-id-root-2");
    m_drive->setParentForFileId("remote-id-root-2", "parent-id-mario");

    SyncActionItem action;
    action.actionType = SyncActionType::MoveRemote;
    action.localPath = relPath;
    action.fileId = "remote-id-root-2";
    action.moveDestination = QString();

    enqueueAndWait(action);
    QCOMPARE(m_db->getLocalPath("remote-id-root-2"), QString("wario.png"));
}

void TestSyncActionThread::testMoveRemoteDefersMissingParent() {
    QString relPath = "remote/defer.txt";
    QString parentPath = "parent";
    QDir(m_tempDir->path()).mkpath(parentPath);
    m_db->setFileId(relPath, "remote-id-4");
    m_drive->setParentForFileId("remote-id-4", "old-parent");

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);

    SyncActionItem action;
    action.actionType = SyncActionType::MoveRemote;
    action.localPath = relPath;
    action.fileId = "remote-id-4";
    action.moveDestination = parentPath;

    m_queue->enqueue(action);

    QTRY_COMPARE(completedSpy.count(), 2);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!m_db->getFileId(parentPath).isEmpty());
    QCOMPARE(m_db->getLocalPath("remote-id-4"), QString("parent/defer.txt"));
}

void TestSyncActionThread::testRenameRemote() {
    QString relPath = "remote/rename.txt";
    m_db->setFileId(relPath, "remote-id-5");

    SyncActionItem action;
    action.actionType = SyncActionType::RenameRemote;
    action.localPath = relPath;
    action.fileId = "remote-id-5";
    action.renameTo = "renamed.txt";

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastRenameCall().newName, QString("renamed.txt"));
    QCOMPARE(m_db->getLocalPath("remote-id-5"), QString("remote/renamed.txt"));
}

void TestSyncActionThread::testBadInputFuzzing() {
    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);

    QList<SyncActionItem> badActions;
    SyncActionItem noPath;
    noPath.actionType = SyncActionType::Upload;
    badActions.append(noPath);

    SyncActionItem noId;
    noId.actionType = SyncActionType::DeleteRemote;
    badActions.append(noId);

    SyncActionItem noLocalPathMove;
    noLocalPathMove.actionType = SyncActionType::MoveLocal;
    badActions.append(noLocalPathMove);

    SyncActionItem noLocalPathRename;
    noLocalPathRename.actionType = SyncActionType::RenameLocal;
    badActions.append(noLocalPathRename);

    SyncActionItem noRemoteIdentifiers;
    noRemoteIdentifiers.actionType = SyncActionType::MoveRemote;
    badActions.append(noRemoteIdentifiers);

    for (const SyncActionItem& item : badActions) {
        m_queue->enqueue(item);
    }

    QTRY_VERIFY(m_queue->isEmpty());
    QCOMPARE(completedSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 0);
}

QTEST_MAIN(TestSyncActionThread)
#include "TestSyncActionThread.moc"
