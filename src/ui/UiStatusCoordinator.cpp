/**
 * @file UiStatusCoordinator.cpp
 * @brief Shared status coordination for tray and main window.
 */

#include "UiStatusCoordinator.h"

#include <QStringList>
#include <QTimer>
#include <algorithm>

#include "auth/GoogleAuthManager.h"
#include "sync/ChangeProcessor.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncActionQueue.h"

namespace {

QString uploadActivityKey(const QString& fileId, const QString& path) {
    if (!fileId.isEmpty()) {
        return fileId;
    }
    if (!path.isEmpty()) {
        return path;
    }
    return QString();
}

bool snapshotsEqual(const UiStatusSnapshot& lhs, const UiStatusSnapshot& rhs) {
    return lhs.resolvedPriority == rhs.resolvedPriority &&
           lhs.resolvedStatusText == rhs.resolvedStatusText &&
           lhs.combinedStatusText == rhs.combinedStatusText &&
           lhs.mirrorStatusText == rhs.mirrorStatusText &&
           lhs.fuseStatusText == rhs.fuseStatusText && lhs.pendingActions == rhs.pendingActions &&
           lhs.authenticated == rhs.authenticated && lhs.authExpired == rhs.authExpired;
}

bool isStickyMirrorStatus(const QString& status) {
    return status.contains(QStringLiteral("Offline"), Qt::CaseInsensitive) ||
           status.contains(QStringLiteral("Recovering"), Qt::CaseInsensitive) ||
           status.contains(QStringLiteral("Scanning"), Qt::CaseInsensitive) ||
           status.contains(QStringLiteral("Fetching"), Qt::CaseInsensitive) ||
           status.contains(QStringLiteral("Error"), Qt::CaseInsensitive) ||
           status.contains(QStringLiteral("Failed"), Qt::CaseInsensitive) ||
           status.contains(QStringLiteral("Authentication"), Qt::CaseInsensitive);
}

bool isAuthMirrorStatus(const QString& status) {
    return status.contains(QStringLiteral("Authentication"), Qt::CaseInsensitive);
}

bool isMeaningfulStatus(const QString& status) {
    return !status.isEmpty() && status.compare(QStringLiteral("Idle"), Qt::CaseInsensitive) != 0;
}

bool isIdleLikeFuseStatus(const QString& status) {
    return status.isEmpty() ||
           status.compare(QStringLiteral("Mounted"), Qt::CaseInsensitive) == 0 ||
           status.compare(QStringLiteral("Idle"), Qt::CaseInsensitive) == 0;
}

}  // namespace

UiStatusCoordinator::UiStatusCoordinator(GoogleAuthManager* authManager, bool mirrorEnabled,
                                         RuntimePauseController* pauseController, QObject* parent)
    : QObject(parent),
      m_authManager(authManager),
      m_pauseController(pauseController),
      m_mirrorEnabled(mirrorEnabled),
      m_fuseEnabled(true),
      m_changeProcessorState(ChangeProcessor::State::Stopped),
      m_statusTimer(nullptr),
      m_fuseIdleTimer(nullptr),
      m_pendingActions(0),
      m_downloadActiveOps(0),
      m_uploadActivityAuthoritativeSeen(false),
      m_uploadActivityActive(false),
      m_metadataRefreshActive(false),
      m_authenticated(false),
      m_authExpired(false),
      m_hasConflicts(false),
      m_storagePercent(-1.0),
      m_mirrorPriority(UiStatusPriority::Idle),
      m_fusePriority(UiStatusPriority::Idle),
      m_globalPriority(UiStatusPriority::Idle) {
    if (m_authManager) {
        m_authenticated = m_authManager->isAuthenticated();

        connect(m_authManager, &GoogleAuthManager::authenticated, this,
                [this]() { updateAuthState(true); });
        connect(m_authManager, &GoogleAuthManager::loggedOut, this,
                [this]() { updateAuthState(false); });
    }

    if (m_pauseController) {
        connect(m_pauseController, &RuntimePauseController::stateChanged, this,
                &UiStatusCoordinator::refreshMirrorStatus);
    }

    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &UiStatusCoordinator::refreshMirrorStatus);
    m_statusTimer->start(5000);

    m_fuseIdleTimer = new QTimer(this);
    m_fuseIdleTimer->setSingleShot(true);
    m_fuseIdleTimer->setInterval(1500);
    connect(m_fuseIdleTimer, &QTimer::timeout, this, [this]() {
        if (!hasFuseActivity()) {
            updateFuseStatus(QStringLiteral("Mounted"));
        }
    });

    refreshMirrorStatusInternal();
}

UiStatusSnapshot UiStatusCoordinator::snapshot() const {
    UiStatusSnapshot status;
    status.resolvedPriority = effectivePriority();
    status.resolvedStatusText = effectiveStatusText();
    status.combinedStatusText = combinedStatusText();
    status.mirrorStatusText = m_mirrorStatusText;
    status.fuseStatusText = m_fuseStatusText;
    status.pendingActions = m_pendingActions;
    status.authenticated = m_authenticated;
    status.authExpired = m_authExpired;
    return status;
}

void UiStatusCoordinator::setFuseEnabled(bool enabled) {
    const UiStatusSnapshot before = snapshot();

    m_fuseEnabled = enabled;
    if (!m_fuseEnabled) {
        clearFuseActivityState();
        m_fuseBaseStatusText.clear();
    }

    refreshFuseStatusFromState();
    refreshMirrorStatusInternal();
    emitIfChanged(before);
}

UiStatusPriority UiStatusCoordinator::priorityFromStatusText(const QString& status) {
    if (status.contains(QStringLiteral("expired"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Authentication"), Qt::CaseInsensitive)) {
        return UiStatusPriority::AuthExpired;
    }
    if (status.contains(QStringLiteral("Error"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Failed"), Qt::CaseInsensitive)) {
        return UiStatusPriority::Error;
    }
    if (status.contains(QStringLiteral("Not connected"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Offline"), Qt::CaseInsensitive)) {
        return UiStatusPriority::Offline;
    }
    if (status.contains(QStringLiteral("Warning"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Conflict"), Qt::CaseInsensitive)) {
        return UiStatusPriority::Warning;
    }
    if (status.contains(QStringLiteral("Paused"), Qt::CaseInsensitive)) {
        return UiStatusPriority::Paused;
    }
    if (status.contains(QStringLiteral("disabled"), Qt::CaseInsensitive)) {
        return UiStatusPriority::Paused;
    }
    if (status.contains(QStringLiteral("Syncing"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Uploading"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Downloading"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Scanning"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Fetching"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Flushing"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Refreshing"), Qt::CaseInsensitive)) {
        return UiStatusPriority::Syncing;
    }
    return UiStatusPriority::Idle;
}

QString UiStatusCoordinator::iconForPriority(UiStatusPriority priority) {
    switch (priority) {
        case UiStatusPriority::AuthExpired:
            return QStringLiteral("auth-expired.svg");
        case UiStatusPriority::Error:
            return QStringLiteral("error.svg");
        case UiStatusPriority::Warning:
            return QStringLiteral("warn.svg");
        case UiStatusPriority::Offline:
            return QStringLiteral("no-connection.svg");
        case UiStatusPriority::CriticalStorage:
            return QStringLiteral("critical-low-storage.svg");
        case UiStatusPriority::LowStorage:
            return QStringLiteral("low-storage.svg");
        case UiStatusPriority::Paused:
            return QStringLiteral("paused.svg");
        case UiStatusPriority::Syncing:
            return QStringLiteral("sync-active.svg");
        case UiStatusPriority::Idle:
        default:
            return QStringLiteral("drive-idle.svg");
    }
}

void UiStatusCoordinator::updateMirrorStatus(const QString& status) {
    const UiStatusSnapshot before = snapshot();

    if (isStickyMirrorStatus(status)) {
        m_mirrorOverrideStatus = status;
    } else {
        m_mirrorOverrideStatus.clear();
    }

    setMirrorStatusInternal(status);
    emitIfChanged(before);
}

void UiStatusCoordinator::updateFuseStatus(const QString& status) {
    const UiStatusSnapshot before = snapshot();

    if (status.compare(QStringLiteral("Idle"), Qt::CaseInsensitive) == 0 ||
        status.compare(QStringLiteral("Mounted"), Qt::CaseInsensitive) == 0) {
        clearFuseActivityState();
    }

    m_fuseBaseStatusText = status;
    refreshFuseStatusFromState();
    emitIfChanged(before);
}

void UiStatusCoordinator::updateAuthState(bool authenticated) {
    const UiStatusSnapshot before = snapshot();

    m_authStateExplicit = true;
    m_authenticated = authenticated;
    if (authenticated) {
        m_authExpired = false;
        m_authExpiredReason.clear();
        m_mirrorOverrideStatus.clear();
    } else {
        m_authExpired = false;
        m_authExpiredReason.clear();
        m_mirrorOverrideStatus.clear();
        if (m_mirrorEnabled) {
            setMirrorStatusInternal(QStringLiteral("Not connected"));
        }
        clearFuseActivityState();
        m_fuseBaseStatusText = QStringLiteral("Idle");
        refreshFuseStatusFromState();
    }

    recalcGlobalPriority();
    if (authenticated) {
        refreshMirrorStatusInternal();
    }
    emitIfChanged(before);
}

void UiStatusCoordinator::setAuthExpired(const QString& reason) {
    const UiStatusSnapshot before = snapshot();

    m_authStateExplicit = true;
    m_authenticated = false;
    m_authExpired = true;
    m_authExpiredReason = reason;
    m_mirrorOverrideStatus = QStringLiteral("Authentication expired");
    setMirrorStatusInternal(m_mirrorOverrideStatus);
    clearFuseActivityState();
    m_fuseBaseStatusText = QStringLiteral("Idle");
    refreshFuseStatusFromState();
    recalcGlobalPriority();

    emitIfChanged(before);
}

void UiStatusCoordinator::setHasConflicts(bool hasConflicts) {
    const UiStatusSnapshot before = snapshot();
    m_hasConflicts = hasConflicts;
    recalcGlobalPriority();
    emitIfChanged(before);
}

void UiStatusCoordinator::updateStorageInfo(qint64 storageUsed, qint64 storageLimit) {
    const UiStatusSnapshot before = snapshot();

    if (storageLimit <= 0) {
        m_storagePercent = -1.0;
    } else {
        m_storagePercent = (storageUsed * 100.0) / storageLimit;
    }

    recalcGlobalPriority();
    emitIfChanged(before);
}

void UiStatusCoordinator::onDownloadStarted(const QString& fileId) {
    Q_UNUSED(fileId)

    const UiStatusSnapshot before = snapshot();
    ++m_downloadActiveOps;
    updateFuseStatusForActivityChange();
    emitIfChanged(before);
}

void UiStatusCoordinator::onDownloadFinished(const QString& fileId) {
    Q_UNUSED(fileId)

    const UiStatusSnapshot before = snapshot();
    m_downloadActiveOps = std::max(0, m_downloadActiveOps - 1);
    updateFuseStatusForActivityChange();
    emitIfChanged(before);
}

void UiStatusCoordinator::onUploadStarted(const QString& fileId, const QString& path) {
    const UiStatusSnapshot before = snapshot();

    const QString key = uploadActivityKey(fileId, path);
    if (key.isEmpty()) {
        m_activeUploadKeys.clear();
    } else {
        m_activeUploadKeys.insert(key);
    }

    updateFuseStatusForActivityChange();
    emitIfChanged(before);
}

void UiStatusCoordinator::onUploadFinished(const QString& fileId, const QString& path) {
    const UiStatusSnapshot before = snapshot();

    const QString key = uploadActivityKey(fileId, path);
    if (key.isEmpty()) {
        m_activeUploadKeys.clear();
    } else {
        m_activeUploadKeys.remove(key);
    }

    updateFuseStatusForActivityChange();
    emitIfChanged(before);
}

void UiStatusCoordinator::onUploadActivityChanged(bool active) {
    const UiStatusSnapshot before = snapshot();

    m_uploadActivityAuthoritativeSeen = true;
    m_uploadActivityActive = active;
    if (!active) {
        m_activeUploadKeys.clear();
    }

    updateFuseStatusForActivityChange();
    emitIfChanged(before);
}

void UiStatusCoordinator::onDirtyFilesFlushed(int count) {
    Q_UNUSED(count)

    updateFuseStatus(QStringLiteral("Mounted"));
}

void UiStatusCoordinator::onMetadataRefreshStarted() {
    const UiStatusSnapshot before = snapshot();

    m_metadataRefreshActive = true;
    updateFuseStatusForActivityChange();
    emitIfChanged(before);
}

void UiStatusCoordinator::onMetadataRefreshFinished() {
    const UiStatusSnapshot before = snapshot();

    m_metadataRefreshActive = false;
    updateFuseStatusForActivityChange();

    emitIfChanged(before);
}

void UiStatusCoordinator::onMetadataRefreshFailed(const QString& error) {
    Q_UNUSED(error)

    onMetadataRefreshFinished();
}

void UiStatusCoordinator::updatePendingActions(int count) {
    const UiStatusSnapshot before = snapshot();

    m_pendingActions = std::max(0, count);
    refreshMirrorStatusInternal();

    emitIfChanged(before);
}

void UiStatusCoordinator::updateMirrorProcessorState(ChangeProcessor::State state) {
    const UiStatusSnapshot before = snapshot();

    m_changeProcessorState = state;
    refreshMirrorStatusInternal();

    emitIfChanged(before);
}

void UiStatusCoordinator::refreshMirrorStatus() {
    const UiStatusSnapshot before = snapshot();
    refreshMirrorStatusInternal();
    emitIfChanged(before);
}

void UiStatusCoordinator::emitIfChanged(const UiStatusSnapshot& before) {
    const UiStatusSnapshot after = snapshot();
    if (!snapshotsEqual(before, after)) {
        emit statusChanged();
    }
}

void UiStatusCoordinator::recalcGlobalPriority() {
    UiStatusPriority priority = UiStatusPriority::Idle;

    if (m_authExpired) {
        priority = std::max(priority, UiStatusPriority::AuthExpired);
    }
    if (m_hasConflicts) {
        priority = std::max(priority, UiStatusPriority::Warning);
    }
    if (m_storagePercent >= 90.0) {
        priority = std::max(priority, UiStatusPriority::CriticalStorage);
    } else if (m_storagePercent >= 75.0) {
        priority = std::max(priority, UiStatusPriority::LowStorage);
    }

    m_globalPriority = priority;
}

void UiStatusCoordinator::refreshMirrorStatusInternal() {
    if (m_authStateExplicit && !m_authenticated && !m_authExpired) {
        setMirrorStatusInternal(QStringLiteral("Not connected"));
        return;
    }

    if (!m_mirrorEnabled && !m_fuseEnabled) {
        setMirrorStatusInternal(QString());
        return;
    }

    if (m_authenticated && m_pauseController && m_pauseController->isEffectivelyPaused()) {
        setMirrorStatusInternal(m_pauseController->effectiveStatusText());
        return;
    }

    if (!m_mirrorOverrideStatus.isEmpty()) {
        if (isAuthMirrorStatus(m_mirrorOverrideStatus) || m_mirrorEnabled) {
            setMirrorStatusInternal(m_mirrorOverrideStatus);
            return;
        }
        m_mirrorOverrideStatus.clear();
    }

    if (!m_mirrorEnabled) {
        setMirrorStatusInternal(QString());
        return;
    }

    QString status;
    switch (m_changeProcessorState) {
        case ChangeProcessor::State::Running:
            if (m_pendingActions > 0) {
                status = QStringLiteral("Syncing... (%1 pending)").arg(m_pendingActions);
            } else {
                status = QStringLiteral("Up to date");
            }
            break;
        case ChangeProcessor::State::Paused:
            status = QStringLiteral("Paused");
            break;
        case ChangeProcessor::State::Stopped:
            status = m_authenticated ? QStringLiteral("Stopped") : QStringLiteral("Not connected");
            break;
    }

    setMirrorStatusInternal(status);
}

void UiStatusCoordinator::setMirrorStatusInternal(const QString& status) {
    m_mirrorStatusText = status;
    m_mirrorPriority = priorityFromStatusText(status);
}

void UiStatusCoordinator::refreshFuseStatusFromState() {
    QString status = m_fuseBaseStatusText;

    if (!m_fuseEnabled) {
        status.clear();
    } else if (m_metadataRefreshActive) {
        status = QStringLiteral("Refreshing metadata");
    } else if (effectiveUploadActive()) {
        status = QStringLiteral("Uploading...");
    } else if (m_downloadActiveOps > 0) {
        status = QStringLiteral("Downloading...");
    }

    setFuseStatusInternal(status);
}

void UiStatusCoordinator::updateFuseStatusForActivityChange() {
    if (!m_fuseEnabled) {
        m_fuseIdleTimer->stop();
        refreshFuseStatusFromState();
        return;
    }

    if (hasFuseActivity()) {
        m_fuseIdleTimer->stop();
        refreshFuseStatusFromState();
        return;
    }

    if (shouldDelayFuseIdleTransition()) {
        m_fuseIdleTimer->start();
        return;
    }

    m_fuseIdleTimer->stop();
    refreshFuseStatusFromState();
}

void UiStatusCoordinator::setFuseStatusInternal(const QString& status) {
    m_fuseStatusText = status;
    m_fusePriority = priorityFromStatusText(status);
}

void UiStatusCoordinator::clearFuseActivityState() {
    m_downloadActiveOps = 0;
    m_uploadActivityAuthoritativeSeen = false;
    m_uploadActivityActive = false;
    m_activeUploadKeys.clear();
    m_metadataRefreshActive = false;
    m_fuseIdleTimer->stop();
}

bool UiStatusCoordinator::effectiveUploadActive() const {
    if (m_uploadActivityAuthoritativeSeen) {
        return m_uploadActivityActive;
    }

    return !m_activeUploadKeys.isEmpty();
}

bool UiStatusCoordinator::hasFuseActivity() const {
    return m_metadataRefreshActive || effectiveUploadActive() || m_downloadActiveOps > 0;
}

bool UiStatusCoordinator::shouldDelayFuseIdleTransition() const {
    return isIdleLikeFuseStatus(m_fuseBaseStatusText);
}

UiStatusPriority UiStatusCoordinator::effectivePriority() const {
    if (m_authExpired) {
        return UiStatusPriority::AuthExpired;
    }

    if (m_authenticated && !m_mirrorEnabled && !m_fuseEnabled) {
        return UiStatusPriority::Paused;
    }

    UiStatusPriority effective = m_mirrorPriority;
    if (m_fusePriority > effective) {
        effective = m_fusePriority;
    }
    if (m_globalPriority > effective) {
        effective = m_globalPriority;
    }
    return effective;
}

QString UiStatusCoordinator::combinedStatusText() const {
    if (m_authExpired) {
        if (m_authExpiredReason.isEmpty()) {
            return QStringLiteral("Authentication expired");
        }
        return QStringLiteral("Authentication expired (%1)").arg(m_authExpiredReason);
    }

    const bool mirrorMeaningful = isMeaningfulStatus(m_mirrorStatusText);
    const bool fuseMeaningful = isMeaningfulStatus(m_fuseStatusText);

    if (mirrorMeaningful && fuseMeaningful) {
        return QStringLiteral("Mirror: %1 | FUSE: %2").arg(m_mirrorStatusText, m_fuseStatusText);
    }
    if (mirrorMeaningful) {
        return m_mirrorStatusText;
    }
    if (fuseMeaningful) {
        return m_fuseStatusText;
    }
    if (!m_authenticated) {
        return QStringLiteral("Not connected");
    }
    if (!m_mirrorEnabled && !m_fuseEnabled) {
        return QStringLiteral("Sync disabled");
    }
    return QStringLiteral("Idle");
}

QString UiStatusCoordinator::effectiveStatusText() const {
    const UiStatusPriority priority = effectivePriority();

    if (priority == UiStatusPriority::AuthExpired) {
        return combinedStatusText();
    }
    if (m_mirrorPriority > m_fusePriority && m_mirrorPriority >= m_globalPriority &&
        isMeaningfulStatus(m_mirrorStatusText)) {
        return m_mirrorStatusText;
    }
    if (m_fusePriority > m_mirrorPriority && m_fusePriority >= m_globalPriority &&
        isMeaningfulStatus(m_fuseStatusText)) {
        return m_fuseStatusText;
    }
    return combinedStatusText();
}