/**
 * @file WakeRefreshNotificationGate.h
 * @brief One-shot suppression gate for wake-triggered token refresh warnings.
 */

#ifndef WAKEREFRESHNOTIFICATIONGATE_H
#define WAKEREFRESHNOTIFICATIONGATE_H

class WakeRefreshNotificationGate {
   public:
    void beginWakeRefreshAttempt() {
        m_pendingWakeRefresh = true;
        m_sawWakeAuthExpired = false;
    }

    void markTokenRefreshed() { reset(); }

    void markAuthExpired() {
        if (m_pendingWakeRefresh) {
            m_sawWakeAuthExpired = true;
        }
    }

    bool consumeTokenRefreshWarningSuppression() {
        if (!m_pendingWakeRefresh) {
            return false;
        }

        reset();
        return true;
    }

    void reset() {
        m_pendingWakeRefresh = false;
        m_sawWakeAuthExpired = false;
    }

    bool sawWakeAuthExpired() const { return m_pendingWakeRefresh && m_sawWakeAuthExpired; }

   private:
    bool m_pendingWakeRefresh = false;
    bool m_sawWakeAuthExpired = false;
};

#endif  // WAKEREFRESHNOTIFICATIONGATE_H