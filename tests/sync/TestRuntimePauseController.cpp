/**
 * @file TestRuntimePauseController.cpp
 * @brief Focused tests for the runtime pause policy controller.
 */

#include <QtTest/QtTest>

#include "sync/RuntimePauseController.h"
#include "sync/WakeResumeNotificationSuppressor.h"

class TestRuntimePauseController : public QObject {
    Q_OBJECT

   private slots:
    void testManualPauseAndResume();
    void testAutoPauseReasonLifecycle();
    void testManualResumeSuppressesActiveAutoReasons();
    void testAdditionalAutoPauseReasonsUseControllerStatusText();
    void testDisablingAutoPauseIgnoresActiveReasons();
    void testWakeResumeSuppressionSuppresssPauseNotificationDuringSuspend();
    void testWakeResumeSuppressionForSleepIntroducedOfflinePause();
    void testWakeResumeSuppressionSuppresssPauseNotificationAfterWake();
    void testWakeResumeSuppressionTracksOfflinePauseEngagedAfterWake();
    void testWakeResumeSuppressionDoesNotHidePreExistingOfflineRecovery();
    void testWakeResumeSuppressionDoesNotHidePowerSaverRecovery();
    void testWakeResumeSuppressionDoesNotSuppressPowerSaverPauseNotification();
    void testWakeResumeSuppressionDoesNotHideManualOfflineOverride();
    void testWakeResumeSuppressionExpiresObservationWindow();
};

void TestRuntimePauseController::testManualPauseAndResume() {
    RuntimePauseController controller;

    controller.requestManualPause();
    QVERIFY(controller.isEffectivelyPaused());
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Paused"));

    controller.requestManualResume();
    QVERIFY(controller.isDriveApiAllowed());
    QVERIFY(controller.effectiveStatusText().isEmpty());
}

void TestRuntimePauseController::testAutoPauseReasonLifecycle() {
    RuntimePauseController controller;

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);

    QVERIFY(controller.isEffectivelyPaused());
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Offline"));
    QVERIFY(
        controller.hasEffectiveAutoPauseReason(RuntimePauseController::AutoPauseReason::Offline));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);

    QVERIFY(controller.isDriveApiAllowed());
    QVERIFY(
        !controller.hasEffectiveAutoPauseReason(RuntimePauseController::AutoPauseReason::Offline));
}

void TestRuntimePauseController::testManualResumeSuppressesActiveAutoReasons() {
    RuntimePauseController controller;

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    QVERIFY(controller.isEffectivelyPaused());

    controller.requestManualResume();
    QVERIFY(controller.isDriveApiAllowed());
    QVERIFY(controller.activeAutoPauseReasons().testFlag(
        RuntimePauseController::AutoPauseReason::Offline));
    QVERIFY(controller.suppressedAutoPauseReasons().testFlag(
        RuntimePauseController::AutoPauseReason::Offline));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);
    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);

    QVERIFY(controller.isEffectivelyPaused());
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Offline"));
}

void TestRuntimePauseController::testAdditionalAutoPauseReasonsUseControllerStatusText() {
    RuntimePauseController controller;

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::MeteredNetwork,
                                        true);
    QVERIFY(controller.isEffectivelyPaused());
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Paused (metered network)"));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::PowerSaver, true);
    QCOMPARE(controller.effectiveStatusText(),
             QStringLiteral("Paused (metered network, power saver)"));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::MeteredNetwork,
                                        false);
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Paused (power saver)"));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::PowerSaver, false);
    QVERIFY(controller.isDriveApiAllowed());
    QVERIFY(controller.effectiveStatusText().isEmpty());
}

void TestRuntimePauseController::testDisablingAutoPauseIgnoresActiveReasons() {
    RuntimePauseController controller;

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::PowerSaver, true);

    QVERIFY(controller.isEffectivelyPaused());
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Offline"));

    controller.setAutoPauseEnabled(false);

    QVERIFY(!controller.isAutoPauseEnabled());
    QVERIFY(controller.isDriveApiAllowed());
    QVERIFY(controller.activeAutoPauseReasons().testFlag(
        RuntimePauseController::AutoPauseReason::Offline));
    QVERIFY(controller.activeAutoPauseReasons().testFlag(
        RuntimePauseController::AutoPauseReason::PowerSaver));
    QCOMPARE(controller.effectiveAutoPauseReasons(), RuntimePauseController::AutoPauseReasons());
    QVERIFY(
        !controller.hasEffectiveAutoPauseReason(RuntimePauseController::AutoPauseReason::Offline));
    QVERIFY(controller.effectiveStatusText().isEmpty());

    controller.setAutoPauseEnabled(true);

    QVERIFY(controller.isAutoPauseEnabled());
    QVERIFY(controller.isEffectivelyPaused());
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Offline"));

    RuntimePauseController manualController;
    manualController.setAutoPauseEnabled(false);

    manualController.requestManualPause();
    QVERIFY(manualController.isEffectivelyPaused());
    QCOMPARE(manualController.effectiveStatusText(), QStringLiteral("Paused"));

    manualController.requestManualResume();
    QVERIFY(manualController.isDriveApiAllowed());
    QVERIFY(manualController.effectiveStatusText().isEmpty());
}

void TestRuntimePauseController::
    testWakeResumeSuppressionSuppresssPauseNotificationDuringSuspend() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    const RuntimePauseController::Snapshot preSuspend = controller.snapshot();
    suppressor.recordPreSuspendState(preSuspend);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();

    suppressor.observePauseTransition(preSuspend, paused, 999);

    QVERIFY(suppressor.consumePauseNotificationSuppression(preSuspend, paused));
    QVERIFY(!suppressor.consumePauseNotificationSuppression(preSuspend, paused));

    suppressor.noteWake(paused, 1000);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

void TestRuntimePauseController::testWakeResumeSuppressionForSleepIntroducedOfflinePause() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    const RuntimePauseController::Snapshot preSuspend = controller.snapshot();
    suppressor.recordPreSuspendState(preSuspend);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();

    suppressor.observePauseTransition(preSuspend, paused, 999);
    suppressor.noteWake(paused, 1000);

    QVERIFY(suppressor.consumePauseNotificationSuppression(preSuspend, paused));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(suppressor.consumeResumeNotificationSuppression(paused, resumed));
    QVERIFY(!suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

void TestRuntimePauseController::testWakeResumeSuppressionSuppresssPauseNotificationAfterWake() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    const RuntimePauseController::Snapshot preWake = controller.snapshot();
    suppressor.recordPreSuspendState(preWake);
    suppressor.noteWake(preWake, 1000);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();
    suppressor.observePauseTransition(preWake, paused, 1001);

    QVERIFY(suppressor.consumePauseNotificationSuppression(preWake, paused));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

void TestRuntimePauseController::testWakeResumeSuppressionTracksOfflinePauseEngagedAfterWake() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    const RuntimePauseController::Snapshot preWake = controller.snapshot();
    suppressor.recordPreSuspendState(preWake);
    suppressor.noteWake(preWake, 1000);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();
    suppressor.observePauseTransition(preWake, paused, 1001);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

void TestRuntimePauseController::testWakeResumeSuppressionDoesNotHidePreExistingOfflineRecovery() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();

    suppressor.recordPreSuspendState(paused);
    suppressor.noteWake(paused, 1000);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(!suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

void TestRuntimePauseController::testWakeResumeSuppressionDoesNotHidePowerSaverRecovery() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    suppressor.recordPreSuspendState(controller.snapshot());

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::PowerSaver, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();

    suppressor.noteWake(paused, 1000);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::PowerSaver, false);
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(!suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

void TestRuntimePauseController::
    testWakeResumeSuppressionDoesNotSuppressPowerSaverPauseNotification() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    const RuntimePauseController::Snapshot preWake = controller.snapshot();
    suppressor.recordPreSuspendState(preWake);
    suppressor.noteWake(preWake, 1000);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::PowerSaver, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();
    suppressor.observePauseTransition(preWake, paused, 1001);

    QVERIFY(!suppressor.consumePauseNotificationSuppression(preWake, paused));
}

void TestRuntimePauseController::testWakeResumeSuppressionDoesNotHideManualOfflineOverride() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    suppressor.recordPreSuspendState(controller.snapshot());

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();

    suppressor.noteWake(paused, 1000);

    controller.requestManualResume();
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(resumed.suppressedAutoPauseReasons.testFlag(
        RuntimePauseController::AutoPauseReason::Offline));
    QVERIFY(!suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

void TestRuntimePauseController::testWakeResumeSuppressionExpiresObservationWindow() {
    RuntimePauseController controller;
    WakeResumeNotificationSuppressor suppressor;

    const RuntimePauseController::Snapshot preWake = controller.snapshot();
    suppressor.recordPreSuspendState(preWake);
    suppressor.noteWake(preWake, 1000);

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    const RuntimePauseController::Snapshot paused = controller.snapshot();
    suppressor.observePauseTransition(
        preWake, paused,
        1000 + WakeResumeNotificationSuppressor::kWakePauseObservationWindowMs + 1);

    QVERIFY(!suppressor.consumePauseNotificationSuppression(preWake, paused));

    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, false);
    const RuntimePauseController::Snapshot resumed = controller.snapshot();

    QVERIFY(!suppressor.consumeResumeNotificationSuppression(paused, resumed));
}

QTEST_MAIN(TestRuntimePauseController)
#include "TestRuntimePauseController.moc"