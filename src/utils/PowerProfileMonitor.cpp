/**
 * @file PowerProfileMonitor.cpp
 * @brief Implementation of Linux power-saver detection via D-Bus
 */

#include "PowerProfileMonitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDebug>

namespace {

constexpr char kPowerProfilesService[] = "net.hadess.PowerProfiles";
constexpr char kPowerProfilesPath[] = "/net/hadess/PowerProfiles";
constexpr char kPowerProfilesInterface[] = "net.hadess.PowerProfiles";
constexpr char kDbusPropertiesInterface[] = "org.freedesktop.DBus.Properties";
constexpr char kActiveProfileProperty[] = "ActiveProfile";

}  // namespace

PowerProfileMonitor::PowerProfileMonitor(QObject* parent) : QObject(parent) {
    QDBusInterface interface(
        QString::fromLatin1(kPowerProfilesService), QString::fromLatin1(kPowerProfilesPath),
        QString::fromLatin1(kPowerProfilesInterface), QDBusConnection::systemBus(), this);
    if (!interface.isValid()) {
        qInfo() << "PowerProfileMonitor: power-profiles-daemon unavailable";
        return;
    }

    m_available = true;

    if (!QDBusConnection::systemBus().connect(
            QString::fromLatin1(kPowerProfilesService), QString::fromLatin1(kPowerProfilesPath),
            QString::fromLatin1(kDbusPropertiesInterface), QStringLiteral("PropertiesChanged"),
            this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)))) {
        qWarning() << "PowerProfileMonitor: Failed to subscribe to power profile changes";
    }

    refresh();
}

bool PowerProfileMonitor::isAvailable() const { return m_available; }

bool PowerProfileMonitor::isPowerSaverActive() const { return m_powerSaverActive; }

bool PowerProfileMonitor::isPowerSaverProfileName(const QString& activeProfile) {
    const QString normalized = activeProfile.trimmed().toLower();
    return normalized == QStringLiteral("power-saver") ||
           normalized == QStringLiteral("power saver") ||
           normalized == QStringLiteral("powersaver");
}

void PowerProfileMonitor::refresh() {
    QDBusInterface interface(
        QString::fromLatin1(kPowerProfilesService), QString::fromLatin1(kPowerProfilesPath),
        QString::fromLatin1(kPowerProfilesInterface), QDBusConnection::systemBus(), this);
    if (!interface.isValid()) {
        if (m_available) {
            qWarning() << "PowerProfileMonitor: power profile service became unavailable";
        }
        m_available = false;
        applyActiveProfile(QString());
        return;
    }

    m_available = true;

    const QVariant activeProfile = interface.property(kActiveProfileProperty);
    if (!activeProfile.isValid()) {
        qWarning() << "PowerProfileMonitor: Failed to query ActiveProfile";
        applyActiveProfile(QString());
        return;
    }

    applyActiveProfile(activeProfile.toString());
}

void PowerProfileMonitor::applyActiveProfile(const QString& activeProfile) {
    const bool powerSaverActive = isPowerSaverProfileName(activeProfile);
    if (m_powerSaverActive == powerSaverActive) {
        return;
    }

    m_powerSaverActive = powerSaverActive;
    qInfo() << "PowerProfileMonitor: Active profile is"
            << (activeProfile.isEmpty() ? QStringLiteral("<unknown>") : activeProfile)
            << "powerSaver=" << m_powerSaverActive;
    emit powerSaverChanged(m_powerSaverActive);
}

void PowerProfileMonitor::onPropertiesChanged(const QString& interfaceName,
                                              const QVariantMap& changedProperties,
                                              const QStringList& invalidatedProperties) {
    if (interfaceName != QString::fromLatin1(kPowerProfilesInterface)) {
        return;
    }

    if (!changedProperties.contains(QString::fromLatin1(kActiveProfileProperty)) &&
        !invalidatedProperties.contains(QString::fromLatin1(kActiveProfileProperty))) {
        return;
    }

    refresh();
}