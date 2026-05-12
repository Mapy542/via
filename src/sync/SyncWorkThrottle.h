/**
 * @file SyncWorkThrottle.h
 * @brief Cooperative mirror-work throttle helper
 */

#ifndef SYNCWORKTHROTTLE_H
#define SYNCWORKTHROTTLE_H

#include <QElapsedTimer>
#include <QtGlobal>

#include "SyncSettings.h"

class SyncWorkThrottle {
   public:
    void setSettings(const SyncSettings& settings) {
        m_dormantTimeMs = SyncSettings::normalizeMirrorDormantTimeMs(settings.mirrorDormantTimeMs);
        m_dutyCyclePercent =
            SyncSettings::normalizeMirrorDutyCyclePercent(settings.mirrorDutyCyclePercent);
        m_enabled =
            m_dormantTimeMs > 0 && m_dutyCyclePercent < SyncSettings::MAX_MIRROR_DUTY_CYCLE_PERCENT;

        if (!m_enabled) {
            m_activeBudgetMs = 0;
            reset();
            return;
        }

        const double activeBudget =
            (static_cast<double>(m_dormantTimeMs) * static_cast<double>(m_dutyCyclePercent)) /
            static_cast<double>(SyncSettings::MAX_MIRROR_DUTY_CYCLE_PERCENT - m_dutyCyclePercent);
        m_activeBudgetMs = qMax<qint64>(1, qRound64(activeBudget));
        reset();
    }

    void reset() {
        if (!m_clock.isValid()) {
            m_clock.start();
        }

        m_activeWindowStartMs = -1;
        m_resumeDeadlineMs = -1;
    }

    int nextDelayMs() {
        if (!m_enabled || m_dormantTimeMs <= 0) {
            return 0;
        }

        if (!m_clock.isValid()) {
            m_clock.start();
        }

        const qint64 nowMs = m_clock.elapsed();

        if (m_resumeDeadlineMs >= 0) {
            if (nowMs < m_resumeDeadlineMs) {
                return static_cast<int>(m_resumeDeadlineMs - nowMs);
            }

            m_resumeDeadlineMs = -1;
            m_activeWindowStartMs = nowMs;
            return 0;
        }

        if (m_activeWindowStartMs < 0) {
            m_activeWindowStartMs = nowMs;
            return 0;
        }

        if ((nowMs - m_activeWindowStartMs) < m_activeBudgetMs) {
            return 0;
        }

        m_activeWindowStartMs = -1;
        m_resumeDeadlineMs = nowMs + m_dormantTimeMs;
        return m_dormantTimeMs;
    }

   private:
    QElapsedTimer m_clock;
    qint64 m_activeWindowStartMs = -1;
    qint64 m_resumeDeadlineMs = -1;
    qint64 m_activeBudgetMs = 0;
    int m_dormantTimeMs = SyncSettings::DEFAULT_MIRROR_DORMANT_TIME_MS;
    int m_dutyCyclePercent = SyncSettings::DEFAULT_MIRROR_DUTY_CYCLE_PERCENT;
    bool m_enabled = false;
};

#endif  // SYNCWORKTHROTTLE_H