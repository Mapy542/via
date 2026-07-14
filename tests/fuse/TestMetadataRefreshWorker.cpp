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
#include "fuse/FileCache.h"
#include "fuse/MetadataCache.h"
#include "fuse/MetadataRefreshWorker.h"
#include "sync/SyncDatabase.h"

class FakeDriveClientMRW : public GoogleDriveClient {
    Q_OBJECT

   public:
    struct InjectedError {
        QString errorMsg;
        int httpStatus = 0;
        int remaining = 1;
        int delayMs = 0;
    };

    struct QueuedChangesResponse {
        QList<DriveChange> changes;
        QString nextToken;
        bool hasMorePages = false;
        int delayMs = -1;
    };

    explicit FakeDriveClientMRW(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    int listChangesCallCount = 0;
    QString lastListChangesToken;
    int getStartPageTokenCallCount = 0;
    int listChangesResponseDelayMs = 0;

    void listChanges(const QString& startPageToken = QString()) override {
        ++listChangesCallCount;
        lastListChangesToken = startPageToken;
        if (emitInjectedError(QStringLiteral("listChanges"))) {
            return;
        }
        const QueuedChangesResponse response = nextResponse();
        const int delayMs = response.delayMs >= 0 ? response.delayMs : listChangesResponseDelayMs;
        QTimer::singleShot(delayMs, this, [this, response]() {
            emit changesReceived(response.changes, response.nextToken, response.hasMorePages);
        });
    }

    void getStartPageToken() override {
        ++getStartPageTokenCallCount;
        if (emitInjectedError(QStringLiteral("getStartPageToken"))) {
            return;
        }
        QTimer::singleShot(0, this, [this]() { emit startPageTokenReceived(m_startPageToken); });
    }

    void downloadFile(const QString&, const QString&) override {}
    void uploadFile(const QString&, const QString&, const QString&) override {}
    void updateFile(const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void renameFile(const QString&, const QString&) override {}
    void deleteFile(const QString&) override {}
    void createFolder(const QString&, const QString&, const QString&) override {}
    QJsonArray getParentsByFileId(const QString&) override { return {}; }
    QString getFolderIdByPath(const QString&) override { return {}; }

    void setStartPageToken(const QString& token) { m_startPageToken = token; }
    void setNextToken(const QString& token) { m_nextToken = token; }
    void setPendingChanges(const QList<DriveChange>& changes) { m_pendingChanges = changes; }
    void setHasMorePages(bool hasMorePages) { m_hasMorePages = hasMorePages; }
    void enqueueChangesResponse(const QList<DriveChange>& changes, const QString& nextToken,
                                bool hasMorePages, int delayMs = -1) {
        m_queuedResponses.append(QueuedChangesResponse{changes, nextToken, hasMorePages, delayMs});
    }
    void injectOperationError(const QString& operation, const QString& errorMsg, int remaining = 1,
                              int delayMs = 0, int httpStatus = 0) {
        m_injectedErrors.insert(operation, InjectedError{errorMsg, httpStatus, remaining, delayMs});
    }

    void emitChangesBatch(const QList<DriveChange>& changes,
                          const QString& nextToken = QStringLiteral("next-token"),
                          bool hasMorePages = false) {
        emit changesReceived(changes, nextToken, hasMorePages);
    }

   private:
    QueuedChangesResponse nextResponse() {
        if (!m_queuedResponses.isEmpty()) {
            return m_queuedResponses.takeFirst();
        }

        return QueuedChangesResponse{m_pendingChanges, m_nextToken, m_hasMorePages,
                                     listChangesResponseDelayMs};
    }

    bool emitInjectedError(const QString& operation) {
        if (!m_injectedErrors.contains(operation)) {
            return false;
        }

        InjectedError injected = m_injectedErrors.value(operation);
        QTimer::singleShot(
            injected.delayMs, this,
            [this, operation, errorMsg = injected.errorMsg, httpStatus = injected.httpStatus]() {
                emit error(operation, errorMsg);
                emit errorDetailed(operation, errorMsg, httpStatus, QString(), QString());
            });

        injected.remaining -= 1;
        if (injected.remaining <= 0) {
            m_injectedErrors.remove(operation);
        } else {
            m_injectedErrors.insert(operation, injected);
        }

        return true;
    }

    QString m_startPageToken = QStringLiteral("start-token");
    QString m_nextToken = QStringLiteral("next-token");
    bool m_hasMorePages = false;
    QList<DriveChange> m_pendingChanges;
    QList<QueuedChangesResponse> m_queuedResponses;
    QHash<QString, InjectedError> m_injectedErrors;
};

class TestMetadataRefreshWorker : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testCreatedChange_EmitsResolvedPath();
    void testCreatedChange_EmitsRootRelativePath();
    void testModifiedChange_EmitsResolvedPath();
    void testDeletedChange_EmitsStoredPathBeforeRemoval();
    void testCreatedChange_FallsBackToFileNameWhenPathUnavailable();
    void testTrashChange_IsIgnored();
    void testModifiedChange_ConsumesPendingOperationAck();
    void testModifiedChange_SkipsInvalidationWhenLocalGenerationAhead();
    void testModifiedChange_UpdatesAuthoritativeNodePathWhenSafe();
    void testDeletedChange_PreservesNodeWhenLocalRenamePending();
    void testTransientApiErrorRetriesWithoutExternalError();
    void testTimeoutApiErrorRetriesWithoutExternalError();
    void testRetryDelaySuppressesPollingCollision();
    void testInFlightRequestDefersExtraCheck();
    void testPaginatedChanges_RequestNextPageImmediately();
    void testExpiredChangeToken_RequestsNewStartPageTokenAndRecovers();

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
    FileCache* m_fileCache = nullptr;
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

    m_fileCache = new FileCache(m_db, m_driveClient, this);
    m_fileCache->setCacheDirectory(m_tempDir->path() + QStringLiteral("/cache"));
    m_fileCache->setDirtyDirectory(m_tempDir->path() + QStringLiteral("/pending"));
    QVERIFY(m_fileCache->initialize());

    m_worker = new MetadataRefreshWorker(m_cache, m_fileCache, m_db, m_driveClient, this);
}

void TestMetadataRefreshWorker::cleanup() {
    delete m_worker;
    m_worker = nullptr;
    delete m_fileCache;
    m_fileCache = nullptr;
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

void TestMetadataRefreshWorker::testCreatedChange_EmitsRootRelativePath() {
    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-root-created"), QStringLiteral("file-root"),
                    QStringLiteral("report.txt"), QStringLiteral("root"))});

    QCOMPARE(changeSpy.count(), 1);
    const QList<QVariant> args = changeSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("report.txt"));
    QCOMPARE(args.at(1).toString(), QStringLiteral("created"));

    const FuseFileMetadata metadata = m_cache->getMetadataByFileId(QStringLiteral("file-root"));
    QVERIFY(metadata.isValid());
    QCOMPARE(metadata.path, QStringLiteral("report.txt"));

    const QList<FuseFileMetadata> rootChildren = m_cache->getChildren(QStringLiteral("/"));
    QCOMPARE(rootChildren.size(), 1);
    QCOMPARE(rootChildren.first().fileId, QStringLiteral("file-root"));
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

void TestMetadataRefreshWorker::testTrashChange_IsIgnored() {
    QSignalSpy detailedSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);
    QSignalSpy genericSpy(m_worker, &MetadataRefreshWorker::changeProcessed);

    DriveFile trashFolder;
    trashFolder.id = QStringLiteral("trash-folder-1");
    trashFolder.name = QStringLiteral(".Trash-1000");
    trashFolder.parents = {QStringLiteral("root")};
    trashFolder.isFolder = true;
    trashFolder.mimeType = QStringLiteral("application/vnd.google-apps.folder");
    trashFolder.ownedByMe = true;
    trashFolder.modifiedTime = QDateTime::currentDateTimeUtc();
    trashFolder.createdTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = QStringLiteral("trash-change-1");
    change.fileId = trashFolder.id;
    change.time = QDateTime::currentDateTimeUtc();
    change.file = trashFolder;

    m_driveClient->emitChangesBatch({change});

    QCOMPARE(detailedSpy.count(), 0);
    QCOMPARE(genericSpy.count(), 0);
    QVERIFY(!m_cache->getMetadataByPath(QStringLiteral(".Trash-1000")).isValid());
    QVERIFY(m_db->getFuseMetadataByPath(QStringLiteral(".Trash-1000")).fileId.isEmpty());
}

void TestMetadataRefreshWorker::testModifiedChange_ConsumesPendingOperationAck() {
    m_cache->setMetadata(makeFolder(QStringLiteral("folder-1"), QStringLiteral("Projects")));
    const FuseFileMetadata cachedFile = makeFile(
        QStringLiteral("file-ack"), QStringLiteral("Projects/ack.txt"), QStringLiteral("folder-1"));
    m_cache->setMetadata(cachedFile);

    const QString cachePath = m_fileCache->getCachePathForFile(QStringLiteral("file-ack"));
    QVERIFY(QDir().mkpath(QFileInfo(cachePath).dir().absolutePath()));
    QFile cacheFile(cachePath);
    QVERIFY(cacheFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(cacheFile.write("ack-bytes"), qint64(9));
    cacheFile.close();
    QVERIFY(m_fileCache->recordCacheEntry(QStringLiteral("file-ack"), cachePath,
                                          QFileInfo(cachePath).size()));

    FuseJournalEntry entry;
    entry.idempotencyKey = QStringLiteral("ack-entry-1");
    entry.operationType = FuseJournalOperationType::WriteGeneration;
    entry.nodeId = QStringLiteral("ack-node");
    entry.path = QStringLiteral("/Projects/ack.txt");
    entry.remoteFileId = QStringLiteral("file-ack");
    entry.localGeneration = 1;
    const qint64 entryId = m_db->appendFuseJournalEntry(entry);
    QVERIFY(entryId > 0);
    QVERIFY(m_db->updateFuseJournalEntryStatus(entryId, FuseJournalEntryStatus::Completed));

    FuseOperationAck ack;
    ack.journalEntryId = entryId;
    ack.idempotencyKey = entry.idempotencyKey;
    ack.nodeId = entry.nodeId;
    ack.remoteFileId = QStringLiteral("file-ack");
    ack.acknowledgedGeneration = 1;
    ack.acknowledgedAt = QDateTime::currentDateTimeUtc();
    QVERIFY(m_db->saveFuseOperationAck(ack));

    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-ack"), QStringLiteral("file-ack"),
                    QStringLiteral("ack.txt"), QStringLiteral("folder-1"))});

    QCOMPARE(changeSpy.count(), 1);
    QVERIFY(m_fileCache->isCached(QStringLiteral("file-ack")));
    const FuseOperationAck consumedAck = m_db->getFuseOperationAck(entryId);
    QVERIFY(consumedAck.appliedAt.isValid());
    QVERIFY(m_db->getPendingFuseOperationAckByRemoteFileId(QStringLiteral("file-ack")).ackId <= 0);
}

void TestMetadataRefreshWorker::testModifiedChange_SkipsInvalidationWhenLocalGenerationAhead() {
    m_cache->setMetadata(makeFolder(QStringLiteral("folder-1"), QStringLiteral("Projects")));
    const FuseFileMetadata cachedFile =
        makeFile(QStringLiteral("file-modified-local"), QStringLiteral("Projects/report.txt"),
                 QStringLiteral("folder-1"));
    m_cache->setMetadata(cachedFile);

    const QString cachePath =
        m_fileCache->getCachePathForFile(QStringLiteral("file-modified-local"));
    const QByteArray localBytes("fresh local bytes");
    QVERIFY(QDir().mkpath(QFileInfo(cachePath).dir().absolutePath()));
    QFile cacheFile(cachePath);
    QVERIFY(cacheFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(cacheFile.write(localBytes), localBytes.size());
    cacheFile.close();
    QVERIFY(m_fileCache->recordCacheEntry(QStringLiteral("file-modified-local"), cachePath,
                                          QFileInfo(cachePath).size()));
    QVERIFY(m_fileCache->isCached(QStringLiteral("file-modified-local")));

    FuseNode node;
    node.nodeId = QStringLiteral("local-newer-node");
    node.remoteFileId = QStringLiteral("file-modified-local");
    node.path = QStringLiteral("/Projects/report.txt");
    node.name = QStringLiteral("report.txt");
    node.remoteName = QStringLiteral("report.txt");
    node.isFolder = false;
    node.size = QFileInfo(cachePath).size();
    node.createdTime = QDateTime::currentDateTimeUtc();
    node.modifiedTime = node.createdTime;
    node.lastAccessed = node.createdTime;
    QVERIFY(m_db->saveFuseNode(node));

    FuseNodeContentState state;
    state.nodeId = node.nodeId;
    state.localContentPath = cachePath;
    state.localGeneration = 2;
    state.remoteAckGeneration = 1;
    state.size = QFileInfo(cachePath).size();
    state.lastLocalWrite = QDateTime::currentDateTimeUtc();
    QVERIFY(m_db->saveFuseNodeContentState(state));

    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-modified-local"), QStringLiteral("file-modified-local"),
                    QStringLiteral("report.txt"), QStringLiteral("folder-1"))});

    QCOMPARE(changeSpy.count(), 1);
    QVERIFY(m_fileCache->isCached(QStringLiteral("file-modified-local")));
}

void TestMetadataRefreshWorker::testModifiedChange_UpdatesAuthoritativeNodePathWhenSafe() {
    m_cache->setMetadata(makeFolder(QStringLiteral("folder-1"), QStringLiteral("Projects")));
    m_cache->setMetadata(makeFile(QStringLiteral("file-rename-safe"),
                                  QStringLiteral("Projects/report.txt"),
                                  QStringLiteral("folder-1")));

    FuseNode node;
    node.nodeId = QStringLiteral("safe-rename-node");
    node.remoteFileId = QStringLiteral("file-rename-safe");
    node.remoteParentId = QStringLiteral("folder-1");
    node.path = QStringLiteral("/Projects/report.txt");
    node.name = QStringLiteral("report.txt");
    node.remoteName = QStringLiteral("report.txt");
    node.isFolder = false;
    node.size = 100;
    node.mimeType = QStringLiteral("text/plain");
    node.createdTime = QDateTime::currentDateTimeUtc();
    node.modifiedTime = node.createdTime;
    node.lastAccessed = node.createdTime;
    QVERIFY(m_db->saveFuseNode(node));

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-rename-safe"), QStringLiteral("file-rename-safe"),
                    QStringLiteral("report-renamed.txt"), QStringLiteral("folder-1"))});

    const FuseNode updated = m_db->getFuseNode(node.nodeId);
    QCOMPARE(updated.path, QStringLiteral("/Projects/report-renamed.txt"));
    QCOMPARE(updated.name, QStringLiteral("report-renamed.txt"));

    const FuseFileMetadata metadata =
        m_cache->getMetadataByFileId(QStringLiteral("file-rename-safe"));
    QVERIFY(metadata.isValid());
    QCOMPARE(metadata.path, QStringLiteral("Projects/report-renamed.txt"));
}

void TestMetadataRefreshWorker::testDeletedChange_PreservesNodeWhenLocalRenamePending() {
    m_cache->setMetadata(makeFolder(QStringLiteral("folder-1"), QStringLiteral("Projects")));
    m_cache->setMetadata(makeFile(QStringLiteral("file-delete-local"),
                                  QStringLiteral("Projects/remove.txt"),
                                  QStringLiteral("folder-1")));

    FuseNode node;
    node.nodeId = QStringLiteral("delete-preserve-node");
    node.remoteFileId = QStringLiteral("file-delete-local");
    node.remoteParentId = QStringLiteral("folder-1");
    node.path = QStringLiteral("/Projects/remove-local-name.txt");
    node.name = QStringLiteral("remove-local-name.txt");
    node.remoteName = QStringLiteral("remove.txt");
    node.isFolder = false;
    node.size = 100;
    node.mimeType = QStringLiteral("text/plain");
    node.createdTime = QDateTime::currentDateTimeUtc();
    node.modifiedTime = node.createdTime;
    node.lastAccessed = node.createdTime;
    QVERIFY(m_db->saveFuseNode(node));

    FuseMutationTransaction renameMutation;
    renameMutation.journalEntry.idempotencyKey = QStringLiteral("pending-local-rename");
    renameMutation.journalEntry.operationType = FuseJournalOperationType::Rename;
    renameMutation.journalEntry.nodeId = node.nodeId;
    renameMutation.journalEntry.path = QStringLiteral("/Projects/remove.txt");
    renameMutation.journalEntry.destinationPath = QStringLiteral("/Projects/remove-local-name.txt");
    QVERIFY(m_db->commitFuseMutationTransaction(renameMutation, nullptr));

    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessedDetailed);

    m_driveClient->emitChangesBatch(
        {makeChange(QStringLiteral("change-delete-preserve"), QStringLiteral("file-delete-local"),
                    QStringLiteral("remove.txt"), QStringLiteral("folder-1"), true)});

    QCOMPARE(changeSpy.count(), 1);
    QVERIFY(!m_db->getFuseNode(node.nodeId).nodeId.isEmpty());
    const FuseFileMetadata metadata =
        m_cache->getMetadataByPath(QStringLiteral("Projects/remove-local-name.txt"));
    QVERIFY(metadata.isValid());
}

void TestMetadataRefreshWorker::testTransientApiErrorRetriesWithoutExternalError() {
    m_db->setFuseSyncState(QStringLiteral("fuse_change_token"), QStringLiteral("token-1"));
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken(QStringLiteral("token-2"));
    m_driveClient->injectOperationError(QStringLiteral("listChanges"),
                                        QStringLiteral("HTTP/2 protocol error"));

    QSignalSpy errorSpy(m_worker, &MetadataRefreshWorker::error);
    QSignalSpy tokenSpy(m_worker, &MetadataRefreshWorker::changeTokenUpdated);

    m_worker->start();

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 2, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(tokenSpy.count() >= 1, 1500);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(m_worker->changeToken(), QStringLiteral("token-2"));
}

void TestMetadataRefreshWorker::testTimeoutApiErrorRetriesWithoutExternalError() {
    m_db->setFuseSyncState(QStringLiteral("fuse_change_token"), QStringLiteral("token-1"));
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken(QStringLiteral("token-2"));
    m_driveClient->injectOperationError(QStringLiteral("listChanges"),
                                        QStringLiteral("Operation timed out"));

    QSignalSpy errorSpy(m_worker, &MetadataRefreshWorker::error);
    QSignalSpy tokenSpy(m_worker, &MetadataRefreshWorker::changeTokenUpdated);

    m_worker->start();

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 2, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(tokenSpy.count() >= 1, 1500);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(m_worker->changeToken(), QStringLiteral("token-2"));
}

void TestMetadataRefreshWorker::testRetryDelaySuppressesPollingCollision() {
    m_db->setFuseSyncState(QStringLiteral("fuse_change_token"), QStringLiteral("token-1"));
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken(QStringLiteral("token-2"));
    m_driveClient->injectOperationError(QStringLiteral("listChanges"),
                                        QStringLiteral("Operation timed out"));
    m_worker->setPollingInterval(50);

    QSignalSpy tokenSpy(m_worker, &MetadataRefreshWorker::changeTokenUpdated);

    m_worker->start();

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 1, 200);
    QTest::qWait(150);
    QCOMPARE(m_driveClient->listChangesCallCount, 1);

    QTRY_VERIFY_WITH_TIMEOUT(m_driveClient->listChangesCallCount >= 2, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(tokenSpy.count() >= 1, 1500);
    m_worker->stop();
    QCOMPARE(m_worker->changeToken(), QStringLiteral("token-2"));
}

void TestMetadataRefreshWorker::testInFlightRequestDefersExtraCheck() {
    m_db->setFuseSyncState(QStringLiteral("fuse_change_token"), QStringLiteral("token-1"));
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken(QStringLiteral("token-2"));
    m_driveClient->listChangesResponseDelayMs = 50;

    m_worker->start();

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 1, 500);
    m_worker->checkNow();
    QCOMPARE(m_driveClient->listChangesCallCount, 1);

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 2, 500);
    QCOMPARE(m_worker->changeToken(), QStringLiteral("token-2"));
}

void TestMetadataRefreshWorker::testPaginatedChanges_RequestNextPageImmediately() {
    m_db->setFuseSyncState(QStringLiteral("fuse_change_token"), QStringLiteral("token-1"));

    m_driveClient->enqueueChangesResponse(
        {makeChange(QStringLiteral("change-page-1"), QStringLiteral("file-page-1"),
                    QStringLiteral("page-1.txt"), QStringLiteral("root"))},
        QStringLiteral("token-2"), true);
    m_driveClient->enqueueChangesResponse(
        {makeChange(QStringLiteral("change-page-2"), QStringLiteral("file-page-2"),
                    QStringLiteral("page-2.txt"), QStringLiteral("root"))},
        QStringLiteral("token-3"), false);

    QSignalSpy changeSpy(m_worker, &MetadataRefreshWorker::changeProcessed);
    QSignalSpy tokenSpy(m_worker, &MetadataRefreshWorker::changeTokenUpdated);

    m_worker->start();

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 2, 1500);
    QTRY_COMPARE_WITH_TIMEOUT(changeSpy.count(), 2, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(tokenSpy.count() >= 2, 1500);
    QCOMPARE(m_driveClient->lastListChangesToken, QStringLiteral("token-2"));
    QCOMPARE(m_worker->changeToken(), QStringLiteral("token-3"));
    QCOMPARE(m_db->getFuseSyncState(QStringLiteral("fuse_change_token")),
             QStringLiteral("token-3"));
}

void TestMetadataRefreshWorker::testExpiredChangeToken_RequestsNewStartPageTokenAndRecovers() {
    m_db->setFuseSyncState(QStringLiteral("fuse_change_token"), QStringLiteral("stale-token"));
    m_driveClient->setStartPageToken(QStringLiteral("fresh-start-token"));
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken(QStringLiteral("caught-up-token"));
    m_driveClient->injectOperationError(QStringLiteral("listChanges"),
                                        QStringLiteral("The page token is expired."), 1, 0, 410);

    QSignalSpy errorSpy(m_worker, &MetadataRefreshWorker::error);
    QSignalSpy tokenSpy(m_worker, &MetadataRefreshWorker::changeTokenUpdated);

    m_worker->start();

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->getStartPageTokenCallCount, 1, 1500);
    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 2, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(tokenSpy.count() >= 2, 1500);
    QCOMPARE(m_driveClient->lastListChangesToken, QStringLiteral("fresh-start-token"));
    QCOMPARE(m_worker->changeToken(), QStringLiteral("caught-up-token"));
    QCOMPARE(m_db->getFuseSyncState(QStringLiteral("fuse_change_token")),
             QStringLiteral("caught-up-token"));
    QVERIFY(errorSpy.count() >= 1);
    QVERIFY(
        errorSpy.at(0).at(0).toString().contains(QStringLiteral("expired"), Qt::CaseInsensitive));
}

QTEST_MAIN(TestMetadataRefreshWorker)
#include "TestMetadataRefreshWorker.moc"