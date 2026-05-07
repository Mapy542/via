/**
 * @file TestUiStatusIntegration.cpp
 * @brief UI integration tests for shared status rendering.
 */

#include <QImage>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncActionQueue.h"
#include "utils/ThemeHelper.h"

#define private public
#include "ui/MainWindow.h"
#include "ui/SystemTrayManager.h"
#undef private

#include "ui/UiStatusCoordinator.h"

class TestUiStatusIntegration : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testFuseUploadingStatusPropagatesToTrayAndWindow();
    void testMirrorStatusPropagatesWhenFuseDisabled();
    void testOfflineStatusPropagatesToTrayAndWindow();
    void testMirrorDisabledOfflineRecoveryReturnsToFuseStatus();
    void testMetadataRefreshLifecyclePropagatesWhenMirrorDisabled();
    void testAuthExpiredRecoveryPropagatesToTrayAndWindow();
    void testMainWindowSyncNowEmitsRequestSignal();
    void testPauseActionsTogglePauseController();
    void testBothSyncSystemsDisabledUsesPausedStatusAndDisablesActions();

   private:
    QImage trayIconImage(int size = 16) const;
    QImage windowIconImage() const;
    QString traySummary() const;
    QString trayTooltip() const;
    QString windowSummary() const;

    ChangeQueue* m_changeQueue = nullptr;
    SyncActionQueue* m_syncActionQueue = nullptr;
    ChangeProcessor* m_changeProcessor = nullptr;
    RuntimePauseController* m_pauseController = nullptr;
    UiStatusCoordinator* m_coordinator = nullptr;
    SystemTrayManager* m_tray = nullptr;
    MainWindow* m_window = nullptr;
};

void TestUiStatusIntegration::init() {
    m_changeQueue = new ChangeQueue();
    m_syncActionQueue = new SyncActionQueue();
    m_changeProcessor = new ChangeProcessor(m_changeQueue, m_syncActionQueue, nullptr, nullptr);
    m_pauseController = new RuntimePauseController();
    m_coordinator = new UiStatusCoordinator(nullptr, true, m_pauseController);
    m_tray = new SystemTrayManager(nullptr, m_pauseController, m_coordinator);
    m_window = new MainWindow(nullptr, nullptr, nullptr, m_pauseController, m_coordinator, nullptr);
}

void TestUiStatusIntegration::cleanup() {
    delete m_window;
    m_window = nullptr;

    delete m_tray;
    m_tray = nullptr;

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

QString TestUiStatusIntegration::traySummary() const {
    return m_tray->m_statusAction->text();
}

QImage TestUiStatusIntegration::trayIconImage(int size) const {
    return m_tray->trayIcon()->icon().pixmap(size, size).toImage();
}

QImage TestUiStatusIntegration::windowIconImage() const {
    return m_window->m_statusIcon->pixmap(Qt::ReturnByValue).toImage();
}

QString TestUiStatusIntegration::trayTooltip() const {
    return m_tray->trayIcon()->toolTip();
}

QString TestUiStatusIntegration::windowSummary() const {
    return m_window->m_statusLabel->text();
}

void TestUiStatusIntegration::testFuseUploadingStatusPropagatesToTrayAndWindow() {
    m_changeProcessor->start();
    m_coordinator->updateMirrorProcessorState(m_changeProcessor->state());
    m_coordinator->updateAuthState(true);
    m_coordinator->refreshMirrorStatus();

    QTRY_COMPARE(windowSummary(), QStringLiteral("Up to date"));
    QTRY_COMPARE(traySummary(), QStringLiteral("Up to date"));

    m_coordinator->onUploadStarted(QStringLiteral("file-1"), QStringLiteral("/report.txt"));

    const QString expected = QStringLiteral("Mirror: Up to date | FUSE: Uploading...");
    QTRY_COMPARE(windowSummary(), expected);
    QTRY_COMPARE(traySummary(), expected);
    QTRY_COMPARE(trayTooltip(), QStringLiteral("Via\n%1").arg(expected));

    m_coordinator->refreshMirrorStatus();

    QTRY_COMPARE(windowSummary(), expected);
    QTRY_COMPARE(traySummary(), expected);
}

void TestUiStatusIntegration::testMirrorStatusPropagatesWhenFuseDisabled() {
    m_coordinator->updateAuthState(true);
    m_coordinator->updateMirrorStatus(QStringLiteral("Scanning local files..."));

    const QString expected = QStringLiteral("Scanning local files...");
    QTRY_COMPARE(windowSummary(), expected);
    QTRY_COMPARE(traySummary(), expected);
    QTRY_COMPARE(trayTooltip(), QStringLiteral("Via\n%1").arg(expected));
    QTRY_COMPARE(trayIconImage(),
                 ThemeHelper::trayIcon(QStringLiteral("sync-active.svg")).pixmap(16, 16).toImage());
}

void TestUiStatusIntegration::testOfflineStatusPropagatesToTrayAndWindow() {
    m_changeProcessor->start();
    m_coordinator->updateMirrorProcessorState(m_changeProcessor->state());
    m_coordinator->updateAuthState(true);
    m_coordinator->refreshMirrorStatus();

    m_pauseController->setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline,
                                                true);

    QTRY_COMPARE(windowSummary(), QStringLiteral("Offline"));
    QTRY_COMPARE(traySummary(), QStringLiteral("Offline"));
    QTRY_COMPARE(trayTooltip(), QStringLiteral("Via\nOffline"));
    QTRY_COMPARE(m_window->m_pauseSyncButton->text(), QStringLiteral("Resume Sync"));
    QTRY_COMPARE(m_tray->m_pauseSyncAction->text(), QStringLiteral("Resume Sync"));
}

void TestUiStatusIntegration::testMirrorDisabledOfflineRecoveryReturnsToFuseStatus() {
    RuntimePauseController pauseController;
    UiStatusCoordinator coordinator(nullptr, false, &pauseController);
    SystemTrayManager tray(nullptr, &pauseController, &coordinator);
    MainWindow window(nullptr, nullptr, nullptr, &pauseController, &coordinator, nullptr);

    coordinator.updateAuthState(true);
    coordinator.updateFuseStatus(QStringLiteral("Mounted"));

    pauseController.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline,
                                             true);

    const QString offlineSummary = QStringLiteral("Mirror: Offline | FUSE: Mounted");
    QTRY_COMPARE(window.m_statusLabel->text(), offlineSummary);
    QTRY_COMPARE(tray.m_statusAction->text(), offlineSummary);
    QTRY_COMPARE(tray.trayIcon()->toolTip(), QStringLiteral("Via\n%1").arg(offlineSummary));
    QTRY_COMPARE(
        tray.trayIcon()->icon().pixmap(16, 16).toImage(),
        ThemeHelper::trayIcon(QStringLiteral("no-connection.svg")).pixmap(16, 16).toImage());

    pauseController.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline,
                                             false);

    QTRY_COMPARE(window.m_statusLabel->text(), QStringLiteral("Mounted"));
    QTRY_COMPARE(tray.m_statusAction->text(), QStringLiteral("Mounted"));
    QTRY_COMPARE(tray.trayIcon()->toolTip(), QStringLiteral("Via\nMounted"));
    QTRY_COMPARE(tray.trayIcon()->icon().pixmap(16, 16).toImage(),
                 ThemeHelper::trayIcon(QStringLiteral("drive-idle.svg")).pixmap(16, 16).toImage());
}

void TestUiStatusIntegration::testMetadataRefreshLifecyclePropagatesWhenMirrorDisabled() {
    RuntimePauseController pauseController;
    UiStatusCoordinator coordinator(nullptr, false, &pauseController);
    SystemTrayManager tray(nullptr, &pauseController, &coordinator);
    MainWindow window(nullptr, nullptr, nullptr, &pauseController, &coordinator, nullptr);

    coordinator.onMetadataRefreshStarted();

    QTRY_COMPARE(window.m_statusLabel->text(), QStringLiteral("Refreshing metadata"));
    QTRY_COMPARE(tray.m_statusAction->text(), QStringLiteral("Refreshing metadata"));
    QTRY_COMPARE(tray.trayIcon()->toolTip(), QStringLiteral("Via\nRefreshing metadata"));

    coordinator.onMetadataRefreshFinished();

    QTRY_COMPARE(window.m_statusLabel->text(), QStringLiteral("Mounted"));
    QTRY_COMPARE(tray.m_statusAction->text(), QStringLiteral("Mounted"));
    QTRY_COMPARE(tray.trayIcon()->toolTip(), QStringLiteral("Via\nMounted"));
}

void TestUiStatusIntegration::testAuthExpiredRecoveryPropagatesToTrayAndWindow() {
    m_changeProcessor->start();
    m_coordinator->updateMirrorProcessorState(m_changeProcessor->state());
    m_coordinator->updateAuthState(true);
    m_coordinator->refreshMirrorStatus();

    m_coordinator->setAuthExpired(QStringLiteral("refresh token revoked"));

    QVERIFY(m_coordinator->snapshot().combinedStatusText.contains(
        QStringLiteral("Authentication expired")));
    QTRY_VERIFY(windowSummary().contains(QStringLiteral("Authentication expired")));
    QTRY_VERIFY(traySummary().contains(QStringLiteral("Authentication expired")));

    m_coordinator->updateAuthState(true);
    m_coordinator->refreshMirrorStatus();

    QTRY_COMPARE(windowSummary(), QStringLiteral("Up to date"));
    QTRY_COMPARE(traySummary(), QStringLiteral("Up to date"));
}

void TestUiStatusIntegration::testMainWindowSyncNowEmitsRequestSignal() {
    QSignalSpy spy(m_window, &MainWindow::fullSyncRequested);

    m_window->onRefreshClicked();

    QCOMPARE(spy.count(), 1);
}

void TestUiStatusIntegration::testPauseActionsTogglePauseController() {
    QVERIFY(!m_pauseController->isEffectivelyPaused());

    m_window->onPauseSyncClicked();

    QTRY_VERIFY(m_pauseController->isEffectivelyPaused());
    QTRY_COMPARE(m_window->m_pauseSyncButton->text(), QStringLiteral("Resume Sync"));
    QTRY_COMPARE(m_tray->m_pauseSyncAction->text(), QStringLiteral("Resume Sync"));

    m_tray->onPauseSyncClicked();

    QTRY_VERIFY(!m_pauseController->isEffectivelyPaused());
    QTRY_COMPARE(m_window->m_pauseSyncButton->text(), QStringLiteral("Pause Sync"));
    QTRY_COMPARE(m_tray->m_pauseSyncAction->text(), QStringLiteral("Pause Sync"));
}

void TestUiStatusIntegration::testBothSyncSystemsDisabledUsesPausedStatusAndDisablesActions() {
    RuntimePauseController pauseController;
    UiStatusCoordinator coordinator(nullptr, false, &pauseController);
    coordinator.setFuseEnabled(false);
    SystemTrayManager tray(nullptr, &pauseController, &coordinator, false, false);
    MainWindow window(nullptr, nullptr, nullptr, &pauseController, &coordinator, nullptr, false,
                      false);

    coordinator.updateAuthState(true);
    tray.updateAuthState(true);
    window.updateAuthState(true);

    QTRY_COMPARE(window.m_statusLabel->text(), QStringLiteral("Sync disabled"));
    QTRY_COMPARE(tray.m_statusAction->text(), QStringLiteral("Sync disabled"));
    QTRY_COMPARE(tray.trayIcon()->toolTip(), QStringLiteral("Via\nSync disabled"));
    QTRY_COMPARE(tray.trayIcon()->icon().pixmap(16, 16).toImage(),
                 ThemeHelper::trayIcon(QStringLiteral("paused.svg")).pixmap(16, 16).toImage());
    QTRY_COMPARE(window.m_statusIcon->pixmap(Qt::ReturnByValue).toImage(),
                 ThemeHelper::guiIcon(QStringLiteral("paused.svg")).pixmap(32, 32).toImage());

    QVERIFY(!window.m_pauseSyncButton->isEnabled());
    QVERIFY(!window.m_refreshButton->isEnabled());
    QVERIFY(!tray.m_pauseSyncAction->isEnabled());
    QVERIFY(!tray.m_syncNowAction->isEnabled());
}

QTEST_MAIN(TestUiStatusIntegration)
#include "TestUiStatusIntegration.moc"