/**
 * @file TestRuntimePauseController.cpp
 * @brief Focused tests for the runtime pause policy controller.
 */

#include <QtTest/QtTest>

#include "sync/RuntimePauseController.h"

class TestRuntimePauseController : public QObject {
    Q_OBJECT

   private slots:
    void testManualPauseAndResume();
    void testAutoPauseReasonLifecycle();
    void testManualResumeSuppressesActiveAutoReasons();
    void testAdditionalAutoPauseReasonsUseControllerStatusText();
    void testDisablingAutoPauseIgnoresActiveReasons();
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

QTEST_MAIN(TestRuntimePauseController)
#include "TestRuntimePauseController.moc"