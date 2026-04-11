/**
 * @file SuspendMonitor.cpp
 * @brief Implementation of system suspend/resume detection
 */

#include "SuspendMonitor.h"

#include <QDBusConnection>
#include <QDebug>

SuspendMonitor::SuspendMonitor(QObject* parent) : QObject(parent) {
    // Connect to the systemd-logind PrepareForSleep signal via the system bus.
    // Signal: org.freedesktop.login1.Manager.PrepareForSleep(boolean suspending)
    m_active = QDBusConnection::systemBus().connect(
        QStringLiteral("org.freedesktop.login1"),          // service
        QStringLiteral("/org/freedesktop/login1"),         // path
        QStringLiteral("org.freedesktop.login1.Manager"),  // interface
        QStringLiteral("PrepareForSleep"),                 // signal name
        this, SLOT(onPrepareForSleep(bool)));

    if (m_active) {
        qInfo() << "SuspendMonitor: Listening for system suspend/resume events";
    } else {
        qWarning() << "SuspendMonitor: Failed to connect to logind PrepareForSleep signal"
                   << "(systemd-logind may not be available)";
    }
}

bool SuspendMonitor::isActive() const { return m_active; }

void SuspendMonitor::onPrepareForSleep(bool suspending) {
    if (suspending) {
        qInfo() << "SuspendMonitor: System is about to suspend";
        emit aboutToSuspend();
    } else {
        qInfo() << "SuspendMonitor: System has resumed from suspend";
        emit resumed();
    }
}
