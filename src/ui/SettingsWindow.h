/**
 * @file SettingsWindow.h
 * @brief Settings/Preferences window
 *
 * Allows users to configure the application settings including
 * account management, bandwidth limits, sync options, and startup behavior.
 */

#ifndef SETTINGSWINDOW_H
#define SETTINGSWINDOW_H

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

class GoogleAuthManager;
class GoogleDriveClient;
class SyncActionQueue;
class ChangeProcessor;

/**
 * @class SettingsWindow
 * @brief Settings and preferences dialog
 *
 * Provides UI for configuring:
 * - Account settings (login/logout)
 * - Bandwidth limits for uploads/downloads
 * - Startup behavior
 */
class SettingsWindow : public QDialog {
    Q_OBJECT

   public:
    /**
     * @brief Construct the settings window
     * @param authManager Pointer to the authentication manager
     * @param syncActionQueue Pointer to the sync action queue
     * @param changeProcessor Pointer to the change processor/conflict resolver
     * @param driveClient Pointer to the Google Drive client
     * @param parent Parent widget
     */
    explicit SettingsWindow(GoogleAuthManager* authManager, SyncActionQueue* syncActionQueue,
                            ChangeProcessor* changeProcessor, GoogleDriveClient* driveClient,
                            QWidget* parent = nullptr);

    ~SettingsWindow() override;

   protected:
    void showEvent(QShowEvent* event) override;

   signals:
    /**
     * @brief Emitted when settings are changed
     */
    void settingsChanged();

    /**
     * @brief Emitted when the user clicks "Clear Cache"
     *
     * Owner should disconnect/stop FileCache before this signal fires
     * and reinitialise afterwards.
     */
    void clearCacheRequested();

   public slots:
    /**
     * @brief Load settings from storage
     */
    void loadSettings();

    /**
     * @brief Save settings to storage
     */
    void saveSettings();

   private slots:
    void onApplyClicked();
    void onCancelClicked();
    void onBrowseFolderClicked();

    void onSaveCredentialsClicked();
    void onStorageInfoReceived(qint64 storageUsed, qint64 storageLimit);

   private:
    void setupUi();
    void setupAccountTab();
    void setupSyncTab();
    void setupAdvancedTab();
    void updateStorageInfo();

    GoogleAuthManager* m_authManager;
    SyncActionQueue* m_syncActionQueue;
    ChangeProcessor* m_changeProcessor;
    GoogleDriveClient* m_driveClient;
    QSettings m_settings;

    // Tab widget
    QTabWidget* m_tabWidget;

    // Account tab widgets
    QWidget* m_accountTab;
    QLabel* m_accountStatus;
    QPushButton* m_loginButton;
    QPushButton* m_logoutButton;
    QLineEdit* m_clientIdEdit;
    QLineEdit* m_clientSecretEdit;
    QPushButton* m_saveCredentialsButton;
    QLabel* m_storageLabel;

    // Sync tab widgets
    QWidget* m_syncTab;
    QLineEdit* m_syncFolderEdit;
    QPushButton* m_browseFolderButton;
    QComboBox* m_syncModeCombo;
    QComboBox* m_conflictResolutionCombo;

    // Advanced tab widgets
    QWidget* m_advancedTab;
    QCheckBox* m_startOnLoginCheck;
    QCheckBox* m_showNotificationsCheck;
    QComboBox* m_themeOverrideCombo;
    QComboBox* m_syncSystemCombo;
    QLineEdit* m_fuseMountPointEdit;
    QSpinBox* m_cacheSize;
    QCheckBox* m_debugModeCheck;

    // Dialog buttons
    QPushButton* m_applyButton;
    QPushButton* m_cancelButton;
    QPushButton* m_okButton;

    // Snapshot of restart-required settings (captured on load)
    QString m_originalSyncFolder;
    QString m_originalSyncMode;
    QString m_originalConflictStrategy;
    QString m_originalSyncSystem;
    QString m_originalFuseMountPoint;
    int m_originalCacheSize = 0;

    bool checkRestartRequired() const;
    void promptRestart();
};

#endif  // SETTINGSWINDOW_H
