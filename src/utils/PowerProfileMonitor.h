/**
 * @file PowerProfileMonitor.h
 * @brief Detects Linux power-saver mode via power-profiles-daemon D-Bus
 */

#ifndef POWERPROFILEMONITOR_H
#define POWERPROFILEMONITOR_H

#include <QObject>
#include <QStringList>
#include <QVariantMap>

/**
 * @class PowerProfileMonitor
 * @brief Monitors the active system power profile on Linux
 *
 * Listens to the system bus service exposed by power-profiles-daemon and
 * emits a signal whenever the system enters or leaves the "power-saver"
 * profile. When the service is unavailable, the monitor stays inactive and
 * reports false.
 */
class PowerProfileMonitor : public QObject {
    Q_OBJECT

   public:
    explicit PowerProfileMonitor(QObject* parent = nullptr);
    ~PowerProfileMonitor() override = default;

    /**
     * @brief Whether a power-profile service is currently reachable
     */
    bool isAvailable() const;

    /**
     * @brief Whether the observed active profile is a power-saver profile
     */
    bool isPowerSaverActive() const;

    /**
     * @brief Normalize a profile name into a power-saver decision
     */
    static bool isPowerSaverProfileName(const QString& activeProfile);

   public slots:
    /**
     * @brief Refresh the current active profile from D-Bus
     */
    void refresh();

    /**
     * @brief Apply an observed active profile name
     *
     * Exposed as a slot so focused tests can drive the state machine without
     * requiring a live D-Bus service.
     */
    void applyActiveProfile(const QString& activeProfile);

   signals:
    /**
     * @brief Emitted when the system enters or leaves power-saver mode
     */
    void powerSaverChanged(bool active);

   private slots:
    void onPropertiesChanged(const QString& interfaceName, const QVariantMap& changedProperties,
                             const QStringList& invalidatedProperties);

   private:
    bool m_available = false;
    bool m_powerSaverActive = false;
};

#endif  // POWERPROFILEMONITOR_H