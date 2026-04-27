/**
 * @file TestPowerProfileMonitor.cpp
 * @brief Focused tests for Linux power-saver profile monitoring.
 */

#include <QSignalSpy>
#include <QtTest/QtTest>

#include "utils/PowerProfileMonitor.h"

class TestPowerProfileMonitor : public QObject {
    Q_OBJECT

   private slots:
    void testRecognizesPowerSaverProfileNames();
    void testApplyActiveProfileEmitsOnlyOnTransitions();
};

void TestPowerProfileMonitor::testRecognizesPowerSaverProfileNames() {
    QVERIFY(PowerProfileMonitor::isPowerSaverProfileName(QStringLiteral("power-saver")));
    QVERIFY(PowerProfileMonitor::isPowerSaverProfileName(QStringLiteral("POWER SAVER")));
    QVERIFY(PowerProfileMonitor::isPowerSaverProfileName(QStringLiteral(" powersaver ")));
    QVERIFY(!PowerProfileMonitor::isPowerSaverProfileName(QStringLiteral("balanced")));
    QVERIFY(!PowerProfileMonitor::isPowerSaverProfileName(QStringLiteral("performance")));
}

void TestPowerProfileMonitor::testApplyActiveProfileEmitsOnlyOnTransitions() {
    PowerProfileMonitor monitor;

    monitor.applyActiveProfile(QStringLiteral("balanced"));
    QVERIFY(!monitor.isPowerSaverActive());

    QSignalSpy stateSpy(&monitor, &PowerProfileMonitor::powerSaverChanged);
    QVERIFY(stateSpy.isValid());

    monitor.applyActiveProfile(QStringLiteral("power-saver"));
    QCOMPARE(stateSpy.count(), 1);
    QVERIFY(monitor.isPowerSaverActive());
    QCOMPARE(stateSpy.takeFirst().at(0).toBool(), true);

    monitor.applyActiveProfile(QStringLiteral("POWER SAVER"));
    QCOMPARE(stateSpy.count(), 0);

    monitor.applyActiveProfile(QStringLiteral("performance"));
    QCOMPARE(stateSpy.count(), 1);
    QVERIFY(!monitor.isPowerSaverActive());
    QCOMPARE(stateSpy.takeFirst().at(0).toBool(), false);
}

QTEST_MAIN(TestPowerProfileMonitor)
#include "TestPowerProfileMonitor.moc"