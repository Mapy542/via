/**
 * @file TestRemoteChangeWatcher.cpp
 * @brief Unit tests for RemoteChangeWatcher subsystem
 *
 * Tests cover: lifecycle, change token management, change interpretation,
 * skip logic, and error handling.
 */

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/DriveChange.h"
#include "api/DriveFile.h"
#include "api/GoogleDriveClient.h"
#include "sync/ChangeQueue.h"
#include "sync/FileFilter.h"
#include "sync/RemoteChangeWatcher.h"
#include "sync/SyncDatabase.h"
#include "sync/SyncSettings.h"

// ---------------------------------------------------------------------------
//  Fake Drive client used by the watcher tests
// ---------------------------------------------------------------------------
class FakeDriveClientForRCW : public GoogleDriveClient {
    Q_OBJECT

   public:
    struct InjectedError {
        QString errorMsg;
        int remaining = 1;
        int delayMs = 0;
    };

    explicit FakeDriveClientForRCW(QObject* parent = nullptr)
        : GoogleDriveClient(nullptr, parent) {}

    // Record calls
    int listChangesCallCount = 0;
    QString lastListChangesToken;
    int getStartPageTokenCallCount = 0;

    void listChanges(const QString& startPageToken = QString()) override {
        ++listChangesCallCount;
        lastListChangesToken = startPageToken;
        if (emitInjectedError(QStringLiteral("listChanges"))) {
            return;
        }
        // Emit results asynchronously so caller returns first
        // hasMorePages=false means we're caught up
        QTimer::singleShot(
            0, this, [this]() { emit changesReceived(m_pendingChanges, m_nextToken, false); });
    }

    void getStartPageToken() override {
        ++getStartPageTokenCallCount;
        if (emitInjectedError(QStringLiteral("getStartPageToken"))) {
            return;
        }
        QTimer::singleShot(0, this, [this]() { emit startPageTokenReceived(m_startPageToken); });
    }

    QString getRootFolderId() override { return m_rootFolderId; }

    DriveFile getFileMetadataBlocking(const QString& fileId) override {
        if (m_fileMetadata.contains(fileId)) {
            return m_fileMetadata.value(fileId);
        }
        return DriveFile();
    }

    // Helpers to configure fake responses
    void setStartPageToken(const QString& token) { m_startPageToken = token; }
    void setNextToken(const QString& token) { m_nextToken = token; }
    void setPendingChanges(const QList<DriveChange>& changes) { m_pendingChanges = changes; }
    void setRootFolderId(const QString& id) { m_rootFolderId = id; }
    void addFileMetadata(const DriveFile& file) { m_fileMetadata.insert(file.id, file); }
    void injectOperationError(const QString& operation, const QString& errorMsg, int remaining = 1,
                              int delayMs = 0) {
        m_injectedErrors.insert(operation, InjectedError{errorMsg, remaining, delayMs});
    }

    void emitError(const QString& operation, const QString& errorMsg) {
        emit error(operation, errorMsg);
    }

   private:
    bool emitInjectedError(const QString& operation) {
        if (!m_injectedErrors.contains(operation)) {
            return false;
        }

        InjectedError injected = m_injectedErrors.value(operation);
        QTimer::singleShot(
            injected.delayMs, this,
            [this, operation, errorMsg = injected.errorMsg]() { emit error(operation, errorMsg); });

        injected.remaining -= 1;
        if (injected.remaining <= 0) {
            m_injectedErrors.remove(operation);
        } else {
            m_injectedErrors.insert(operation, injected);
        }

        return true;
    }

    QString m_startPageToken = "start-token-1";
    QString m_nextToken = "next-token-1";
    QList<DriveChange> m_pendingChanges;
    QString m_rootFolderId = "root-id";
    QHash<QString, DriveFile> m_fileMetadata;
    QHash<QString, InjectedError> m_injectedErrors;
};

// ---------------------------------------------------------------------------
//  Test class
// ---------------------------------------------------------------------------
class TestRemoteChangeWatcher : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // Lifecycle
    void testStart();
    void testStop();
    void testPause();
    void testResume();

    // Change token management
    void testChangeTokenPersistence();
    void testChangeTokenExpiry();
    void testEmptyChangeResponse();

    // Change interpretation
    void testInterpretDeleteChange_Trashed();
    void testInterpretDeleteChange_Removed();
    void testInterpretModifyChange();
    void testInterpretModifyChange_TrashPathDropped();
    void testInterpretModifyChange_ReusesExistingDuplicateAlias();
    void testInterpretModifyChange_UsesDuplicateFolderAliasForChild();
    void testInterpretModifyChange_DisambiguatesAgainstLocalFile();
    void testInterpretModifyChange_NativeDocRepresentationModes_data();
    void testInterpretModifyChange_NativeDocRepresentationModes();
    void testInterpretModifyChange_NativeDocReusesVisibleAlias();

    // Skip logic
    void testGenericFilterAllowsGoogleDoc();
    void testSkipNotOwnedFile();
    void testDontSkipNormalFile();
    void testDontSkipTrashedFile();

    // Token ordering (token committed after processing)
    void testTokenCommittedAfterProcessing();

    // Polling
    void testPollForChanges();

    // Error handling
    void testApiErrorEmitsErrorSignal();
    void testTransientApiErrorRetriesWithoutErrorSignal();

    // In-flight dedup
    void testInFlightDedup();
    void testDeferredCheckDuringProcessing();

   private:
    QTemporaryDir* m_tempDir = nullptr;
    ChangeQueue* m_changeQueue = nullptr;
    SyncDatabase* m_syncDatabase = nullptr;
    FakeDriveClientForRCW* m_driveClient = nullptr;
    RemoteChangeWatcher* m_watcher = nullptr;
    QByteArray m_originalHome;
};

void TestRemoteChangeWatcher::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    m_originalHome = qgetenv("HOME");
    qputenv("HOME", m_tempDir->path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);

    const QString syncDir = m_tempDir->filePath("sync");
    QDir().mkpath(syncDir);

    QSettings settings;
    settings.setValue("sync/folder", syncDir);
    settings.setValue("sync/duplicateNameStrategy", "file-id-suffix");
    settings.sync();

    m_syncDatabase = new SyncDatabase();
    QVERIFY(m_syncDatabase->initialize());

    m_changeQueue = new ChangeQueue();
    m_driveClient = new FakeDriveClientForRCW();
    m_watcher = new RemoteChangeWatcher(m_changeQueue, m_driveClient, m_syncDatabase);
}

void TestRemoteChangeWatcher::cleanup() {
    delete m_watcher;
    m_watcher = nullptr;
    delete m_driveClient;
    m_driveClient = nullptr;
    delete m_changeQueue;
    m_changeQueue = nullptr;
    delete m_syncDatabase;
    m_syncDatabase = nullptr;

    qputenv("HOME", m_originalHome);
    QStandardPaths::setTestModeEnabled(false);

    delete m_tempDir;
    m_tempDir = nullptr;
}

// ---------------------------------------------------------------------------
//  Lifecycle tests
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testStart() {
    // Set a token so start doesn't need to fetch one
    m_watcher->setChangeToken("token-1");

    QSignalSpy stateSpy(m_watcher, &RemoteChangeWatcher::stateChanged);
    m_watcher->start();

    QCOMPARE(m_watcher->state(), RemoteChangeWatcher::State::Running);
    QVERIFY(stateSpy.count() >= 1);
}

void TestRemoteChangeWatcher::testStop() {
    m_watcher->setChangeToken("token-1");
    m_watcher->start();

    QSignalSpy stateSpy(m_watcher, &RemoteChangeWatcher::stateChanged);
    m_watcher->stop();

    QCOMPARE(m_watcher->state(), RemoteChangeWatcher::State::Stopped);
    QVERIFY(stateSpy.count() >= 1);
}

void TestRemoteChangeWatcher::testPause() {
    m_watcher->setChangeToken("token-1");
    m_watcher->start();

    m_watcher->pause();
    QCOMPARE(m_watcher->state(), RemoteChangeWatcher::State::Paused);
}

void TestRemoteChangeWatcher::testResume() {
    m_watcher->setChangeToken("token-1");
    m_watcher->start();
    m_watcher->pause();
    QCOMPARE(m_watcher->state(), RemoteChangeWatcher::State::Paused);

    m_watcher->resume();
    QCOMPARE(m_watcher->state(), RemoteChangeWatcher::State::Running);
}

// ---------------------------------------------------------------------------
//  Change token management
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testChangeTokenPersistence() {
    m_watcher->setChangeToken("my-token-42");
    QCOMPARE(m_watcher->changeToken(), QString("my-token-42"));
}

void TestRemoteChangeWatcher::testChangeTokenExpiry() {
    // When no token is set, start() should request one via getStartPageToken()
    QVERIFY(m_watcher->changeToken().isEmpty());

    m_driveClient->setStartPageToken("fresh-token");
    m_watcher->start();

    // Process events so the async getStartPageToken fires
    QTRY_COMPARE(m_driveClient->getStartPageTokenCallCount, 1);
    QCoreApplication::processEvents();
    QTRY_COMPARE(m_watcher->changeToken(), QString("fresh-token"));
}

void TestRemoteChangeWatcher::testEmptyChangeResponse() {
    m_watcher->setChangeToken("token-1");
    m_driveClient->setPendingChanges({});  // no changes
    m_driveClient->setNextToken("token-2");

    m_watcher->start();

    QSignalSpy tokenSpy(m_watcher, &RemoteChangeWatcher::changeTokenUpdated);

    m_watcher->checkNow();
    QTRY_VERIFY(tokenSpy.count() >= 1);

    // Token should be updated even with empty list
    QCOMPARE(m_watcher->changeToken(), QString("token-2"));
    // No items enqueued
    QVERIFY(m_changeQueue->isEmpty());
}

// ---------------------------------------------------------------------------
//  Change interpretation
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testInterpretDeleteChange_Trashed() {
    // Set up: file known in DB so path can be resolved
    m_syncDatabase->setFileId("docs/report.txt", "file-1");

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveFile trashedFile;
    trashedFile.id = "file-1";
    trashedFile.name = "report.txt";
    trashedFile.trashed = true;
    trashedFile.mimeType = "text/plain";
    trashedFile.ownedByMe = true;
    trashedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-1";
    change.fileId = "file-1";
    change.removed = false;
    change.file = trashedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});
    m_watcher->start();

    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->checkNow();
    QTRY_VERIFY(changeSpy.count() >= 1);

    // Should be enqueued as Delete
    QVERIFY(!m_changeQueue->isEmpty());
    ChangeQueueItem item = m_changeQueue->dequeue();
    QCOMPARE(item.changeType, ChangeType::Delete);
    QCOMPARE(item.fileId, QString("file-1"));
    QCOMPARE(item.origin, ChangeOrigin::Remote);
}

void TestRemoteChangeWatcher::testInterpretDeleteChange_Removed() {
    m_syncDatabase->setFileId("docs/old.txt", "file-2");

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveChange change;
    change.changeId = "change-2";
    change.fileId = "file-2";
    change.removed = true;
    change.time = QDateTime::currentDateTimeUtc();
    change.file.mimeType = "application/octet-stream";
    change.file.ownedByMe = true;

    m_driveClient->setPendingChanges({change});
    m_watcher->start();

    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->checkNow();
    QTRY_VERIFY(changeSpy.count() >= 1);

    ChangeQueueItem item = m_changeQueue->dequeue();
    QCOMPARE(item.changeType, ChangeType::Delete);
    QCOMPARE(item.origin, ChangeOrigin::Remote);
}

void TestRemoteChangeWatcher::testInterpretModifyChange() {
    // Set folder mapping so path can be resolved
    QHash<QString, QString> folderMap;
    folderMap.insert("root-id", "");
    m_watcher->setFolderIdToPath(folderMap);

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveFile modifiedFile;
    modifiedFile.id = "file-3";
    modifiedFile.name = "hello.txt";
    modifiedFile.mimeType = "text/plain";
    modifiedFile.ownedByMe = true;
    modifiedFile.parents = {"root-id"};
    modifiedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-3";
    change.fileId = "file-3";
    change.removed = false;
    change.file = modifiedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});
    m_watcher->start();

    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->checkNow();
    QTRY_VERIFY(changeSpy.count() >= 1);

    ChangeQueueItem item = m_changeQueue->dequeue();
    QCOMPARE(item.changeType, ChangeType::Modify);
    QCOMPARE(item.localPath, QString("hello.txt"));
    QCOMPARE(item.origin, ChangeOrigin::Remote);
}

void TestRemoteChangeWatcher::testInterpretModifyChange_TrashPathDropped() {
    QHash<QString, QString> folderMap;
    folderMap.insert("root-id", "");
    m_watcher->setFolderIdToPath(folderMap);

    m_watcher->setChangeToken("token-1");
    m_driveClient->setRootFolderId(QStringLiteral("root-id"));
    m_driveClient->setNextToken("token-2");

    DriveFile modifiedFile;
    modifiedFile.id = "trash-folder-1";
    modifiedFile.name = ".Trash-1000";
    modifiedFile.mimeType = "application/vnd.google-apps.folder";
    modifiedFile.isFolder = true;
    modifiedFile.ownedByMe = true;
    modifiedFile.parents = {"root-id"};
    modifiedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-trash-1";
    change.fileId = modifiedFile.id;
    change.removed = false;
    change.file = modifiedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});
    m_watcher->start();
    m_watcher->checkNow();

    QTRY_VERIFY_WITH_TIMEOUT(m_changeQueue->isEmpty(), 2000);
}

void TestRemoteChangeWatcher::testInterpretModifyChange_ReusesExistingDuplicateAlias() {
    m_syncDatabase->setFileId("hello_file-3.txt", "file-3");

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveFile modifiedFile;
    modifiedFile.id = "file-3";
    modifiedFile.name = "hello.txt";
    modifiedFile.mimeType = "text/plain";
    modifiedFile.ownedByMe = true;
    modifiedFile.parents = {"root-id"};
    modifiedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-3b";
    change.fileId = "file-3";
    change.removed = false;
    change.file = modifiedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});
    m_watcher->start();

    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->checkNow();
    QTRY_VERIFY(changeSpy.count() >= 1);

    ChangeQueueItem item = m_changeQueue->dequeue();
    QCOMPARE(item.localPath, QString("hello_file-3.txt"));
}

void TestRemoteChangeWatcher::testInterpretModifyChange_UsesDuplicateFolderAliasForChild() {
    m_syncDatabase->setFileId("docs", "folder-1");
    m_syncDatabase->setFileId("docs_folder-2", "folder-2");

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveFile modifiedFile;
    modifiedFile.id = "child-file";
    modifiedFile.name = "note.txt";
    modifiedFile.mimeType = "text/plain";
    modifiedFile.ownedByMe = true;
    modifiedFile.parents = {"folder-2"};
    modifiedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-folder-child";
    change.fileId = "child-file";
    change.removed = false;
    change.file = modifiedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});
    m_watcher->start();

    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->checkNow();
    QTRY_VERIFY(changeSpy.count() >= 1);

    ChangeQueueItem item = m_changeQueue->dequeue();
    QCOMPARE(item.localPath, QString("docs_folder-2/note.txt"));
}

void TestRemoteChangeWatcher::testInterpretModifyChange_DisambiguatesAgainstLocalFile() {
    QSettings settings;
    settings.setValue("sync/duplicateNameStrategy", "numeric-suffix");
    settings.sync();

    QFile existingFile(m_tempDir->filePath("sync/hello.txt"));
    QVERIFY(existingFile.open(QIODevice::WriteOnly));
    existingFile.write("local");
    existingFile.close();

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveFile modifiedFile;
    modifiedFile.id = "file-4";
    modifiedFile.name = "hello.txt";
    modifiedFile.mimeType = "text/plain";
    modifiedFile.ownedByMe = true;
    modifiedFile.parents = {"root-id"};
    modifiedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-3c";
    change.fileId = "file-4";
    change.removed = false;
    change.file = modifiedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});
    m_watcher->start();

    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->checkNow();
    QTRY_VERIFY(changeSpy.count() >= 1);

    ChangeQueueItem item = m_changeQueue->dequeue();
    QCOMPARE(item.localPath, QString("hello (1).txt"));
}

void TestRemoteChangeWatcher::testInterpretModifyChange_NativeDocRepresentationModes_data() {
    QTest::addColumn<QString>("globalMode");
    QTest::addColumn<QString>("overrideMode");
    QTest::addColumn<QString>("expectedPath");
    QTest::addColumn<bool>("expectQueued");

    QTest::newRow("hide") << QString("hide") << QString() << QString() << false;
    QTest::newRow("browser-shortcut")
        << QString("browser-shortcut") << QString() << QString("Quarterly.gdoc") << true;
    QTest::newRow("open-document")
        << QString("open-document") << QString() << QString("Quarterly.odt") << true;
    QTest::newRow("text") << QString("text") << QString() << QString("Quarterly.md") << true;
    QTest::newRow("override-hide")
        << QString("browser-shortcut") << QString("hide") << QString() << false;
    QTest::newRow("override-text")
        << QString("browser-shortcut") << QString("text") << QString("Quarterly.md") << true;
}

void TestRemoteChangeWatcher::testInterpretModifyChange_NativeDocRepresentationModes() {
    QFETCH(QString, globalMode);
    QFETCH(QString, overrideMode);
    QFETCH(QString, expectedPath);
    QFETCH(bool, expectQueued);

    QSettings settings;
    settings.setValue("advanced/nativeDocMode", globalMode);
    settings.sync();

    if (!overrideMode.isEmpty()) {
        NativeDocState state;
        state.fileId = "doc-1";
        state.nativeDocModeOverride = overrideMode;
        QVERIFY(m_syncDatabase->saveNativeDocState(state));
    }

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveFile modifiedFile;
    modifiedFile.id = "doc-1";
    modifiedFile.name = "Quarterly";
    modifiedFile.mimeType = "application/vnd.google-apps.document";
    modifiedFile.webViewLink = "https://docs.google.com/document/d/doc-1/edit";
    modifiedFile.ownedByMe = true;
    modifiedFile.parents = {"root-id"};
    modifiedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-native";
    change.fileId = "doc-1";
    change.removed = false;
    change.file = modifiedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});

    QSignalSpy tokenSpy(m_watcher, &RemoteChangeWatcher::changeTokenUpdated);
    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->start();

    QTRY_VERIFY(tokenSpy.count() >= 1);
    QCOMPARE(m_watcher->changeToken(), QString("token-2"));

    QString resolvedPath;
    while (!m_changeQueue->isEmpty()) {
        const ChangeQueueItem item = m_changeQueue->dequeue();
        if (item.fileId == QStringLiteral("doc-1")) {
            resolvedPath = item.localPath;
        }
    }

    if (expectQueued) {
        QCOMPARE(changeSpy.count(), 1);
        QCOMPARE(resolvedPath, expectedPath);
    } else {
        QCOMPARE(changeSpy.count(), 0);
        QVERIFY(resolvedPath.isEmpty());
    }

    const NativeDocState stored = m_syncDatabase->getNativeDocState("doc-1");
    QVERIFY(stored.isValid());
    QCOMPARE(stored.remoteName, QString("Quarterly"));
    QCOMPARE(stored.remoteMimeType, QString("application/vnd.google-apps.document"));
    QCOMPARE(stored.webViewLink, QString("https://docs.google.com/document/d/doc-1/edit"));
    QCOMPARE(stored.nativeDocModeOverride, overrideMode);
}

void TestRemoteChangeWatcher::testInterpretModifyChange_NativeDocReusesVisibleAlias() {
    QSettings settings;
    settings.setValue("advanced/nativeDocMode", "browser-shortcut");
    settings.sync();

    m_syncDatabase->setFileId("Quarterly_file-3.gdoc", "file-3");

    m_watcher->setChangeToken("token-1");
    m_driveClient->setNextToken("token-2");

    DriveFile modifiedFile;
    modifiedFile.id = "file-3";
    modifiedFile.name = "Quarterly";
    modifiedFile.mimeType = "application/vnd.google-apps.document";
    modifiedFile.webViewLink = "https://docs.google.com/document/d/file-3/edit";
    modifiedFile.ownedByMe = true;
    modifiedFile.parents = {"root-id"};
    modifiedFile.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-native-alias";
    change.fileId = "file-3";
    change.removed = false;
    change.file = modifiedFile;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});

    QSignalSpy changeSpy(m_watcher, &RemoteChangeWatcher::changeDetected);
    m_watcher->start();

    QTRY_VERIFY(changeSpy.count() >= 1);

    const ChangeQueueItem item = m_changeQueue->dequeue();
    QCOMPARE(item.localPath, QString("Quarterly_file-3.gdoc"));
}

// ---------------------------------------------------------------------------
//  Skip logic (FileFilter)
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testGenericFilterAllowsGoogleDoc() {
    SyncSettings settings = SyncSettings::load();

    DriveFile googleDoc;
    googleDoc.mimeType = "application/vnd.google-apps.document";
    googleDoc.ownedByMe = true;

    QVERIFY(!FileFilter::shouldSkipRemoteFile(googleDoc, settings));
}

void TestRemoteChangeWatcher::testSkipNotOwnedFile() {
    SyncSettings settings = SyncSettings::load();

    DriveFile notOwned;
    notOwned.mimeType = "text/plain";
    notOwned.ownedByMe = false;

    QVERIFY(FileFilter::shouldSkipRemoteFile(notOwned, settings));
}

void TestRemoteChangeWatcher::testDontSkipNormalFile() {
    SyncSettings settings = SyncSettings::load();

    DriveFile normal;
    normal.mimeType = "text/plain";
    normal.ownedByMe = true;

    QVERIFY(!FileFilter::shouldSkipRemoteFile(normal, settings));
}

void TestRemoteChangeWatcher::testDontSkipTrashedFile() {
    SyncSettings settings = SyncSettings::load();

    DriveFile trashed;
    trashed.mimeType = "text/plain";
    trashed.ownedByMe = true;
    trashed.trashed = true;

    // Trashed files are NOT skipped - we need to detect deletions
    QVERIFY(!FileFilter::shouldSkipRemoteFile(trashed, settings));
}

// ---------------------------------------------------------------------------
//  Token ordering: token committed after processing
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testTokenCommittedAfterProcessing() {
    // Verify the fix: token should only be updated AFTER changes are processed
    QHash<QString, QString> folderMap;
    folderMap.insert("root-id", "");
    m_watcher->setFolderIdToPath(folderMap);

    m_watcher->setChangeToken("old-token");
    m_driveClient->setNextToken("new-token");

    DriveFile file;
    file.id = "file-tok";
    file.name = "tokentest.txt";
    file.mimeType = "text/plain";
    file.ownedByMe = true;
    file.parents = {"root-id"};
    file.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-tok";
    change.fileId = "file-tok";
    change.removed = false;
    change.file = file;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});
    m_watcher->start();

    QSignalSpy tokenSpy(m_watcher, &RemoteChangeWatcher::changeTokenUpdated);
    m_watcher->checkNow();

    QTRY_VERIFY(tokenSpy.count() >= 1);

    // Token updated to new value
    QCOMPARE(m_watcher->changeToken(), QString("new-token"));
    // And the change was actually processed (enqueued)
    QVERIFY(!m_changeQueue->isEmpty());
}

// ---------------------------------------------------------------------------
//  Polling
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testPollForChanges() {
    m_watcher->setChangeToken("token-1");
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken("token-2");

    m_watcher->start();
    // Override the default poll interval AFTER start (start() resets it from settings)
    m_watcher->setPollingInterval(50);

    // Wait for at least one poll to happen beyond the initial one
    QTRY_VERIFY_WITH_TIMEOUT(m_driveClient->listChangesCallCount >= 2, 2000);
}

// ---------------------------------------------------------------------------
//  Error handling
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testApiErrorEmitsErrorSignal() {
    m_watcher->setChangeToken("token-1");
    m_watcher->start();

    QSignalSpy errorSpy(m_watcher, &RemoteChangeWatcher::error);
    m_driveClient->emitError("listChanges", "network timeout");

    QTRY_VERIFY(errorSpy.count() >= 1);
}

void TestRemoteChangeWatcher::testTransientApiErrorRetriesWithoutErrorSignal() {
    m_watcher->setChangeToken("token-1");
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken("token-2");
    m_driveClient->injectOperationError(QStringLiteral("listChanges"),
                                        QStringLiteral("HTTP/2 protocol error"));

    QSignalSpy errorSpy(m_watcher, &RemoteChangeWatcher::error);
    QSignalSpy tokenSpy(m_watcher, &RemoteChangeWatcher::changeTokenUpdated);

    m_watcher->start();

    QTRY_COMPARE_WITH_TIMEOUT(m_driveClient->listChangesCallCount, 2, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(tokenSpy.count() >= 1, 1500);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(m_watcher->changeToken(), QStringLiteral("token-2"));
}

// ---------------------------------------------------------------------------
//  In-flight dedup
// ---------------------------------------------------------------------------
void TestRemoteChangeWatcher::testInFlightDedup() {
    // When a changes request is in flight, a second checkNow() should be deferred
    m_watcher->setChangeToken("token-1");
    m_driveClient->setPendingChanges({});
    m_driveClient->setNextToken("token-2");
    m_watcher->start();

    // First check starts a request
    m_watcher->checkNow();
    int countAfterFirst = m_driveClient->listChangesCallCount;
    // Second check while first is in flight should be deferred, not doubled
    m_watcher->checkNow();
    QCOMPARE(m_driveClient->listChangesCallCount, countAfterFirst);

    // Let the first request complete and the deferred one fire
    QTRY_VERIFY_WITH_TIMEOUT(m_driveClient->listChangesCallCount > countAfterFirst, 500);
}

void TestRemoteChangeWatcher::testDeferredCheckDuringProcessing() {
    QHash<QString, QString> folderMap;
    folderMap.insert("root-id", "");
    m_watcher->setFolderIdToPath(folderMap);

    m_watcher->setChangeToken("old-token");
    m_driveClient->setNextToken("new-token");

    DriveFile file;
    file.id = "file-race";
    file.name = "race.txt";
    file.mimeType = "text/plain";
    file.ownedByMe = true;
    file.parents = {"root-id"};
    file.modifiedTime = QDateTime::currentDateTimeUtc();

    DriveChange change;
    change.changeId = "change-race";
    change.fileId = "file-race";
    change.removed = false;
    change.file = file;
    change.time = QDateTime::currentDateTimeUtc();

    m_driveClient->setPendingChanges({change});

    bool injectedDeferredCheck = false;
    bool requestStayedDeferred = false;

    connect(
        m_watcher, &RemoteChangeWatcher::changeDetected, this,
        [this, &injectedDeferredCheck, &requestStayedDeferred]() {
            if (injectedDeferredCheck) {
                return;
            }

            injectedDeferredCheck = true;
            m_driveClient->setPendingChanges({});
            m_driveClient->setNextToken("new-token");
            m_watcher->checkNow();
            requestStayedDeferred = (m_driveClient->listChangesCallCount == 1);
        },
        Qt::DirectConnection);

    m_watcher->start();

    QTRY_VERIFY_WITH_TIMEOUT(injectedDeferredCheck, 500);
    QVERIFY(requestStayedDeferred);
    QTRY_VERIFY_WITH_TIMEOUT(m_driveClient->listChangesCallCount >= 2, 500);
    QCOMPARE(m_driveClient->listChangesCallCount, 2);
    QCOMPARE(m_driveClient->lastListChangesToken, QString("new-token"));
}

QTEST_MAIN(TestRemoteChangeWatcher)
#include "TestRemoteChangeWatcher.moc"
