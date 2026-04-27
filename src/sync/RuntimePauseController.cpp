/**
 * @file RuntimePauseController.cpp
 * @brief Shared runtime pause policy for Drive API access.
 */

#include "RuntimePauseController.h"

#include <QStringList>

namespace {

bool hasAnyReasons(RuntimePauseController::AutoPauseReasons reasons) {
    return reasons != RuntimePauseController::AutoPauseReasons();
}

}  // namespace

RuntimePauseController::RuntimePauseController(QObject* parent) : QObject(parent) {}

RuntimePauseController::Snapshot RuntimePauseController::snapshot() const {
    return snapshotLocked();
}

bool RuntimePauseController::isEffectivelyPaused() const { return snapshotLocked().effectivePause; }

bool RuntimePauseController::isDriveApiAllowed() const { return !isEffectivelyPaused(); }

bool RuntimePauseController::isManualPauseRequested() const {
    return snapshotLocked().manualPauseRequested;
}

bool RuntimePauseController::hasEffectiveAutoPauseReason(AutoPauseReason reason) const {
    return effectiveAutoPauseReasons().testFlag(reason);
}

RuntimePauseController::AutoPauseReasons RuntimePauseController::activeAutoPauseReasons() const {
    return snapshotLocked().activeAutoPauseReasons;
}

RuntimePauseController::AutoPauseReasons RuntimePauseController::suppressedAutoPauseReasons()
    const {
    return snapshotLocked().suppressedAutoPauseReasons;
}

RuntimePauseController::AutoPauseReasons RuntimePauseController::effectiveAutoPauseReasons() const {
    const Snapshot current = snapshotLocked();
    return current.activeAutoPauseReasons & ~current.suppressedAutoPauseReasons;
}

QString RuntimePauseController::effectiveStatusText() const {
    const Snapshot current = snapshotLocked();
    if (current.manualPauseRequested) {
        return QStringLiteral("Paused");
    }

    const AutoPauseReasons effectiveReasons =
        current.activeAutoPauseReasons & ~current.suppressedAutoPauseReasons;
    if (!hasAnyReasons(effectiveReasons)) {
        return QString();
    }

    if (effectiveReasons.testFlag(AutoPauseReason::Offline)) {
        return QStringLiteral("Offline");
    }

    return QStringLiteral("Paused (%1)").arg(joinedReasonLabels(effectiveReasons));
}

QString RuntimePauseController::pauseActionText() const {
    return isEffectivelyPaused() ? QStringLiteral("Resume Sync") : QStringLiteral("Pause Sync");
}

QString RuntimePauseController::pauseNotificationTitle() const {
    if (effectiveStatusText().compare(QStringLiteral("Offline"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Offline");
    }
    return QStringLiteral("Sync Paused");
}

QString RuntimePauseController::pauseNotificationMessage() const {
    const Snapshot current = snapshotLocked();
    if (current.manualPauseRequested) {
        return QStringLiteral(
            "Sync is paused manually. Uploads, remote polling, and Drive-backed mutations will "
            "resume when you unpause.");
    }

    const AutoPauseReasons effectiveReasons =
        current.activeAutoPauseReasons & ~current.suppressedAutoPauseReasons;
    if (effectiveReasons.testFlag(AutoPauseReason::Offline)) {
        return QStringLiteral(
            "Network access is unavailable. Via will keep cached content available, but remote "
            "create, rename, move, trash, and delete operations are blocked until the connection "
            "returns.");
    }

    return QStringLiteral("Drive API access is paused because of %1.")
        .arg(joinedReasonLabels(effectiveReasons));
}

QString RuntimePauseController::resumeNotificationMessage() const {
    const Snapshot current = snapshotLocked();
    const AutoPauseReasons suppressedReasons =
        current.activeAutoPauseReasons & current.suppressedAutoPauseReasons;

    if (hasAnyReasons(suppressedReasons)) {
        return QStringLiteral(
            "Sync resumed. Current auto-pause conditions are overridden until they change.");
    }

    return QStringLiteral("Sync resumed. Drive access is available again.");
}

QString RuntimePauseController::blockedOperationMessage(const QString& action) const {
    const Snapshot current = snapshotLocked();
    return QStringLiteral(
               "Cannot %1 while %2. Via only allows cached reads and edits to already cached files "
               "until Drive access resumes.")
        .arg(action, blockedStatePhrase(current));
}

void RuntimePauseController::requestManualPause() {
    const Snapshot before = snapshotLocked();
    m_state.manualPauseRequested = true;
    commitStateChange(before);
}

void RuntimePauseController::requestManualResume() {
    const Snapshot before = snapshotLocked();
    m_state.manualPauseRequested = false;
    m_state.suppressedAutoPauseReasons |= m_state.activeAutoPauseReasons;
    commitStateChange(before);
}

void RuntimePauseController::togglePause() {
    if (isEffectivelyPaused()) {
        requestManualResume();
    } else {
        requestManualPause();
    }
}

void RuntimePauseController::setAutoPauseReasonActive(AutoPauseReason reason, bool active) {
    const Snapshot before = snapshotLocked();
    if (active) {
        m_state.activeAutoPauseReasons |= reason;
    } else {
        m_state.activeAutoPauseReasons &= ~AutoPauseReasons(reason);
        m_state.suppressedAutoPauseReasons &= ~AutoPauseReasons(reason);
    }
    commitStateChange(before);
}

QString RuntimePauseController::reasonLabel(AutoPauseReason reason) {
    switch (reason) {
        case AutoPauseReason::Offline:
            return QStringLiteral("offline");
        case AutoPauseReason::MeteredNetwork:
            return QStringLiteral("metered network");
        case AutoPauseReason::PowerSaver:
            return QStringLiteral("power saver");
    }

    return QStringLiteral("unknown reason");
}

QString RuntimePauseController::blockedStatePhrase(const Snapshot& snapshot) {
    if (snapshot.manualPauseRequested) {
        return QStringLiteral("sync is paused");
    }

    const AutoPauseReasons effectiveReasons =
        snapshot.activeAutoPauseReasons & ~snapshot.suppressedAutoPauseReasons;
    if (effectiveReasons.testFlag(AutoPauseReason::Offline)) {
        return QStringLiteral("Via is offline");
    }
    if (hasAnyReasons(effectiveReasons)) {
        return QStringLiteral("sync is paused for %1").arg(joinedReasonLabels(effectiveReasons));
    }

    return QStringLiteral("Drive access is unavailable");
}

QString RuntimePauseController::joinedReasonLabels(AutoPauseReasons reasons) {
    QStringList labels;
    if (reasons.testFlag(AutoPauseReason::Offline)) {
        labels << reasonLabel(AutoPauseReason::Offline);
    }
    if (reasons.testFlag(AutoPauseReason::MeteredNetwork)) {
        labels << reasonLabel(AutoPauseReason::MeteredNetwork);
    }
    if (reasons.testFlag(AutoPauseReason::PowerSaver)) {
        labels << reasonLabel(AutoPauseReason::PowerSaver);
    }
    return labels.join(QStringLiteral(", "));
}

bool RuntimePauseController::snapshotsEqual(const Snapshot& lhs, const Snapshot& rhs) {
    return lhs.manualPauseRequested == rhs.manualPauseRequested &&
           lhs.activeAutoPauseReasons == rhs.activeAutoPauseReasons &&
           lhs.suppressedAutoPauseReasons == rhs.suppressedAutoPauseReasons &&
           lhs.effectivePause == rhs.effectivePause;
}

RuntimePauseController::Snapshot RuntimePauseController::snapshotLocked() const {
    Snapshot current = m_state;
    current.effectivePause =
        current.manualPauseRequested ||
        hasAnyReasons(current.activeAutoPauseReasons & ~current.suppressedAutoPauseReasons);
    return current;
}

void RuntimePauseController::commitStateChange(const Snapshot& before) {
    const Snapshot after = snapshotLocked();
    m_state.effectivePause = after.effectivePause;

    if (snapshotsEqual(before, after)) {
        return;
    }

    if (before.effectivePause != after.effectivePause) {
        emit effectivePauseChanged(after.effectivePause);
    }
    emit stateChanged();
}