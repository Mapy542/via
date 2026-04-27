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

QTEST_MAIN(TestRuntimePauseController)
#include "TestRuntimePauseController.moc"