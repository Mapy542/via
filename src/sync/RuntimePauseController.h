/**
 * @file RuntimePauseController.h
 * @brief Shared runtime pause policy for Drive API access.
 */

#ifndef RUNTIMEPAUSECONTROLLER_H
#define RUNTIMEPAUSECONTROLLER_H

#include <QFlags>
#include <QObject>
#include <QString>

class RuntimePauseController : public QObject {
    Q_OBJECT

   public:
    enum class AutoPauseReason {
        Offline = 0x1,
        MeteredNetwork = 0x2,
        PowerSaver = 0x4,
    };
    Q_ENUM(AutoPauseReason)

    Q_DECLARE_FLAGS(AutoPauseReasons, AutoPauseReason)
    Q_FLAG(AutoPauseReasons)

    struct Snapshot {
        bool manualPauseRequested = false;
        AutoPauseReasons activeAutoPauseReasons;
        AutoPauseReasons suppressedAutoPauseReasons;
        bool effectivePause = false;
    };

    explicit RuntimePauseController(QObject* parent = nullptr);

    Snapshot snapshot() const;

    bool isEffectivelyPaused() const;
    bool isDriveApiAllowed() const;
    bool isManualPauseRequested() const;
    bool hasEffectiveAutoPauseReason(AutoPauseReason reason) const;
    AutoPauseReasons activeAutoPauseReasons() const;
    AutoPauseReasons suppressedAutoPauseReasons() const;
    AutoPauseReasons effectiveAutoPauseReasons() const;

    QString effectiveStatusText() const;
    QString pauseActionText() const;
    QString pauseNotificationTitle() const;
    QString pauseNotificationMessage() const;
    QString resumeNotificationMessage() const;
    QString blockedOperationMessage(const QString& action) const;

   public slots:
    void requestManualPause();
    void requestManualResume();
    void togglePause();
    void setAutoPauseReasonActive(AutoPauseReason reason, bool active);

   signals:
    void stateChanged();
    void effectivePauseChanged(bool paused);

   private:
    static QString reasonLabel(AutoPauseReason reason);
    static QString blockedStatePhrase(const Snapshot& snapshot);
    static QString joinedReasonLabels(AutoPauseReasons reasons);
    static bool snapshotsEqual(const Snapshot& lhs, const Snapshot& rhs);

    Snapshot snapshotLocked() const;
    void commitStateChange(const Snapshot& before);

    Snapshot m_state;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(RuntimePauseController::AutoPauseReasons)

#endif  // RUNTIMEPAUSECONTROLLER_H