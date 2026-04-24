/**
 * @file TestUiStatusIntegration.cpp
 * @brief UI integration tests for shared status rendering.
 */

#include <QtTest/QtTest>

#include "sync/ChangeProcessor.h"
#include "sync/ChangeQueue.h"
#include "sync/SyncActionQueue.h"

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
    void testFuseOnlyStatusPropagatesWhenMirrorDisabled();
    void testAuthExpiredRecoveryPropagatesToTrayAndWindow();

   private:
    QString traySummary() const;
    QString trayTooltip() const;
    QString windowSummary() const;

    ChangeQueue* m_changeQueue = nullptr;
    SyncActionQueue* m_syncActionQueue = nullptr;
    ChangeProcessor* m_changeProcessor = nullptr;
    UiStatusCoordinator* m_coordinator = nullptr;
    SystemTrayManager* m_tray = nullptr;
    MainWindow* m_window = nullptr;
};

void TestUiStatusIntegration::init() {
    m_changeQueue = new ChangeQueue();
    m_syncActionQueue = new SyncActionQueue();
    m_changeProcessor = new ChangeProcessor(m_changeQueue, m_syncActionQueue, nullptr, nullptr);
    m_coordinator = new UiStatusCoordinator(nullptr, m_syncActionQueue, m_changeProcessor);
    m_tray = new SystemTrayManager(nullptr, m_changeProcessor, m_coordinator);
    m_window = new MainWindow(nullptr, nullptr, m_syncActionQueue, m_changeProcessor, nullptr,
                              nullptr, m_coordinator, nullptr);
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

    delete m_syncActionQueue;
    m_syncActionQueue = nullptr;

    delete m_changeQueue;
    m_changeQueue = nullptr;
}

QString TestUiStatusIntegration::traySummary() const { return m_tray->m_statusAction->text(); }

QString TestUiStatusIntegration::trayTooltip() const { return m_tray->trayIcon()->toolTip(); }

QString TestUiStatusIntegration::windowSummary() const { return m_window->m_statusLabel->text(); }

void TestUiStatusIntegration::testFuseUploadingStatusPropagatesToTrayAndWindow() {
    m_changeProcessor->start();
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

void TestUiStatusIntegration::testFuseOnlyStatusPropagatesWhenMirrorDisabled() {
    UiStatusCoordinator coordinator(nullptr, nullptr, nullptr);
    SystemTrayManager tray(nullptr, nullptr, &coordinator);
    MainWindow window(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &coordinator, nullptr);

    coordinator.updateFuseStatus(QStringLiteral("Refreshing metadata"));

    QTRY_COMPARE(window.m_statusLabel->text(), QStringLiteral("Refreshing metadata"));
    QTRY_COMPARE(tray.m_statusAction->text(), QStringLiteral("Refreshing metadata"));
    QTRY_COMPARE(tray.trayIcon()->toolTip(), QStringLiteral("Via\nRefreshing metadata"));
}

void TestUiStatusIntegration::testAuthExpiredRecoveryPropagatesToTrayAndWindow() {
    m_changeProcessor->start();
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

QTEST_MAIN(TestUiStatusIntegration)
#include "TestUiStatusIntegration.moc"