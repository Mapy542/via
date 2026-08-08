/**
 * @file WakeResumeNotificationSuppressor.h
 * @brief One-shot suppression gate for wake-triggered pause and resume notifications.
 */

#ifndef WAKERESUMENOTIFICATIONSUPPRESSOR_H
#define WAKERESUMENOTIFICATIONSUPPRESSOR_H

#include <QtGlobal>

#include "sync/RuntimePauseController.h"

class WakeResumeNotificationSuppressor {
   public:
    static constexpr qint64 kWakePauseObservationWindowMs = 60000;

    void recordPreSuspendState(const RuntimePauseController::Snapshot& snapshot) {
        m_preSuspend = snapshot;
        m_hasPreSuspendState = true;
        m_waitingForWake = true;
        m_pendingPauseNotificationSuppression = false;
        m_pendingResumeNotificationSuppression = false;
        m_observePauseTransitionUntilMs = 0;
    }

    void noteWake(const RuntimePauseController::Snapshot& wakeSnapshot, qint64 nowMs) {
        if (!m_hasPreSuspendState) {
            reset();
            return;
        }

        m_waitingForWake = false;

        if (shouldSuppressTransitionFrom(m_preSuspend, wakeSnapshot)) {
            armNotificationSuppressions();
            m_observePauseTransitionUntilMs = 0;
            return;
        }

        if (hasPendingNotificationSuppression() || m_preSuspend.effectivePause) {
            m_observePauseTransitionUntilMs = 0;
            return;
        }

        m_observePauseTransitionUntilMs = nowMs + kWakePauseObservationWindowMs;
    }

    void observePauseTransition(const RuntimePauseController::Snapshot& previous,
                                const RuntimePauseController::Snapshot& current, qint64 nowMs) {
        if (!m_hasPreSuspendState || previous.effectivePause == current.effectivePause) {
            return;
        }

        if (m_waitingForWake) {
            if (!previous.effectivePause && current.effectivePause &&
                shouldSuppressTransitionFrom(m_preSuspend, current)) {
                armNotificationSuppressions();
            }
            return;
        }

        if (hasPendingNotificationSuppression() || m_observePauseTransitionUntilMs == 0) {
            return;
        }

        if (nowMs > m_observePauseTransitionUntilMs) {
            m_observePauseTransitionUntilMs = 0;
            return;
        }

        if (!previous.effectivePause && current.effectivePause &&
            shouldSuppressTransitionFrom(m_preSuspend, current)) {
            armNotificationSuppressions();
        }

        m_observePauseTransitionUntilMs = 0;
    }

    bool consumePauseNotificationSuppression(const RuntimePauseController::Snapshot& previous,
                                             const RuntimePauseController::Snapshot& current) {
        if (!m_pendingPauseNotificationSuppression || previous.effectivePause ||
            !current.effectivePause) {
            return false;
        }

        m_pendingPauseNotificationSuppression = false;
        return shouldSuppressTransitionFrom(m_preSuspend, current);
    }

    bool consumeResumeNotificationSuppression(const RuntimePauseController::Snapshot& previous,
                                              const RuntimePauseController::Snapshot& current) {
        if (!m_pendingResumeNotificationSuppression || !previous.effectivePause ||
            current.effectivePause) {
            return false;
        }

        const bool shouldSuppress = isOfflineOnly(previous) && current.autoPauseEnabled &&
                                    !hasSuppressedActiveAutoPause(current);
        reset();
        return shouldSuppress;
    }

    void reset() {
        m_preSuspend = RuntimePauseController::Snapshot();
        m_hasPreSuspendState = false;
        m_waitingForWake = false;
        m_pendingPauseNotificationSuppression = false;
        m_pendingResumeNotificationSuppression = false;
        m_observePauseTransitionUntilMs = 0;
    }

   private:
    void armNotificationSuppressions() {
        m_pendingPauseNotificationSuppression = true;
        m_pendingResumeNotificationSuppression = true;
    }

    bool hasPendingNotificationSuppression() const {
        return m_pendingPauseNotificationSuppression || m_pendingResumeNotificationSuppression;
    }

    static RuntimePauseController::AutoPauseReasons effectiveAutoPauseReasons(
        const RuntimePauseController::Snapshot& snapshot) {
        if (!snapshot.autoPauseEnabled) {
            return RuntimePauseController::AutoPauseReasons();
        }

        return snapshot.activeAutoPauseReasons & ~snapshot.suppressedAutoPauseReasons;
    }

    static bool isOfflineOnly(const RuntimePauseController::Snapshot& snapshot) {
        return !snapshot.manualPauseRequested &&
               effectiveAutoPauseReasons(snapshot) ==
                   RuntimePauseController::AutoPauseReasons(
                       RuntimePauseController::AutoPauseReason::Offline);
    }

    static bool hasSuppressedActiveAutoPause(const RuntimePauseController::Snapshot& snapshot) {
        return snapshot.autoPauseEnabled &&
               (snapshot.activeAutoPauseReasons & snapshot.suppressedAutoPauseReasons) !=
                   RuntimePauseController::AutoPauseReasons();
    }

    static bool shouldSuppressTransitionFrom(const RuntimePauseController::Snapshot& preSuspend,
                                             const RuntimePauseController::Snapshot& current) {
        return !preSuspend.effectivePause && isOfflineOnly(current);
    }

    RuntimePauseController::Snapshot m_preSuspend;
    bool m_hasPreSuspendState = false;
    bool m_waitingForWake = false;
    bool m_pendingPauseNotificationSuppression = false;
    bool m_pendingResumeNotificationSuppression = false;
    qint64 m_observePauseTransitionUntilMs = 0;
};

#endif  // WAKERESUMENOTIFICATIONSUPPRESSOR_H