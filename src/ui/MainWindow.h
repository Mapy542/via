/**
 * @file MainWindow.h
 * @brief Main application window
 *
 * The main window displays synchronization status, recent files,
 * and provides access to settings and other features.
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

class QAction;

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

class GoogleAuthManager;
class GoogleDriveClient;
class MirrorSyncRuntime;
class NotificationManager;
class SettingsWindow;
class UiStatusCoordinator;
class RuntimePauseController;

/**
 * @class MainWindow
 * @brief The main application window
 *
 * Provides the primary user interface showing sync status,
 * recent activity, and quick access to common functions.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

   public:
    /**
     * @brief Construct the main window
     * @param authManager Pointer to the authentication manager
     * @param driveClient Pointer to the Google Drive API client
     * @param mirrorSyncRuntime Pointer to the mirror runtime facade
     * @param notificationManager Pointer to the notification manager
     * @param parent Parent widget
     */
    explicit MainWindow(GoogleAuthManager* authManager, GoogleDriveClient* driveClient,
                        MirrorSyncRuntime* mirrorSyncRuntime,
                        RuntimePauseController* pauseController,
                        UiStatusCoordinator* statusCoordinator,
                        NotificationManager* notificationManager, bool canPauseSync = true,
                        bool canRequestFullSync = true, QWidget* parent = nullptr);

    ~MainWindow() override;

   public slots:
    /**
     * @brief Update the sync status display
     * @param status Current sync status message
     */
    void updateSyncStatus(const QString& status);

    /**
     * @brief Update the sync progress
     * @param current Current progress value
     * @param total Total progress value
     */
    void updateSyncProgress(qint64 current, qint64 total);

    /**
     * @brief Update the pending actions count display
     * @param count Number of pending sync actions
     */
    void updatePendingActions(int count);

    /**
     * @brief Add an item to the recent activity list
     * @param activity Activity description
     */
    void addRecentActivity(const QString& activity);

    /**
     * @brief Update the authentication state display
     * @param authenticated Whether the user is authenticated
     */
    void updateAuthState(bool authenticated);

    /**
     * @brief Put UI into auth-expired state until user re-authenticates
     * @param reason Human-readable failure reason
     */
    void setAuthExpired(const QString& reason);

   signals:
    /**
     * @brief Emitted when the user requests sign-out
     *
     * Connected in main.cpp so the dirty-file count and confirmation
     * dialog can be shown with full context before logout() is called.
     */
    void logoutRequested();

    /**
     * @brief Emitted when the user requests a restart-gated cache purge
     *
     * Connected in main.cpp so the app can queue the purge, unmount FUSE,
     * relaunch, and quit.
     */
    void clearCacheRequested();

    /**
     * @brief Emitted when the user accepts a settings-driven restart
     *
     * Connected in main.cpp to orchestrate safe FUSE unmount,
     * pending purge flags, relaunch, and quit.
     */
    void restartRequested();

    /**
     * @brief Emitted when the user requests a full mirror sync
     */
    void fullSyncRequested();

   protected:
    /**
     * @brief Handle window close event
     * @param event Close event
     */
    void closeEvent(QCloseEvent* event) override;

   private slots:
    void onLoginClicked();
    void onLogoutClicked();
    void onSettingsClicked();
    void onOpenFolderClicked();
    void onPauseSyncClicked();
    void onRefreshClicked();
    void applyStatusSnapshot();
    void applyPauseControllerState();

   private:
    void setupUi();
    void setupMenuBar();
    void connectSignals();
    void updateSyncActionAvailability(bool authenticated);
    void updatePauseButton(bool paused);

    GoogleAuthManager* m_authManager;
    GoogleDriveClient* m_driveClient;
    MirrorSyncRuntime* m_mirrorSyncRuntime;
    RuntimePauseController* m_pauseController;
    UiStatusCoordinator* m_statusCoordinator;
    NotificationManager* m_notificationManager;
    SettingsWindow* m_settingsWindow;

    // UI elements
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;

    // Status section
    QGroupBox* m_statusGroup;
    QLabel* m_statusIcon;
    QLabel* m_statusLabel;
    QLabel* m_pendingActionsLabel;
    QProgressBar* m_progressBar;

    // Account section
    QGroupBox* m_accountGroup;
    QLabel* m_accountLabel;
    QPushButton* m_loginButton;
    QPushButton* m_logoutButton;

    // Actions section
    QGroupBox* m_actionsGroup;
    QPushButton* m_openFolderButton;
    QPushButton* m_pauseSyncButton;
    QPushButton* m_refreshButton;
    QPushButton* m_settingsButton;
    QAction* m_pauseMenuAction;
    QAction* m_syncNowMenuAction;

    // Recent activity section
    QGroupBox* m_activityGroup;
    QListWidget* m_activityList;

    bool m_syncPaused;
    bool m_authExpired;
    bool m_canPauseSync;
    bool m_canRequestFullSync;
};

#endif  // MAINWINDOW_H
