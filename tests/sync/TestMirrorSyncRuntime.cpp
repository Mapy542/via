/**
 * @file TestMirrorSyncRuntime.cpp
 * @brief Regression tests for the mirror runtime facade boundary.
 */

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include "api/GoogleDriveClient.h"
#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/FullSync.h"
#include "sync/LocalChangeWatcher.h"
#include "sync/MirrorSyncRuntime.h"
#include "sync/RemoteChangeWatcher.h"
#include "sync/SyncActionQueue.h"
#include "sync/SyncActionThread.h"
#include "sync/SyncDatabase.h"

Q_DECLARE_METATYPE(FullSync::State)

class FakeDriveClientMSR : public GoogleDriveClient {
    Q_OBJECT

   public:
    explicit FakeDriveClientMSR(QObject* parent = nullptr) : GoogleDriveClient(nullptr, parent) {}

    void getFile(const QString&) override {}
    void downloadFile(const QString&, const QString&) override {}
    void uploadFile(const QString&, const QString&, const QString&) override {}
    void updateFile(const QString&, const QString&) override {}
    void moveFile(const QString&, const QString&, const QString&) override {}
    void renameFile(const QString&, const QString&) override {}
    void deleteFile(const QString&) override {}
    void trashFile(const QString&) override {}
    void untrashFile(const QString&) override {}
    void createFolder(const QString&, const QString&, const QString&) override {}
    void listFiles(const QString&, const QString&) override {}
    void listChanges(const QString&) override {}
    void getStartPageToken() override {}
    void getAboutInfo() override {}
    QString getRootFolderId() override { return QStringLiteral("root"); }
};

class TestChangeProcessorRuntime : public ChangeProcessor {
    Q_OBJECT

   public:
    using ChangeProcessor::ChangeProcessor;

    void emitErrorSignal(const QString& errorMessage) { emit error(errorMessage); }
    void emitChangeProcessedSignal(const QString& localPath) { emit changeProcessed(localPath); }
};

class TestSyncActionThreadRuntime : public SyncActionThread {
    Q_OBJECT

   public:
    using SyncActionThread::SyncActionThread;

    void emitActionCompletedSignal(const SyncActionItem& item) { emit actionCompleted(item); }
    void emitActionFailedSignal(const SyncActionItem& item, const QString& errorMessage) {
        emit actionFailed(item, errorMessage);
    }
    void emitActionProgressSignal(const SyncActionItem& item, qint64 bytesProcessed,
                                  qint64 bytesTotal) {
        emit actionProgress(item, bytesProcessed, bytesTotal);
    }
    void emitErrorSignal(const QString& errorMessage) { emit error(errorMessage); }
};

class TestFullSyncRuntime : public FullSync {
    Q_OBJECT

   public:
    using FullSync::FullSync;

    void emitStateChangedSignal(FullSync::State state) { emit stateChanged(state); }
    void emitProgressUpdatedSignal(const QString& phase, int current, int total) {
        emit progressUpdated(phase, current, total);
    }
    void emitRemoteFolderMapReadySignal(const QHash<QString, QString>& mapping) {
        emit remoteFolderMapReady(mapping);
    }
    void emitCompletedSignal(int localCount, int remoteCount) {
        emit completed(localCount, remoteCount);
    }
    void emitErrorSignal(const QString& errorMessage) { emit error(errorMessage); }
};

class TestMirrorSyncRuntime : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testMovesMirrorGraphToDedicatedWorkerThread();
    void testForwardsPendingCountsAndConflictResolution();
    void testForwardsProcessorSyncActionAndFullSyncSignals();
    void testInitialSyncDefersRemoteWatcherUntilFolderAuthorityReady();
    void testRestartAfterWakeDefersRemoteWatcherUntilFolderAuthorityReady();
    void testClearSessionStateResetsRuntimeCaches();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    QByteArray m_originalHome;

    ChangeQueue* m_changeQueue = nullptr;
    SyncActionQueue* m_syncActionQueue = nullptr;
    SyncDatabase* m_syncDatabase = nullptr;
    FakeDriveClientMSR* m_driveClient = nullptr;
    LocalChangeWatcher* m_localWatcher = nullptr;
    RemoteChangeWatcher* m_remoteWatcher = nullptr;
    TestChangeProcessorRuntime* m_changeProcessor = nullptr;
    TestSyncActionThreadRuntime* m_syncActionThread = nullptr;
    TestFullSyncRuntime* m_fullSync = nullptr;
    MirrorSyncRuntime* m_runtime = nullptr;
};

void TestMirrorSyncRuntime::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    m_originalHome = qgetenv("HOME");
    qputenv("HOME", m_tempDir->path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    qRegisterMetaType<ChangeProcessor::State>("ChangeProcessor::State");
    qRegisterMetaType<ConflictInfo>("ConflictInfo");
    qRegisterMetaType<ConflictResolutionStrategy>("ConflictResolutionStrategy");
    qRegisterMetaType<FullSync::State>("FullSync::State");
    qRegisterMetaType<QHash<QString, QString>>("QHash<QString,QString>");
    qRegisterMetaType<SyncActionItem>("SyncActionItem");

    m_changeQueue = new ChangeQueue();
    m_syncActionQueue = new SyncActionQueue();
    m_syncDatabase = new SyncDatabase();
    QVERIFY(m_syncDatabase->initialize());

    m_driveClient = new FakeDriveClientMSR();
    m_localWatcher = new LocalChangeWatcher(m_changeQueue);
    m_remoteWatcher = new RemoteChangeWatcher(m_changeQueue, m_driveClient, m_syncDatabase);
    m_changeProcessor = new TestChangeProcessorRuntime(m_changeQueue, m_syncActionQueue,
                                                       m_syncDatabase, m_driveClient);
    m_syncActionThread = new TestSyncActionThreadRuntime(
        m_syncActionQueue, m_syncDatabase, m_driveClient, m_changeProcessor, m_localWatcher);
    m_fullSync =
        new TestFullSyncRuntime(m_changeQueue, m_syncDatabase, m_driveClient, m_changeProcessor);
    m_runtime =
        new MirrorSyncRuntime(m_localWatcher, m_remoteWatcher, m_changeProcessor, m_syncActionQueue,
                              m_syncActionThread, m_fullSync, m_driveClient, m_syncDatabase);
}

void TestMirrorSyncRuntime::cleanup() {
    delete m_runtime;
    m_runtime = nullptr;

    delete m_fullSync;
    m_fullSync = nullptr;

    delete m_syncActionThread;
    m_syncActionThread = nullptr;

    delete m_changeProcessor;
    m_changeProcessor = nullptr;

    delete m_remoteWatcher;
    m_remoteWatcher = nullptr;

    delete m_localWatcher;
    m_localWatcher = nullptr;

    delete m_driveClient;
    m_driveClient = nullptr;

    if (m_syncDatabase) {
        m_syncDatabase->close();
        delete m_syncDatabase;
        m_syncDatabase = nullptr;
    }

    delete m_syncActionQueue;
    m_syncActionQueue = nullptr;

    delete m_changeQueue;
    m_changeQueue = nullptr;

    QStandardPaths::setTestModeEnabled(false);
    if (m_originalHome.isEmpty()) {
        qunsetenv("HOME");
    } else {
        qputenv("HOME", m_originalHome);
    }

    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestMirrorSyncRuntime::testMovesMirrorGraphToDedicatedWorkerThread() {
    QThread* const runtimeThread = m_runtime->thread();

    QCOMPARE(runtimeThread, QThread::currentThread());
    QVERIFY(m_driveClient->thread() != runtimeThread);
    QCOMPARE(m_localWatcher->thread(), m_driveClient->thread());
    QCOMPARE(m_remoteWatcher->thread(), m_driveClient->thread());
    QCOMPARE(m_changeProcessor->thread(), m_driveClient->thread());
    QCOMPARE(m_syncActionThread->thread(), m_driveClient->thread());
    QCOMPARE(m_fullSync->thread(), m_driveClient->thread());

    QSignalSpy stateSpy(m_runtime, &MirrorSyncRuntime::processorStateChanged);

    m_runtime->start();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QCOMPARE(m_runtime->processorState(), ChangeProcessor::State::Running);

    m_runtime->cancelAndStop();

    QTRY_VERIFY(stateSpy.count() >= 2);
    QCOMPARE(m_runtime->processorState(), ChangeProcessor::State::Stopped);
}

void TestMirrorSyncRuntime::testForwardsPendingCountsAndConflictResolution() {
    QSignalSpy pendingSpy(m_runtime, &MirrorSyncRuntime::pendingActionsChanged);
    QSignalSpy conflictDetectedSpy(m_runtime, &MirrorSyncRuntime::conflictDetected);
    QSignalSpy conflictResolvedSpy(m_runtime, &MirrorSyncRuntime::conflictResolved);
    QSignalSpy queuedSpy(m_runtime, &MirrorSyncRuntime::syncActionQueued);

    SyncActionItem queuedItem;
    queuedItem.actionType = SyncActionType::Upload;
    queuedItem.localPath = QStringLiteral("docs/queued.txt");
    m_syncActionQueue->enqueue(queuedItem);

    QTRY_COMPARE(pendingSpy.count(), 1);
    QCOMPARE(m_runtime->pendingActionCount(), 1);
    QCOMPARE(pendingSpy.takeFirst().at(0).toInt(), 1);

    const QString localPath = QStringLiteral("docs/conflict.txt");
    const QString fileId = QStringLiteral("file-1");
    const int conflictId = m_syncDatabase->upsertConflictRecord(localPath, fileId);
    QVERIFY(conflictId >= 0);

    ConflictVersion version;
    version.localModifiedTime = QDateTime::currentDateTimeUtc();
    version.remoteModifiedTime = version.localModifiedTime.addSecs(5);
    version.dbSyncTime = version.localModifiedTime.addSecs(-5);
    m_syncDatabase->addConflictVersion(conflictId, version);

    QMetaObject::invokeMethod(
        m_changeProcessor, [this]() { m_changeProcessor->rehydrateUnresolvedConflicts(); },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(conflictDetectedSpy.count(), 1);
    QCOMPARE(m_runtime->unresolvedConflictCount(), 1);
    QCOMPARE(m_runtime->unresolvedConflicts().size(), 1);

    m_runtime->resolveConflict(localPath, ConflictResolutionStrategy::KeepLocal);

    QTRY_COMPARE(conflictResolvedSpy.count(), 1);
    QCOMPARE(m_runtime->unresolvedConflictCount(), 0);
    QTRY_COMPARE(queuedSpy.count(), 2);

    const SyncActionItem resolvedItem = qvariant_cast<SyncActionItem>(queuedSpy.at(1).at(0));
    QCOMPARE(resolvedItem.actionType, SyncActionType::Upload);
    QCOMPARE(resolvedItem.localPath, localPath);
    QCOMPARE(resolvedItem.fileId, fileId);
}

void TestMirrorSyncRuntime::testForwardsProcessorSyncActionAndFullSyncSignals() {
    QSignalSpy stateSpy(m_runtime, &MirrorSyncRuntime::processorStateChanged);
    QSignalSpy processorErrorSpy(m_runtime, &MirrorSyncRuntime::processorError);
    QSignalSpy changeProcessedSpy(m_runtime, &MirrorSyncRuntime::changeProcessed);
    QSignalSpy syncActionCompletedSpy(m_runtime, &MirrorSyncRuntime::syncActionCompleted);
    QSignalSpy syncActionFailedSpy(m_runtime, &MirrorSyncRuntime::syncActionFailed);
    QSignalSpy syncActionProgressSpy(m_runtime, &MirrorSyncRuntime::syncActionProgress);
    QSignalSpy syncActionErrorSpy(m_runtime, &MirrorSyncRuntime::syncActionError);
    QSignalSpy fullSyncStateSpy(m_runtime, &MirrorSyncRuntime::fullSyncStateChanged);
    QSignalSpy fullSyncProgressSpy(m_runtime, &MirrorSyncRuntime::fullSyncProgressUpdated);
    QSignalSpy fullSyncCompletedSpy(m_runtime, &MirrorSyncRuntime::fullSyncCompleted);
    QSignalSpy fullSyncErrorSpy(m_runtime, &MirrorSyncRuntime::fullSyncError);

    m_runtime->start();

    QTRY_COMPARE(stateSpy.count(), 1);
    QCOMPARE(m_runtime->processorState(), ChangeProcessor::State::Running);

    QMetaObject::invokeMethod(
        m_changeProcessor,
        [this]() {
            m_changeProcessor->emitErrorSignal(QStringLiteral("processor boom"));
            m_changeProcessor->emitChangeProcessedSignal(QStringLiteral("docs/processed.txt"));
        },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(processorErrorSpy.count(), 1);
    QTRY_COMPARE(changeProcessedSpy.count(), 1);
    QCOMPARE(processorErrorSpy.takeFirst().at(0).toString(), QStringLiteral("processor boom"));
    QCOMPARE(changeProcessedSpy.takeFirst().at(0).toString(), QStringLiteral("docs/processed.txt"));

    SyncActionItem actionItem;
    actionItem.actionType = SyncActionType::Download;
    actionItem.localPath = QStringLiteral("docs/download.txt");
    actionItem.fileId = QStringLiteral("file-2");

    QMetaObject::invokeMethod(
        m_syncActionThread,
        [this, actionItem]() {
            m_syncActionThread->emitActionCompletedSignal(actionItem);
            m_syncActionThread->emitActionProgressSignal(actionItem, 32, 64);
            m_syncActionThread->emitActionFailedSignal(actionItem, QStringLiteral("action failed"));
            m_syncActionThread->emitErrorSignal(QStringLiteral("worker error"));
        },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(syncActionCompletedSpy.count(), 1);
    QTRY_COMPARE(syncActionProgressSpy.count(), 1);
    QTRY_COMPARE(syncActionFailedSpy.count(), 1);
    QTRY_COMPARE(syncActionErrorSpy.count(), 1);
    QCOMPARE(m_runtime->lastSyncActionError(), QStringLiteral("worker error"));

    QMetaObject::invokeMethod(
        m_fullSync,
        [this]() {
            m_fullSync->emitStateChangedSignal(FullSync::State::FetchingRemote);
            m_fullSync->emitProgressUpdatedSignal(QStringLiteral("Fetching remote files..."), 3, 9);
            m_fullSync->emitCompletedSignal(4, 7);
            m_fullSync->emitErrorSignal(QStringLiteral("full sync error"));
        },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(fullSyncStateSpy.count(), 1);
    QTRY_COMPARE(fullSyncProgressSpy.count(), 1);
    QTRY_COMPARE(fullSyncCompletedSpy.count(), 1);
    QTRY_COMPARE(fullSyncErrorSpy.count(), 1);

    QCOMPARE(m_runtime->fullSyncPhase(), QStringLiteral("Fetching remote files..."));
    QCOMPARE(m_runtime->fullSyncProgressCurrent(), 3);
    QCOMPARE(m_runtime->fullSyncProgressTotal(), 9);
    QCOMPARE(m_runtime->lastFullSyncLocalCount(), 4);
    QCOMPARE(m_runtime->lastFullSyncRemoteCount(), 7);
    QCOMPARE(m_runtime->lastFullSyncError(), QStringLiteral("full sync error"));
}

void TestMirrorSyncRuntime::testInitialSyncDefersRemoteWatcherUntilFolderAuthorityReady() {
    m_runtime->setChangeToken(QStringLiteral("token-initial"));

    m_runtime->startAndScheduleInitialSync(0);

    QTRY_COMPARE(m_changeProcessor->state(), ChangeProcessor::State::Running);
    QTRY_COMPARE(m_remoteWatcher->state(), RemoteChangeWatcher::State::Stopped);

    QMetaObject::invokeMethod(
        m_fullSync,
        [this]() {
            QHash<QString, QString> folderMap;
            folderMap.insert(QStringLiteral("folder-1"), QStringLiteral("docs"));
            m_fullSync->emitRemoteFolderMapReadySignal(folderMap);
        },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(m_remoteWatcher->folderIdToPath().value(QStringLiteral("folder-1")),
                 QStringLiteral("docs"));
    QCOMPARE(m_remoteWatcher->state(), RemoteChangeWatcher::State::Stopped);

    QMetaObject::invokeMethod(
        m_fullSync, [this]() { m_fullSync->emitCompletedSignal(1, 1); },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(m_remoteWatcher->state(), RemoteChangeWatcher::State::Running);
}

void TestMirrorSyncRuntime::testRestartAfterWakeDefersRemoteWatcherUntilFolderAuthorityReady() {
    m_runtime->setChangeToken(QStringLiteral("token-restart"));
    m_runtime->start();

    QTRY_COMPARE(m_remoteWatcher->state(), RemoteChangeWatcher::State::Running);

    QMetaObject::invokeMethod(
        m_remoteWatcher,
        [this]() {
            QHash<QString, QString> staleMap;
            staleMap.insert(QStringLiteral("stale-folder"), QStringLiteral("stale/path"));
            m_remoteWatcher->setFolderIdToPath(staleMap);
        },
        Qt::BlockingQueuedConnection);

    m_runtime->restartAfterWake(0);

    QTRY_COMPARE(m_remoteWatcher->state(), RemoteChangeWatcher::State::Stopped);

    QMetaObject::invokeMethod(
        m_fullSync,
        [this]() {
            QHash<QString, QString> folderMap;
            folderMap.insert(QStringLiteral("fresh-folder"), QStringLiteral("fresh/path"));
            m_fullSync->emitRemoteFolderMapReadySignal(folderMap);
        },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(m_remoteWatcher->folderIdToPath().value(QStringLiteral("fresh-folder")),
                 QStringLiteral("fresh/path"));
    QVERIFY(!m_remoteWatcher->folderIdToPath().contains(QStringLiteral("stale-folder")));
    QCOMPARE(m_remoteWatcher->state(), RemoteChangeWatcher::State::Stopped);

    QMetaObject::invokeMethod(
        m_fullSync, [this]() { m_fullSync->emitCompletedSignal(2, 3); },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(m_remoteWatcher->state(), RemoteChangeWatcher::State::Running);
}

void TestMirrorSyncRuntime::testClearSessionStateResetsRuntimeCaches() {
    const QString localPath = QStringLiteral("docs/conflict-clear.txt");
    const QString fileId = QStringLiteral("file-clear");
    const int conflictId = m_syncDatabase->upsertConflictRecord(localPath, fileId);
    QVERIFY(conflictId >= 0);

    ConflictVersion version;
    version.localModifiedTime = QDateTime::currentDateTimeUtc();
    version.remoteModifiedTime = version.localModifiedTime.addSecs(5);
    version.dbSyncTime = version.localModifiedTime.addSecs(-5);
    m_syncDatabase->addConflictVersion(conflictId, version);

    QMetaObject::invokeMethod(
        m_changeProcessor, [this]() { m_changeProcessor->rehydrateUnresolvedConflicts(); },
        Qt::BlockingQueuedConnection);
    QTRY_COMPARE(m_runtime->unresolvedConflictCount(), 1);

    m_runtime->setChangeToken(QStringLiteral("token-1"));

    SyncActionItem actionItem;
    actionItem.actionType = SyncActionType::Upload;
    actionItem.localPath = QStringLiteral("docs/cache.txt");

    QMetaObject::invokeMethod(
        m_syncActionThread,
        [this]() { m_syncActionThread->emitErrorSignal(QStringLiteral("stale action error")); },
        Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(
        m_fullSync,
        [this]() {
            m_fullSync->emitProgressUpdatedSignal(QStringLiteral("Scanning local files..."), 2, 5);
            m_fullSync->emitCompletedSignal(2, 6);
            m_fullSync->emitErrorSignal(QStringLiteral("stale full-sync error"));
        },
        Qt::BlockingQueuedConnection);

    QTRY_COMPARE(m_runtime->lastSyncActionError(), QStringLiteral("stale action error"));
    QTRY_COMPARE(m_runtime->fullSyncPhase(), QStringLiteral("Scanning local files..."));
    QTRY_COMPARE(m_runtime->lastFullSyncLocalCount(), 2);
    QTRY_COMPARE(m_runtime->lastFullSyncRemoteCount(), 6);
    QTRY_COMPARE(m_runtime->lastFullSyncError(), QStringLiteral("stale full-sync error"));

    m_runtime->clearSessionState();

    QCOMPARE(m_runtime->unresolvedConflictCount(), 0);
    QVERIFY(m_remoteWatcher->changeToken().isEmpty());
    QVERIFY(m_runtime->lastSyncActionError().isEmpty());
    QVERIFY(m_runtime->fullSyncPhase().isEmpty());
    QCOMPARE(m_runtime->fullSyncProgressCurrent(), 0);
    QCOMPARE(m_runtime->fullSyncProgressTotal(), 0);
    QCOMPARE(m_runtime->lastFullSyncLocalCount(), 0);
    QCOMPARE(m_runtime->lastFullSyncRemoteCount(), 0);
    QVERIFY(m_runtime->lastFullSyncError().isEmpty());
}

QTEST_MAIN(TestMirrorSyncRuntime)
#include "TestMirrorSyncRuntime.moc"