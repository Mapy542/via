/**
 * @file TestUiStatusCoordinator.cpp
 * @brief Focused regression tests for shared UI status coordination.
 */

#include <QtTest/QtTest>

#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncActionQueue.h"
#include "ui/UiStatusCoordinator.h"

class TestUiStatusCoordinator : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testFuseStatusSurvivesMirrorRefresh();
    void testMetadataRefreshLifecycleReturnsToMounted();
    void testMetadataRefreshFailureReturnsToMounted();
    void testMetadataRefreshClearsOnLogoutAndAuthExpired();
    void testStickyMirrorStatusSurvivesRefresh();
    void testLoggedOutAndAuthExpiredStayDistinct();

   private:
    ChangeQueue* m_changeQueue = nullptr;
    SyncActionQueue* m_syncActionQueue = nullptr;
    ChangeProcessor* m_changeProcessor = nullptr;
    RuntimePauseController* m_pauseController = nullptr;
    UiStatusCoordinator* m_coordinator = nullptr;
};

void TestUiStatusCoordinator::init() {
    m_changeQueue = new ChangeQueue();
    m_syncActionQueue = new SyncActionQueue();
    m_changeProcessor = new ChangeProcessor(m_changeQueue, m_syncActionQueue, nullptr, nullptr);
    m_pauseController = new RuntimePauseController();
    m_coordinator =
        new UiStatusCoordinator(nullptr, m_syncActionQueue, m_changeProcessor, m_pauseController);
}

void TestUiStatusCoordinator::cleanup() {
    delete m_coordinator;
    m_coordinator = nullptr;

    delete m_changeProcessor;
    m_changeProcessor = nullptr;

    delete m_pauseController;
    m_pauseController = nullptr;

    delete m_syncActionQueue;
    m_syncActionQueue = nullptr;

    delete m_changeQueue;
    m_changeQueue = nullptr;
}

void TestUiStatusCoordinator::testFuseStatusSurvivesMirrorRefresh() {
    m_changeProcessor->start();
    m_coordinator->refreshMirrorStatus();

    UiStatusSnapshot status = m_coordinator->snapshot();
    QCOMPARE(status.mirrorStatusText, QStringLiteral("Up to date"));

    m_coordinator->onUploadStarted(QStringLiteral("file-1"), QStringLiteral("/report.txt"));
    status = m_coordinator->snapshot();
    QCOMPARE(status.fuseStatusText, QStringLiteral("Uploading..."));
    QCOMPARE(static_cast<int>(status.resolvedPriority),
             static_cast<int>(UiStatusPriority::Syncing));

    m_coordinator->refreshMirrorStatus();
    status = m_coordinator->snapshot();
    QCOMPARE(status.fuseStatusText, QStringLiteral("Uploading..."));
    QVERIFY(status.combinedStatusText.contains(QStringLiteral("Uploading...")));
}

void TestUiStatusCoordinator::testMetadataRefreshLifecycleReturnsToMounted() {
    m_coordinator->updateAuthState(true);
    m_coordinator->updateFuseStatus(QStringLiteral("Mounted"));

    m_coordinator->onMetadataRefreshStarted();

    UiStatusSnapshot status = m_coordinator->snapshot();
    QCOMPARE(status.fuseStatusText, QStringLiteral("Refreshing metadata"));
    QCOMPARE(static_cast<int>(status.resolvedPriority),
             static_cast<int>(UiStatusPriority::Syncing));

    m_coordinator->onMetadataRefreshFinished();

    QTRY_COMPARE(m_coordinator->snapshot().fuseStatusText, QStringLiteral("Mounted"));
}

void TestUiStatusCoordinator::testMetadataRefreshFailureReturnsToMounted() {
    m_coordinator->updateAuthState(true);
    m_coordinator->updateFuseStatus(QStringLiteral("Mounted"));

    m_coordinator->onMetadataRefreshStarted();
    m_coordinator->onMetadataRefreshFailed(QStringLiteral("network unavailable"));

    QTRY_COMPARE(m_coordinator->snapshot().fuseStatusText, QStringLiteral("Mounted"));
}

void TestUiStatusCoordinator::testMetadataRefreshClearsOnLogoutAndAuthExpired() {
    m_coordinator->updateAuthState(true);
    m_coordinator->updateFuseStatus(QStringLiteral("Mounted"));

    m_coordinator->onMetadataRefreshStarted();
    QCOMPARE(m_coordinator->snapshot().fuseStatusText, QStringLiteral("Refreshing metadata"));

    m_coordinator->updateAuthState(false);

    UiStatusSnapshot status = m_coordinator->snapshot();
    QCOMPARE(status.fuseStatusText, QStringLiteral("Idle"));
    QCOMPARE(status.combinedStatusText, QStringLiteral("Not connected"));

    m_coordinator->updateAuthState(true);
    m_coordinator->updateFuseStatus(QStringLiteral("Mounted"));
    m_coordinator->onMetadataRefreshStarted();
    m_coordinator->setAuthExpired(QStringLiteral("refresh token revoked"));

    status = m_coordinator->snapshot();
    QCOMPARE(status.fuseStatusText, QStringLiteral("Idle"));
    QVERIFY(status.combinedStatusText.contains(QStringLiteral("Authentication expired")));
}

void TestUiStatusCoordinator::testStickyMirrorStatusSurvivesRefresh() {
    m_changeProcessor->start();

    m_coordinator->updateMirrorStatus(QStringLiteral("Offline"));
    m_coordinator->refreshMirrorStatus();

    UiStatusSnapshot status = m_coordinator->snapshot();
    QCOMPARE(status.mirrorStatusText, QStringLiteral("Offline"));
    QCOMPARE(static_cast<int>(status.resolvedPriority),
             static_cast<int>(UiStatusPriority::Offline));

    m_coordinator->updateMirrorStatus(QStringLiteral("Syncing..."));
    m_coordinator->refreshMirrorStatus();

    status = m_coordinator->snapshot();
    QCOMPARE(status.mirrorStatusText, QStringLiteral("Up to date"));
}

void TestUiStatusCoordinator::testLoggedOutAndAuthExpiredStayDistinct() {
    m_coordinator->updateAuthState(true);
    m_changeProcessor->start();
    m_coordinator->refreshMirrorStatus();

    m_pauseController->setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline,
                                                true);
    UiStatusSnapshot status = m_coordinator->snapshot();
    QCOMPARE(status.mirrorStatusText, QStringLiteral("Offline"));
    QCOMPARE(static_cast<int>(status.resolvedPriority),
             static_cast<int>(UiStatusPriority::Offline));

    m_coordinator->updateAuthState(false);
    status = m_coordinator->snapshot();
    QCOMPARE(status.combinedStatusText, QStringLiteral("Not connected"));
    QVERIFY2(static_cast<int>(status.resolvedPriority) !=
                 static_cast<int>(UiStatusPriority::AuthExpired),
             qPrintable(QStringLiteral("unexpected priority=%1")
                            .arg(static_cast<int>(status.resolvedPriority))));

    m_pauseController->setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline,
                                                false);
    m_pauseController->setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline,
                                                true);
    status = m_coordinator->snapshot();
    QCOMPARE(status.combinedStatusText, QStringLiteral("Not connected"));

    m_coordinator->setAuthExpired(QStringLiteral("refresh token revoked"));
    status = m_coordinator->snapshot();
    QVERIFY(status.combinedStatusText.contains(QStringLiteral("Authentication expired")));
    QCOMPARE(static_cast<int>(status.resolvedPriority),
             static_cast<int>(UiStatusPriority::AuthExpired));

    m_coordinator->updateAuthState(true);
    status = m_coordinator->snapshot();
    QVERIFY2(static_cast<int>(status.resolvedPriority) !=
                 static_cast<int>(UiStatusPriority::AuthExpired),
             qPrintable(QStringLiteral("unexpected priority=%1")
                            .arg(static_cast<int>(status.resolvedPriority))));
}

QTEST_MAIN(TestUiStatusCoordinator)
#include "TestUiStatusCoordinator.moc"