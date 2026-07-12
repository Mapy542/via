/**
 * @file TestFuseReplayWorker.cpp
 * @brief Focused unit tests for durable FUSE journal replay.
 */

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>
#include <functional>

#include "api/GoogleDriveClient.h"
#include "fuse/FuseReplayWorker.h"
#include "sync/SyncDatabase.h"

class FakeDriveClientFRW : public GoogleDriveClient {
    Q_OBJECT

   public:
    struct InjectedError {
        QString operation;
        QString message;
        int httpStatus = 0;
        int remaining = 1;
    };

    explicit FakeDriveClientFRW(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    std::function<void()> beforeCompleteUpload;

    int uploadCallCount = 0;
    int updateCallCount = 0;
    int createFolderCallCount = 0;
    int renameCallCount = 0;
    int trashCallCount = 0;
    QList<QString> uploadedParentIds;
    QList<QString> uploadedNames;
    QList<QString> updatedFileIds;
    QList<QString> createdFolderParentIds;
    QList<QString> createdFolderNames;
    QList<QString> renamedIds;
    QList<QString> renamedNames;
    QList<QString> trashedIds;
    int nextId = 0;
    QList<InjectedError> injectedErrors;

    QString nextGeneratedId(const QString& prefix) {
        ++nextId;
        return QStringLiteral("%1-%2").arg(prefix).arg(nextId);
    }

    void injectError(const QString& operation, const QString& message, int httpStatus,
                     int remaining = 1) {
        injectedErrors.append(InjectedError{operation, message, httpStatus, remaining});
    }

    bool emitInjectedError(const QString& operation, const QString& fileId = QString(),
                           const QString& localPath = QString()) {
        for (int index = 0; index < injectedErrors.size(); ++index) {
            InjectedError& injected = injectedErrors[index];
            if (injected.operation != operation || injected.remaining <= 0) {
                continue;
            }

            injected.remaining -= 1;
            const QString message = injected.message;
            const int httpStatus = injected.httpStatus;
            QTimer::singleShot(
                0, this, [this, operation, message, httpStatus, fileId, localPath]() {
                    emit errorDetailed(operation, message, httpStatus, fileId, localPath);
                });

            if (injected.remaining <= 0) {
                injectedErrors.removeAt(index);
            }
            return true;
        }

        return false;
    }

    void uploadFile(const QString& localPath, const QString& parentId,
                    const QString& fileName) override {
        ++uploadCallCount;
        uploadedParentIds.append(parentId);
        uploadedNames.append(fileName);

        if (emitInjectedError(QStringLiteral("uploadFile"), QString(), localPath)) {
            return;
        }

        DriveFile file;
        file.id = nextGeneratedId(QStringLiteral("upload"));
        file.name = fileName;
        file.parents = {parentId};
        file.size = QFileInfo(localPath).size();
        file.mimeType = QStringLiteral("application/octet-stream");
        file.createdTime = QDateTime::currentDateTimeUtc();
        file.modifiedTime = file.createdTime;

        QTimer::singleShot(0, this, [this, file, localPath]() {
            if (beforeCompleteUpload) {
                beforeCompleteUpload();
            }
            emit fileUploadedDetailed(file, localPath);
        });
    }

    void updateFile(const QString& fileId, const QString& localPath) override {
        ++updateCallCount;
        updatedFileIds.append(fileId);

        if (emitInjectedError(QStringLiteral("updateFile"), fileId, localPath)) {
            return;
        }

        DriveFile file;
        file.id = fileId;
        file.name = QFileInfo(localPath).fileName();
        file.size = QFileInfo(localPath).size();
        file.mimeType = QStringLiteral("application/octet-stream");
        file.createdTime = QDateTime::currentDateTimeUtc();
        file.modifiedTime = file.createdTime;

        QTimer::singleShot(0, this, [this, file]() { emit fileUpdated(file); });
    }

    void createFolder(const QString& name, const QString& parentId,
                      const QString& localPath) override {
        ++createFolderCallCount;
        createdFolderParentIds.append(parentId);
        createdFolderNames.append(name);

        if (emitInjectedError(QStringLiteral("createFolder"), QString(), localPath)) {
            return;
        }

        DriveFile folder;
        folder.id = nextGeneratedId(QStringLiteral("folder"));
        folder.name = name;
        folder.parents = {parentId};
        folder.isFolder = true;
        folder.mimeType = QStringLiteral("application/vnd.google-apps.folder");
        folder.createdTime = QDateTime::currentDateTimeUtc();
        folder.modifiedTime = folder.createdTime;

        QTimer::singleShot(0, this, [this, folder, localPath]() {
            emit folderCreatedDetailed(folder, localPath);
        });
    }

    void renameFile(const QString& fileId, const QString& newName) override {
        ++renameCallCount;
        renamedIds.append(fileId);
        renamedNames.append(newName);

        if (emitInjectedError(QStringLiteral("renameFile"), fileId)) {
            return;
        }

        DriveFile file;
        file.id = fileId;
        file.name = newName;
        file.modifiedTime = QDateTime::currentDateTimeUtc();
        QTimer::singleShot(0, this, [this, file]() { emit fileRenamedDetailed(file); });
    }

    void trashFile(const QString& fileId) override {
        ++trashCallCount;
        trashedIds.append(fileId);

        if (emitInjectedError(QStringLiteral("trashFile"), fileId)) {
            return;
        }

        QTimer::singleShot(0, this, [this, fileId]() { emit fileTrashed(fileId); });
    }

    void downloadFile(const QString&, const QString&) override {}
    void exportFile(const QString&, const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void moveAndRenameFile(const QString&, const QString&, const QString&,
                           const QString&) override {}
    void deleteFile(const QString&) override {}
    void untrashFile(const QString&) override {}
    QJsonArray getParentsByFileId(const QString&) override { return {}; }
    QString getFolderIdByPath(const QString&) override { return {}; }
};

namespace {

class ThreadedReplayHarness {
   public:
    ThreadedReplayHarness(SyncDatabase* database, GoogleDriveClient* driveClient)
        : worker(new FuseReplayWorker(database, driveClient)) {
        worker->setSyncIntervalMs(60000);
        worker->moveToThread(&thread);
        thread.start();
        QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);
    }

    ~ThreadedReplayHarness() {
        if (thread.isRunning() && worker) {
            QMetaObject::invokeMethod(worker, "stop", Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait(5000);
        }
        delete worker;
    }

    FuseReplayWorker* worker = nullptr;

   private:
    QThread thread;
};

}  // namespace

class TestFuseReplayWorker : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testReplayCreateDirectoryThenFile_AssignsRemoteIds();
    void testReplayWriteAfterCreate_CompletesQueuedContentMutation();
    void testReplayConflict_KeepBothBlocksRename();
    void testReplayConflict_KeepRemoteCompletesRename();
    void testReplayTrashWithoutRemoteId_CompletesLocally();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    SyncDatabase* m_db = nullptr;
    FakeDriveClientFRW* m_driveClient = nullptr;
};

void TestFuseReplayWorker::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    QStandardPaths::setTestModeEnabled(true);
    qputenv("HOME", m_tempDir->path().toUtf8());

    m_db = new SyncDatabase();
    QVERIFY(m_db->initialize());

    m_driveClient = new FakeDriveClientFRW(this);
}

void TestFuseReplayWorker::cleanup() {
    QSettings settings;
    settings.remove(QStringLiteral("sync/conflictStrategy"));

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

void TestFuseReplayWorker::testReplayCreateDirectoryThenFile_AssignsRemoteIds() {
    FuseNode dirNode;
    dirNode.nodeId = QStringLiteral("dir-node");
    dirNode.path = QStringLiteral("/Projects");
    dirNode.name = QStringLiteral("Projects");
    dirNode.remoteName = dirNode.name;
    dirNode.isFolder = true;
    dirNode.isPendingCreate = true;
    dirNode.mimeType = QStringLiteral("application/vnd.google-apps.folder");
    dirNode.createdTime = QDateTime::currentDateTimeUtc();
    dirNode.modifiedTime = dirNode.createdTime;
    dirNode.lastAccessed = dirNode.createdTime;

    FuseMutationTransaction dirMutation;
    dirMutation.nodesToUpsert.append(dirNode);
    dirMutation.journalEntry.idempotencyKey = QStringLiteral("create-dir-1");
    dirMutation.journalEntry.operationType = FuseJournalOperationType::CreateDirectory;
    dirMutation.journalEntry.nodeId = dirNode.nodeId;
    dirMutation.journalEntry.path = dirNode.path;
    QVERIFY(m_db->commitFuseMutationTransaction(dirMutation, nullptr));

    const QString contentPath = m_tempDir->path() + QStringLiteral("/board.kicad_pcb");
    QFile localFile(contentPath);
    QVERIFY(localFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(localFile.write("pcb-bytes"), qint64(9));
    localFile.close();

    FuseNode fileNode;
    fileNode.nodeId = QStringLiteral("file-node");
    fileNode.parentNodeId = dirNode.nodeId;
    fileNode.path = QStringLiteral("/Projects/board.kicad_pcb");
    fileNode.name = QStringLiteral("board.kicad_pcb");
    fileNode.remoteName = fileNode.name;
    fileNode.isFolder = false;
    fileNode.isPendingCreate = true;
    fileNode.mimeType = QStringLiteral("application/octet-stream");
    fileNode.createdTime = QDateTime::currentDateTimeUtc();
    fileNode.modifiedTime = fileNode.createdTime;
    fileNode.lastAccessed = fileNode.createdTime;

    FuseNodeContentState fileState;
    fileState.nodeId = fileNode.nodeId;
    fileState.localContentPath = contentPath;
    fileState.localGeneration = 1;
    fileState.remoteAckGeneration = 0;
    fileState.size = QFileInfo(contentPath).size();
    fileState.lastLocalWrite = QDateTime::currentDateTimeUtc();

    FuseMutationTransaction fileMutation;
    fileMutation.nodesToUpsert.append(fileNode);
    fileMutation.contentStatesToUpsert.append(fileState);
    fileMutation.journalEntry.idempotencyKey = QStringLiteral("create-file-1");
    fileMutation.journalEntry.operationType = FuseJournalOperationType::CreateFile;
    fileMutation.journalEntry.nodeId = fileNode.nodeId;
    fileMutation.journalEntry.parentNodeId = dirNode.nodeId;
    fileMutation.journalEntry.path = fileNode.path;
    QVERIFY(m_db->commitFuseMutationTransaction(fileMutation, nullptr));

    ThreadedReplayHarness harness(m_db, m_driveClient);
    QSignalSpy completedSpy(harness.worker, &FuseReplayWorker::replayEntryCompleted);

    QTRY_VERIFY_WITH_TIMEOUT(completedSpy.size() >= 2, 5000);

    const FuseNode replayedDir = m_db->getFuseNode(dirNode.nodeId);
    QVERIFY(!replayedDir.remoteFileId.isEmpty());
    QVERIFY(!replayedDir.isPendingCreate);

    const FuseNode replayedFile = m_db->getFuseNode(fileNode.nodeId);
    QVERIFY(!replayedFile.remoteFileId.isEmpty());
    QVERIFY(!replayedFile.isPendingCreate);
    QCOMPARE(m_driveClient->uploadedParentIds, QList<QString>{replayedDir.remoteFileId});

    const FuseNodeContentState replayedState = m_db->getFuseNodeContentState(fileNode.nodeId);
    QCOMPARE(replayedState.remoteAckGeneration, static_cast<quint64>(1));

    const QList<FuseJournalEntry> journal = m_db->getAllFuseJournalEntries();
    QCOMPARE(journal.size(), 2);
    QCOMPARE(journal.at(0).status, FuseJournalEntryStatus::Completed);
    QCOMPARE(journal.at(1).status, FuseJournalEntryStatus::Completed);
}

void TestFuseReplayWorker::testReplayWriteAfterCreate_CompletesQueuedContentMutation() {
    const QString contentPath = m_tempDir->path() + QStringLiteral("/write-after-create.txt");
    QFile localFile(contentPath);
    QVERIFY(localFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(localFile.write("initial-bytes"), qint64(13));
    localFile.close();

    FuseNode fileNode;
    fileNode.nodeId = QStringLiteral("write-after-create-node");
    fileNode.path = QStringLiteral("/write-after-create.txt");
    fileNode.name = QStringLiteral("write-after-create.txt");
    fileNode.remoteName = fileNode.name;
    fileNode.isFolder = false;
    fileNode.isPendingCreate = true;
    fileNode.mimeType = QStringLiteral("application/octet-stream");
    fileNode.createdTime = QDateTime::currentDateTimeUtc();
    fileNode.modifiedTime = fileNode.createdTime;
    fileNode.lastAccessed = fileNode.createdTime;

    FuseNodeContentState fileState;
    fileState.nodeId = fileNode.nodeId;
    fileState.localContentPath = contentPath;
    fileState.localGeneration = 0;
    fileState.remoteAckGeneration = 0;
    fileState.size = QFileInfo(contentPath).size();
    fileState.lastLocalWrite = QDateTime::currentDateTimeUtc();

    FuseMutationTransaction createMutation;
    createMutation.nodesToUpsert.append(fileNode);
    createMutation.contentStatesToUpsert.append(fileState);
    createMutation.journalEntry.idempotencyKey = QStringLiteral("create-file-remap-1");
    createMutation.journalEntry.operationType = FuseJournalOperationType::CreateFile;
    createMutation.journalEntry.nodeId = fileNode.nodeId;
    createMutation.journalEntry.path = fileNode.path;
    QVERIFY(m_db->commitFuseMutationTransaction(createMutation, nullptr));

    m_driveClient->beforeCompleteUpload = [&, contentPath]() {
        QFile updateFile(contentPath);
        QVERIFY(updateFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(updateFile.write("updated-bytes-after-create"), qint64(26));
        updateFile.close();

        FuseNodeContentState updatedState = m_db->getFuseNodeContentState(fileNode.nodeId);
        updatedState.localGeneration = 2;
        updatedState.size = QFileInfo(contentPath).size();
        updatedState.lastLocalWrite = QDateTime::currentDateTimeUtc();
        QVERIFY(m_db->saveFuseNodeContentState(updatedState));

        FuseJournalEntry writeEntry;
        writeEntry.idempotencyKey = QStringLiteral("write-after-create-1");
        writeEntry.operationType = FuseJournalOperationType::WriteGeneration;
        writeEntry.nodeId = fileNode.nodeId;
        writeEntry.path = fileNode.path;
        writeEntry.localGeneration = 2;
        QVERIFY(m_db->appendFuseJournalEntry(writeEntry) > 0);
    };

    ThreadedReplayHarness harness(m_db, m_driveClient);
    QSignalSpy completedSpy(harness.worker, &FuseReplayWorker::replayEntryCompleted);

    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.size(), 1, 5000);
    QMetaObject::invokeMethod(harness.worker, "syncNow", Qt::QueuedConnection);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.size(), 2, 5000);

    const FuseNode replayedNode = m_db->getFuseNode(fileNode.nodeId);
    QVERIFY(!replayedNode.remoteFileId.isEmpty());
    const FuseNodeContentState replayedState = m_db->getFuseNodeContentState(fileNode.nodeId);
    QCOMPARE(replayedState.remoteAckGeneration, static_cast<quint64>(2));
    QCOMPARE(m_driveClient->uploadCallCount, 1);
    QCOMPARE(m_driveClient->updateCallCount, 1);
    QCOMPARE(m_driveClient->updatedFileIds, QList<QString>{replayedNode.remoteFileId});
    m_driveClient->beforeCompleteUpload = {};
}

void TestFuseReplayWorker::testReplayConflict_KeepBothBlocksRename() {
    QSettings settings;
    settings.setValue(QStringLiteral("sync/conflictStrategy"), QStringLiteral("keep-both"));

    FuseNode node;
    node.nodeId = QStringLiteral("conflict-node");
    node.remoteFileId = QStringLiteral("remote-conflict-file");
    node.remoteParentId = QStringLiteral("root");
    node.path = QStringLiteral("/renamed-locally.txt");
    node.name = QStringLiteral("renamed-locally.txt");
    node.remoteName = QStringLiteral("old.txt");
    node.isFolder = false;
    node.mimeType = QStringLiteral("text/plain");
    node.createdTime = QDateTime::currentDateTimeUtc();
    node.modifiedTime = node.createdTime;
    node.lastAccessed = node.createdTime;
    QVERIFY(m_db->saveFuseNode(node));

    FuseMutationTransaction mutation;
    mutation.nodesToUpsert.append(node);
    mutation.journalEntry.idempotencyKey = QStringLiteral("rename-conflict-1");
    mutation.journalEntry.operationType = FuseJournalOperationType::Rename;
    mutation.journalEntry.nodeId = node.nodeId;
    mutation.journalEntry.path = QStringLiteral("/old.txt");
    mutation.journalEntry.destinationPath = QStringLiteral("/renamed-locally.txt");
    mutation.journalEntry.remoteFileId = node.remoteFileId;
    mutation.journalEntry.remoteParentId = node.remoteParentId;
    QVERIFY(m_db->commitFuseMutationTransaction(mutation, nullptr));

    m_driveClient->injectError(QStringLiteral("renameFile"),
                               QStringLiteral("409 conflict: already exists"), 409);

    ThreadedReplayHarness harness(m_db, m_driveClient);
    QSignalSpy blockedSpy(harness.worker, &FuseReplayWorker::replayEntryBlocked);

    QTRY_COMPARE_WITH_TIMEOUT(blockedSpy.size(), 1, 5000);

    const QList<FuseJournalEntry> journal = m_db->getAllFuseJournalEntries();
    QCOMPARE(journal.size(), 1);
    QCOMPARE(journal.first().status, FuseJournalEntryStatus::BlockedConflict);
    QCOMPARE(m_driveClient->renameCallCount, 1);
}

void TestFuseReplayWorker::testReplayConflict_KeepRemoteCompletesRename() {
    QSettings settings;
    settings.setValue(QStringLiteral("sync/conflictStrategy"), QStringLiteral("keep-remote"));

    FuseNode node;
    node.nodeId = QStringLiteral("keep-remote-node");
    node.remoteFileId = QStringLiteral("remote-keep-remote");
    node.remoteParentId = QStringLiteral("root");
    node.path = QStringLiteral("/renamed-locally.txt");
    node.name = QStringLiteral("renamed-locally.txt");
    node.remoteName = QStringLiteral("old.txt");
    node.isFolder = false;
    node.mimeType = QStringLiteral("text/plain");
    node.createdTime = QDateTime::currentDateTimeUtc();
    node.modifiedTime = node.createdTime;
    node.lastAccessed = node.createdTime;
    QVERIFY(m_db->saveFuseNode(node));

    FuseMutationTransaction mutation;
    mutation.nodesToUpsert.append(node);
    mutation.journalEntry.idempotencyKey = QStringLiteral("rename-conflict-keep-remote");
    mutation.journalEntry.operationType = FuseJournalOperationType::Rename;
    mutation.journalEntry.nodeId = node.nodeId;
    mutation.journalEntry.path = QStringLiteral("/old.txt");
    mutation.journalEntry.destinationPath = QStringLiteral("/renamed-locally.txt");
    mutation.journalEntry.remoteFileId = node.remoteFileId;
    mutation.journalEntry.remoteParentId = node.remoteParentId;
    QVERIFY(m_db->commitFuseMutationTransaction(mutation, nullptr));

    m_driveClient->injectError(QStringLiteral("renameFile"),
                               QStringLiteral("409 conflict: already exists"), 409);

    ThreadedReplayHarness harness(m_db, m_driveClient);
    QSignalSpy completedSpy(harness.worker, &FuseReplayWorker::replayEntryCompleted);

    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.size(), 1, 5000);

    const QList<FuseJournalEntry> journal = m_db->getAllFuseJournalEntries();
    QCOMPARE(journal.size(), 1);
    QCOMPARE(journal.first().status, FuseJournalEntryStatus::Completed);
    QCOMPARE(m_driveClient->renameCallCount, 1);
}

void TestFuseReplayWorker::testReplayTrashWithoutRemoteId_CompletesLocally() {
    FuseJournalEntry entry;
    entry.idempotencyKey = QStringLiteral("trash-no-remote");
    entry.operationType = FuseJournalOperationType::Trash;
    entry.nodeId = QStringLiteral("missing-node");
    entry.path = QStringLiteral("/temp.txt");
    QVERIFY(m_db->appendFuseJournalEntry(entry) > 0);

    ThreadedReplayHarness harness(m_db, m_driveClient);
    QSignalSpy completedSpy(harness.worker, &FuseReplayWorker::replayEntryCompleted);

    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.size(), 1, 5000);
    QCOMPARE(m_driveClient->trashCallCount, 0);

    const QList<FuseJournalEntry> journal = m_db->getAllFuseJournalEntries();
    QCOMPARE(journal.size(), 1);
    QCOMPARE(journal.first().status, FuseJournalEntryStatus::Completed);
}

QTEST_MAIN(TestFuseReplayWorker)
#include "TestFuseReplayWorker.moc"