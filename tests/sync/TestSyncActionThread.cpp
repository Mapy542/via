/**
 * @file TestSyncActionThread.cpp
 * @brief Unit tests for SyncActionThread wake and execution behavior
 */

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncActionThread.h"
#include "sync/SyncDatabase.h"
#include "utils/NativeDocShortcutHandler.h"
#include "utils/NativeDocSupport.h"

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

    struct UpdateCall {
        QString fileId;
        QString localPath;
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

    struct TrashCall {
        QString fileId;
    };

    struct DownloadCall {
        QString fileId;
        QString localPath;
    };

    struct ExportCall {
        QString fileId;
        QString exportMimeType;
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

    void emitDetailedError(const QString& operation, const QString& errorMsg, int httpStatus,
                           const QString& fileId = QString(),
                           const QString& localPath = QString()) {
        emit errorDetailed(operation, errorMsg, httpStatus, fileId, localPath);
    }

    QString lastUploadedFileId() const { return m_lastUploadedFileId; }
    UploadCall lastUploadCall() const { return m_lastUploadCall; }
    UpdateCall lastUpdateCall() const { return m_lastUpdateCall; }
    MoveCall lastMoveCall() const { return m_lastMoveCall; }
    RenameCall lastRenameCall() const { return m_lastRenameCall; }
    DeleteCall lastDeleteCall() const { return m_lastDeleteCall; }
    TrashCall lastTrashCall() const { return m_lastTrashCall; }
    DownloadCall lastDownloadCall() const { return m_lastDownloadCall; }
    ExportCall lastExportCall() const { return m_lastExportCall; }
    FolderCall lastFolderCall() const { return m_lastFolderCall; }
    int uploadCallCount() const { return m_uploadCallCount; }
    int updateCallCount() const { return m_updateCallCount; }
    int exportCallCount() const { return m_exportCallCount; }
    int folderCallCount() const { return m_folderCallCount; }

    void setFileMetadata(const DriveFile& file) { m_metadataByFileId.insert(file.id, file); }

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

    void exportFile(const QString& fileId, const QString& exportMimeType,
                    const QString& localPath) override {
        ++m_exportCallCount;
        m_lastExportCall = {fileId, exportMimeType, localPath};

        const QString exactOperation = QStringLiteral("exportFile:%1").arg(fileId);
        if (emitInjectedError(exactOperation, fileId, localPath) ||
            emitInjectedError(QStringLiteral("exportFile"), fileId, localPath)) {
            return;
        }

        QFileInfo info(localPath);
        QDir dir(info.dir());
        dir.mkpath(".");
        QFile file(localPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("export:");
            file.write(exportMimeType.toUtf8());
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
        ++m_updateCallCount;
        m_lastUpdateCall = {fileId, localPath};
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

    void trashFile(const QString& fileId) override {
        m_lastTrashCall = {fileId};
        emit fileTrashed(fileId);
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

    DriveFile getFileMetadataBlocking(const QString& fileId) override {
        return m_metadataByFileId.value(fileId);
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
    int m_updateCallCount = 0;
    int m_exportCallCount = 0;
    int m_folderCallCount = 0;
    QString m_lastUploadedFileId;
    UploadCall m_lastUploadCall;
    UpdateCall m_lastUpdateCall;
    MoveCall m_lastMoveCall;
    RenameCall m_lastRenameCall;
    DeleteCall m_lastDeleteCall;
    TrashCall m_lastTrashCall;
    DownloadCall m_lastDownloadCall;
    ExportCall m_lastExportCall;
    FolderCall m_lastFolderCall;
    QHash<QString, QString> m_folderIdByPath;
    QHash<QString, QString> m_folderPathById;
    QHash<QString, QString> m_parentByFileId;
    QHash<QString, DriveFile> m_metadataByFileId;
    QHash<QString, InjectedError> m_injectedErrors;
};

class TestSyncActionThread : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testWakeOnItemsAvailable();
    void testPendingWorkDuringIdleDisarmStaysArmed();
    void testUploadFile();
    void testUploadFolder();
    void testUploadFolder_SkipsCreateWhenDbMappingExists();
    void testUploadFolder_DoesNotAdoptPathMatchWithoutDbMapping();
    void testUploadFolder_RecoversFromStaleParentId();
    void testUploadFile_DeferredParentDeduplicatesPendingParentCreate();
    void testUploadFile_RetriesAfterAuthFailure();
    void testUnmatchedWatcherFailureDoesNotEmitUserError();
    void testUnmatchedAuthFailureDoesNotEmitUserError();
    void testWatcherLegacyErrorDoesNotFailUnrelatedAction();
    void testUploadFile_RetriesTransientFailure();
    void testUploadFile_StopsAfterRetryBudget();
    void testUploadNativeDocGuard_RejectsMappedNativeDoc();
    void testDownloadFile();
    void testDownloadFile_DisambiguatesDuplicateWithFileIdSuffix();
    void testDownloadFile_DisambiguatesDuplicateWithNumericSuffix();
    void testDownloadNativeDoc_BrowserShortcutMaterializesReadOnlyShortcut();
    void testDownloadNativeDoc_OpenDocumentUsesExportMimeAndReadOnlyPermissions();
    void testDownloadNativeDoc_FetchesMissingMetadataForShortcutMaterialization();
    void testDownloadNativeDoc_ExportLimitFallsBackToBrowserShortcut();
    void testDownloadNativeDoc_ExportLimitFallbackWriteFailurePreservesState();
    void testDownloadNativeDoc_NonLimitExportFailureStaysError();
    void testDownloadFolder();
    void testDeleteLocal();
    void testDeleteLocalFolderMarksDescendants();
    void testDeleteLocalSymlinkFileRemovesLinkOnly();
    void testDeleteLocalSymlinkDirectoryRemovesLinkOnly();
    void testMoveLocal();
    void testMoveLocal_DisambiguatesWhenDestinationExists();
    void testMoveLocal_UpdatesMetadataOnDestination();
    void testMoveLocal_UsesActionFileIdWhenSourceMappingMissing();
    void testRenameLocal();
    void testRenameLocal_DisambiguatesWhenDestinationExists();
    void testRenameLocal_UsesActionFileIdWhenSourceMappingMissing();
    void testDeleteRemoteById();
    void testDeleteRemoteFromDb();
    void testDeleteRemoteFolderMarksDescendants();
    void testTrashRemoteById();
    void testTrashRemoteFromDb();
    void testTrashRemoteFolderMarksDescendants();
    void testMoveRemote();
    void testMoveRemoteToRootUpdatesDbPath();
    void testMoveRemoteToRootKeepsExactPathWhenLocalExists();
    void testMoveRemoteDefersMissingParent();
    void testRenameRemote();
    void testRenameRemote_NativeDocStripsVisibleSuffix();
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

    QSettings settings;
    settings.setValue("sync/duplicateNameStrategy", "file-id-suffix");
    settings.sync();

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

void TestSyncActionThread::testPendingWorkDuringIdleDisarmStaysArmed() {
    QString relPath = "idle-disarm.txt";
    QString absPath = createFile(relPath, "data");

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteLocal;
    action.localPath = relPath;

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->m_beforeIdleDisarmHook = [this, action]() { m_queue->enqueue(action); };

    m_thread->start();

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

void TestSyncActionThread::testUnmatchedWatcherFailureDoesNotEmitUserError() {
    QSignalSpy errorSpy(m_thread, &SyncActionThread::error);

    m_thread->onDriveErrorDetailed("listChanges", "HTTP/2 GOAWAY stream error", 0, QString(),
                                   QString());

    QCoreApplication::processEvents();

    QCOMPARE(errorSpy.count(), 0);
}

void TestSyncActionThread::testUnmatchedAuthFailureDoesNotEmitUserError() {
    QSignalSpy errorSpy(m_thread, &SyncActionThread::error);
    QSignalSpy refreshSpy(m_thread, &SyncActionThread::tokenRefreshRequested);

    m_drive->emitDetailedError(
        "listChanges",
        "Request had invalid authentication credentials. Expected OAuth 2 access token.", 401);

    QCoreApplication::processEvents();

    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(refreshSpy.count(), 0);
}

void TestSyncActionThread::testWatcherLegacyErrorDoesNotFailUnrelatedAction() {
    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = "upload/inflight.txt";

    const QString actionKey = m_thread->actionKeyForLocalPath(action.localPath);
    QVERIFY(!actionKey.isEmpty());
    m_thread->m_driveActionsInProgress.insert(actionKey, action);

    QSignalSpy errorSpy(m_thread, &SyncActionThread::error);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->onDriveError("listChanges", "HTTP/2 stream 1 was not closed cleanly");

    QCoreApplication::processEvents();

    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(m_thread->m_driveActionsInProgress.contains(actionKey));
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

void TestSyncActionThread::testUploadNativeDocGuard_RejectsMappedNativeDoc() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "browser-shortcut");
    settings.sync();

    NativeDocState state;
    state.fileId = "native-doc-upload";
    state.remoteName = "Quarterly";
    state.remoteMimeType = "application/vnd.google-apps.document";
    state.webViewLink = "https://docs.google.com/document/d/native-doc-upload/edit";
    QVERIFY(m_db->saveNativeDocState(state));

    createFile("upload/Quarterly.gdoc", "edited-local-content");

    SyncActionItem action;
    action.actionType = SyncActionType::Upload;
    action.localPath = "upload/Quarterly.gdoc";
    action.fileId = state.fileId;

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);
    m_queue->enqueue(action);

    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 0);

    QCOMPARE(m_drive->uploadCallCount(), 0);
    QCOMPARE(m_drive->updateCallCount(), 0);
}

void TestSyncActionThread::testDownloadFile_DisambiguatesDuplicateWithFileIdSuffix() {
    createFile("downloads/file.txt", "existing");

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/file.txt";
    action.fileId = "remote-file-dup";

    enqueueAndWait(action);

    QCOMPARE(m_db->getLocalPath("remote-file-dup"), QString("downloads/file_remote-file-dup.txt"));
    QVERIFY(QFile::exists(m_tempDir->filePath("downloads/file_remote-file-dup.txt")));
}

void TestSyncActionThread::testDownloadFile_DisambiguatesDuplicateWithNumericSuffix() {
    QSettings settings;
    settings.setValue("sync/duplicateNameStrategy", "numeric-suffix");
    settings.sync();

    createFile("downloads/file.txt", "existing");

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/file.txt";
    action.fileId = "remote-file-num";

    enqueueAndWait(action);

    QCOMPARE(m_db->getLocalPath("remote-file-num"), QString("downloads/file (1).txt"));
    QVERIFY(QFile::exists(m_tempDir->filePath("downloads/file (1).txt")));
}

void TestSyncActionThread::testDownloadNativeDoc_BrowserShortcutMaterializesReadOnlyShortcut() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "browser-shortcut");
    settings.sync();

    NativeDocState state;
    state.fileId = "native-doc-shortcut";
    state.remoteName = "Quarterly";
    state.remoteMimeType = "application/vnd.google-apps.document";
    state.webViewLink = "https://docs.google.com/document/d/native-doc-shortcut/edit";
    QVERIFY(m_db->saveNativeDocState(state));

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/Quarterly.gdoc";
    action.fileId = state.fileId;
    action.modifiedTime = QDateTime::currentDateTimeUtc().addSecs(-90);

    enqueueAndWait(action);

    QCOMPARE(m_drive->exportCallCount(), 0);
    QVERIFY(m_drive->lastDownloadCall().fileId.isEmpty());

    const QString absolutePath = m_tempDir->filePath(action.localPath);
    QFile shortcutFile(absolutePath);
    QVERIFY(shortcutFile.open(QIODevice::ReadOnly));
    QCOMPARE(shortcutFile.readAll(), nativeDocShortcutPayload(state));
    shortcutFile.close();

    const QFileInfo shortcutInfo(absolutePath);
    QVERIFY(!(shortcutInfo.permissions() & (QFileDevice::WriteOwner | QFileDevice::WriteUser |
                                            QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    const qint64 actualMtime = shortcutInfo.lastModified().toUTC().toSecsSinceEpoch();
    const qint64 expectedMtime = action.modifiedTime.toUTC().toSecsSinceEpoch();
    QVERIFY(qAbs(actualMtime - expectedMtime) <= 2);

    QCOMPARE(m_db->getLocalPath(state.fileId), action.localPath);
}

void TestSyncActionThread::
    testDownloadNativeDoc_OpenDocumentUsesExportMimeAndReadOnlyPermissions() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "open-document");
    settings.sync();

    NativeDocState state;
    state.fileId = "native-doc-export";
    state.remoteName = "Quarterly";
    state.remoteMimeType = "application/vnd.google-apps.document";
    state.webViewLink = "https://docs.google.com/document/d/native-doc-export/edit";
    QVERIFY(m_db->saveNativeDocState(state));

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/Quarterly.odt";
    action.fileId = state.fileId;
    action.modifiedTime = QDateTime::currentDateTimeUtc().addSecs(-90);

    enqueueAndWait(action);

    QCOMPARE(m_drive->exportCallCount(), 1);
    QCOMPARE(m_drive->lastExportCall().fileId, state.fileId);
    QCOMPARE(m_drive->lastExportCall().exportMimeType,
             QString("application/vnd.oasis.opendocument.text"));
    QVERIFY(m_drive->lastDownloadCall().fileId.isEmpty());

    const QString absolutePath = m_tempDir->filePath(action.localPath);
    QFile exportedFile(absolutePath);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    QCOMPARE(exportedFile.readAll(), QByteArray("export:application/vnd.oasis.opendocument.text"));
    exportedFile.close();

    const QFileInfo exportedInfo(absolutePath);
    QVERIFY(!(exportedInfo.permissions() & (QFileDevice::WriteOwner | QFileDevice::WriteUser |
                                            QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    const qint64 actualMtime = exportedInfo.lastModified().toUTC().toSecsSinceEpoch();
    const qint64 expectedMtime = action.modifiedTime.toUTC().toSecsSinceEpoch();
    QVERIFY(qAbs(actualMtime - expectedMtime) <= 2);
}

void TestSyncActionThread::
    testDownloadNativeDoc_FetchesMissingMetadataForShortcutMaterialization() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "browser-shortcut");
    settings.sync();

    DriveFile file;
    file.id = "native-doc-fetched";
    file.name = "Fetched Doc";
    file.mimeType = "application/vnd.google-apps.document";
    file.webViewLink = "https://docs.google.com/document/d/native-doc-fetched/edit";
    m_drive->setFileMetadata(file);

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/Fetched Doc.gdoc";
    action.fileId = file.id;

    enqueueAndWait(action);

    const NativeDocState stored = m_db->getNativeDocState(file.id);
    QVERIFY(stored.isValid());
    QCOMPARE(stored.remoteName, file.name);
    QCOMPARE(stored.remoteMimeType, file.mimeType);
    QCOMPARE(stored.webViewLink, file.webViewLink);

    const QString absolutePath = m_tempDir->filePath(action.localPath);
    const auto parsedShortcut = parseNativeDocShortcutFile(absolutePath);
    QVERIFY(parsedShortcut.has_value());
    QCOMPARE(parsedShortcut->url.toString(), file.webViewLink);
    QCOMPARE(parsedShortcut->remoteMimeType, file.mimeType);
}

void TestSyncActionThread::testDownloadNativeDoc_ExportLimitFallsBackToBrowserShortcut() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "open-document");
    settings.sync();

    NativeDocState state;
    state.fileId = "native-doc-export-limit";
    state.remoteName = "Quarterly";
    state.remoteMimeType = "application/vnd.google-apps.document";
    state.webViewLink = "https://docs.google.com/document/d/native-doc-export-limit/edit";
    QVERIFY(m_db->saveNativeDocState(state));
    m_db->setLocalPath(state.fileId, "downloads/Quarterly.odt");
    createFile("downloads/Quarterly.odt", "stale-export");

    m_drive->injectOperationError(QStringLiteral("exportFile:%1").arg(state.fileId),
                                  "This file is too large to be exported.", 403, 1);

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/Quarterly.odt";
    action.fileId = state.fileId;
    action.modifiedTime = QDateTime::currentDateTimeUtc().addSecs(-90);

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);
    QSignalSpy errorSpy(m_thread, &SyncActionThread::error);

    m_thread->start();
    QTest::qWait(10);
    m_queue->enqueue(action);

    QTRY_COMPARE(completedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(m_drive->exportCallCount(), 1);
    QCOMPARE(m_drive->lastExportCall().fileId, state.fileId);
    QCOMPARE(m_drive->lastExportCall().exportMimeType,
             QString("application/vnd.oasis.opendocument.text"));

    const NativeDocState stored = m_db->getNativeDocState(state.fileId);
    QCOMPARE(stored.nativeDocModeOverride, QString("browser-shortcut"));
    QCOMPARE(m_db->getLocalPath(state.fileId), QString("downloads/Quarterly.gdoc"));

    QVERIFY(!QFile::exists(m_tempDir->filePath("downloads/Quarterly.odt")));
    const QString shortcutPath = m_tempDir->filePath("downloads/Quarterly.gdoc");
    const auto parsedShortcut = parseNativeDocShortcutFile(shortcutPath);
    QVERIFY(parsedShortcut.has_value());
    QCOMPARE(parsedShortcut->url.toString(), state.webViewLink);
    QCOMPARE(parsedShortcut->remoteMimeType, state.remoteMimeType);
}

void TestSyncActionThread::testDownloadNativeDoc_ExportLimitFallbackWriteFailurePreservesState() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "open-document");
    settings.sync();

    struct PermissionsRestorer {
        QString path;
        QFileDevice::Permissions permissions;

        ~PermissionsRestorer() { QFile(path).setPermissions(permissions); }
    };

    NativeDocState state;
    state.fileId = "native-doc-export-limit-write-failure";
    state.remoteName = "Quarterly";
    state.remoteMimeType = "application/vnd.google-apps.document";
    state.webViewLink =
        "https://docs.google.com/document/d/native-doc-export-limit-write-failure/edit";
    QVERIFY(m_db->saveNativeDocState(state));
    m_db->setLocalPath(state.fileId, "downloads/Quarterly.odt");
    createFile("downloads/Quarterly.odt", "stale-export");

    const QString downloadsPath = m_tempDir->filePath("downloads");
    QFile downloadsDir(downloadsPath);
    const QFileDevice::Permissions originalPermissions = downloadsDir.permissions();
    PermissionsRestorer permissionsRestorer{downloadsPath, originalPermissions};
    const QFileDevice::Permissions readOnlyPermissions =
        originalPermissions & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser |
                                QFileDevice::WriteGroup | QFileDevice::WriteOther);
    QVERIFY(downloadsDir.setPermissions(readOnlyPermissions));

    m_drive->injectOperationError(QStringLiteral("exportFile:%1").arg(state.fileId),
                                  "This file is too large to be exported.", 403, 1);

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/Quarterly.odt";
    action.fileId = state.fileId;

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);

    m_thread->start();
    QTest::qWait(10);
    m_queue->enqueue(action);

    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 0);

    const NativeDocState stored = m_db->getNativeDocState(state.fileId);
    QVERIFY(stored.nativeDocModeOverride.isEmpty());
    QCOMPARE(m_db->getLocalPath(state.fileId), QString("downloads/Quarterly.odt"));
    QVERIFY(QFile::exists(m_tempDir->filePath("downloads/Quarterly.odt")));
    QVERIFY(!QFile::exists(m_tempDir->filePath("downloads/Quarterly.gdoc")));
}

void TestSyncActionThread::testDownloadNativeDoc_NonLimitExportFailureStaysError() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "open-document");
    settings.sync();

    NativeDocState state;
    state.fileId = "native-doc-export-permission";
    state.remoteName = "Quarterly";
    state.remoteMimeType = "application/vnd.google-apps.document";
    state.webViewLink = "https://docs.google.com/document/d/native-doc-export-permission/edit";
    QVERIFY(m_db->saveNativeDocState(state));

    SyncActionItem action;
    action.actionType = SyncActionType::Download;
    action.localPath = "downloads/Quarterly.odt";
    action.fileId = state.fileId;

    QSignalSpy completedSpy(m_thread, &SyncActionThread::actionCompleted);
    QSignalSpy failedSpy(m_thread, &SyncActionThread::actionFailed);
    QSignalSpy errorSpy(m_thread, &SyncActionThread::error);

    m_drive->injectOperationError(QStringLiteral("exportFile:%1").arg(state.fileId),
                                  "The user does not have sufficient permissions for this file.",
                                  403, 1);

    m_thread->start();
    QTest::qWait(10);
    m_queue->enqueue(action);

    QTRY_COMPARE(failedSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(m_drive->exportCallCount(), 1);

    const NativeDocState stored = m_db->getNativeDocState(state.fileId);
    QVERIFY(stored.nativeDocModeOverride.isEmpty());
    QVERIFY(!QFile::exists(m_tempDir->filePath("downloads/Quarterly.gdoc")));
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

void TestSyncActionThread::testDeleteLocalSymlinkFileRemovesLinkOnly() {
    QTemporaryDir externalDir;
    QVERIFY(externalDir.isValid());

    const QString externalFilePath = externalDir.filePath("outside.txt");
    QVERIFY(QDir().mkpath(QFileInfo(externalFilePath).path()));
    QFile externalFile(externalFilePath);
    QVERIFY(externalFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(externalFile.write("outside"), 7);
    externalFile.close();

    const QString relPath = "delete/outside-link.txt";
    const QString linkPath = m_tempDir->filePath(relPath);
    QVERIFY(QDir().mkpath(QFileInfo(linkPath).path()));

    if (!QFile::link(externalFilePath, linkPath)) {
        QSKIP("Symlink creation not supported");
    }

    m_db->setFileId(relPath, "link-id-1");

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteLocal;
    action.localPath = relPath;

    enqueueAndWait(action);

    QVERIFY(!QFileInfo::exists(linkPath));
    QVERIFY(QFile::exists(externalFilePath));
    QVERIFY(m_db->wasFileDeleted(relPath));
}

void TestSyncActionThread::testDeleteLocalSymlinkDirectoryRemovesLinkOnly() {
    QTemporaryDir externalDir;
    QVERIFY(externalDir.isValid());

    const QString externalTargetDir = externalDir.filePath("outside-dir");
    const QString externalNestedFile = externalDir.filePath("outside-dir/kept.txt");
    QVERIFY(QDir().mkpath(externalTargetDir));
    QFile externalFile(externalNestedFile);
    QVERIFY(externalFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(externalFile.write("outside-dir"), 11);
    externalFile.close();

    const QString relPath = "delete/outside-dir-link";
    const QString childRelPath = relPath + "/kept.txt";
    const QString linkPath = m_tempDir->filePath(relPath);
    QVERIFY(QDir().mkpath(QFileInfo(linkPath).path()));

    if (!QFile::link(externalTargetDir, linkPath)) {
        QSKIP("Symlink creation not supported");
    }

    m_db->setFileId(relPath, "link-id-2");
    m_db->setFileId(childRelPath, "child-id-1");

    SyncActionItem action;
    action.actionType = SyncActionType::DeleteLocal;
    action.localPath = relPath;

    enqueueAndWait(action);

    QVERIFY(!QFileInfo::exists(linkPath));
    QVERIFY(QDir(externalTargetDir).exists());
    QVERIFY(QFile::exists(externalNestedFile));
    QVERIFY(m_db->wasFileDeleted(relPath));
    QVERIFY(!m_db->wasFileDeleted(childRelPath));
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

void TestSyncActionThread::testMoveLocal_DisambiguatesWhenDestinationExists() {
    QString relPath = "move/conflict-source.txt";
    createFile(relPath, "move");
    createFile("move/dest.txt", "existing");
    m_db->setFileId(relPath, "local-id-conflict");

    SyncActionItem action;
    action.actionType = SyncActionType::MoveLocal;
    action.localPath = relPath;
    action.moveDestination = "move/dest.txt";

    enqueueAndWait(action);
    QVERIFY(QFile::exists(m_tempDir->filePath("move/dest_local-id-conflict.txt")));
    QCOMPARE(m_db->getLocalPath("local-id-conflict"), QString("move/dest_local-id-conflict.txt"));
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

void TestSyncActionThread::testRenameLocal_DisambiguatesWhenDestinationExists() {
    QString relPath = "rename/conflict-source.txt";
    createFile(relPath, "rename");
    createFile("rename/target.txt", "existing");
    m_db->setFileId(relPath, "local-id-rename-conflict");

    SyncActionItem action;
    action.actionType = SyncActionType::RenameLocal;
    action.localPath = relPath;
    action.renameTo = "target.txt";

    enqueueAndWait(action);
    QVERIFY(QFile::exists(m_tempDir->filePath("rename/target_local-id-rename-conflict.txt")));
    QCOMPARE(m_db->getLocalPath("local-id-rename-conflict"),
             QString("rename/target_local-id-rename-conflict.txt"));
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

void TestSyncActionThread::testTrashRemoteById() {
    SyncActionItem action;
    action.actionType = SyncActionType::TrashRemote;
    action.fileId = "trash-id-1";

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastTrashCall().fileId, QString("trash-id-1"));
}

void TestSyncActionThread::testTrashRemoteFromDb() {
    QString relPath = "remote/trash.txt";
    m_db->setFileId(relPath, "trash-id-2");

    SyncActionItem action;
    action.actionType = SyncActionType::TrashRemote;
    action.localPath = relPath;

    enqueueAndWait(action);
    QCOMPARE(m_drive->lastTrashCall().fileId, QString("trash-id-2"));
}

void TestSyncActionThread::testTrashRemoteFolderMarksDescendants() {
    FileSyncState folder;
    folder.localPath = "tfolder";
    folder.fileId = "tfolder-id";
    folder.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    folder.isFolder = true;

    FileSyncState childFile;
    childFile.localPath = "tfolder/child.txt";
    childFile.fileId = "tchild-id";
    childFile.modifiedTimeAtSync = QDateTime::currentDateTimeUtc();
    childFile.isFolder = false;

    m_db->saveFileState(folder);
    m_db->saveFileState(childFile);

    SyncActionItem action;
    action.actionType = SyncActionType::TrashRemote;
    action.fileId = "tfolder-id";
    action.localPath = "tfolder";

    enqueueAndWait(action);

    QVERIFY(m_db->wasFileDeleted("tfolder"));
    QVERIFY(m_db->wasFileDeleted("tfolder/child.txt"));
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

void TestSyncActionThread::testRenameRemote_NativeDocStripsVisibleSuffix() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "browser-shortcut");
    settings.sync();

    NativeDocState state;
    state.fileId = "remote-native-rename";
    state.remoteName = "Quarterly";
    state.remoteMimeType = "application/vnd.google-apps.document";
    state.webViewLink = "https://docs.google.com/document/d/remote-native-rename/edit";
    QVERIFY(m_db->saveNativeDocState(state));

    SyncActionItem action;
    action.actionType = SyncActionType::RenameRemote;
    action.localPath = "remote/Quarterly.gdoc";
    action.fileId = state.fileId;
    action.renameTo = "Renamed.gdoc";

    enqueueAndWait(action);

    QCOMPARE(m_drive->lastRenameCall().fileId, state.fileId);
    QCOMPARE(m_drive->lastRenameCall().newName, QString("Renamed"));
    QCOMPARE(m_db->getLocalPath(state.fileId), QString("remote/Renamed.gdoc"));

    const NativeDocState updatedState = m_db->getNativeDocState(state.fileId);
    QCOMPARE(updatedState.remoteName, QString("Renamed"));
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
