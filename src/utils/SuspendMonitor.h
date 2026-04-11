/**
 * @file SuspendMonitor.h
 * @brief Detects system suspend/resume events via D-Bus
 *
 * Listens for the org.freedesktop.login1.Manager.PrepareForSleep signal
 * emitted by systemd-logind before suspend and after resume. This allows
 * the application to recover network connections, refresh auth tokens,
 * and restart background workers after waking from sleep.
 */

#ifndef SUSPENDMONITOR_H
#define SUSPENDMONITOR_H

#include <QDBusConnection>
#include <QObject>

/**
 * @class SuspendMonitor
 * @brief Monitors system suspend/resume via systemd-logind D-Bus signals
 *
 * On Linux with systemd, the login1 Manager emits PrepareForSleep(true)
 * before suspend and PrepareForSleep(false) after resume.  This class
 * converts those signals into Qt signals that the rest of the application
 * can connect to for recovery actions.
 */
class SuspendMonitor : public QObject {
    Q_OBJECT

   public:
    explicit SuspendMonitor(QObject* parent = nullptr);
    ~SuspendMonitor() override = default;

    /**
     * @brief Whether the D-Bus signal was successfully connected
     */
    bool isActive() const;

   signals:
    /**
     * @brief Emitted when the system is about to suspend
     */
    void aboutToSuspend();

    /**
     * @brief Emitted when the system has resumed from suspend
     */
    void resumed();

   private slots:
    void onPrepareForSleep(bool suspending);

   private:
    bool m_active = false;
};

#endif  // SUSPENDMONITOR_H
