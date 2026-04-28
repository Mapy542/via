/**
 * @file SettingsWindow.h
 * @brief Settings/Preferences window
 *
 * Allows users to configure the application settings including
 * login, mirror sync, FUSE, and miscellaneous behavior.
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
#include <memory>

class GoogleAuthManager;
class GoogleDriveClient;
class SyncActionQueue;
class ChangeProcessor;
class RuntimePauseController;

class SettingsCredentialStore {
   public:
    virtual ~SettingsCredentialStore() = default;

    virtual QString getClientId() const = 0;
    virtual QString getClientSecret() const = 0;
    virtual void saveCredentials(const QString& clientId, const QString& clientSecret) = 0;
};

/**
 * @class SettingsWindow
 * @brief Settings and preferences dialog
 *
 * Provides UI for configuring login, mirror sync, FUSE, and
 * miscellaneous application behavior.
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
                            QWidget* parent = nullptr,
                            SettingsCredentialStore* credentialStore = nullptr,
                            RuntimePauseController* pauseController = nullptr);

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
     * The owner should queue a restart-gated purge so the next launch can
     * safely clear evictable cache files and representation metadata.
     */
    void clearCacheRequested();

    /**
     * @brief Emitted when the user accepts a restart after changing
     * restart-required settings.
     *
     * The owner (MainWindow/main.cpp) should perform a safe FUSE
     * unmount, set any pending purge flags, relaunch, and quit.
     */
    void restartRequested();

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
    void setupLoginTab();
    void setupMirrorTab();
    void setupFuseTab();
    void setupMiscTab();
    void updateStorageInfo();
    void updateClientSecretPlaceholder(bool hasStoredSecret);
    void refreshCacheUsageTracker();
    qint64 scanFuseCacheUsageBytes() const;

    GoogleAuthManager* m_authManager;
    SyncActionQueue* m_syncActionQueue;
    ChangeProcessor* m_changeProcessor;
    GoogleDriveClient* m_driveClient;
    RuntimePauseController* m_pauseController;
    QSettings m_settings;
    std::unique_ptr<SettingsCredentialStore> m_ownedCredentialStore;
    SettingsCredentialStore* m_credentialStore;

    // Tab widget
    QTabWidget* m_tabWidget;

    // Login tab widgets
    QWidget* m_loginTab;
    QLabel* m_accountStatus;
    QPushButton* m_loginButton;
    QPushButton* m_logoutButton;
    QLineEdit* m_clientIdEdit;
    QLineEdit* m_clientSecretEdit;
    QPushButton* m_saveCredentialsButton;
    QLabel* m_storageLabel;

    // Mirror tab widgets
    QWidget* m_mirrorTab;
    QLineEdit* m_syncFolderEdit;
    QPushButton* m_browseFolderButton;
    QComboBox* m_syncModeCombo;
    QComboBox* m_duplicateNameCombo;
    QComboBox* m_conflictResolutionCombo;

    // Fuse tab widgets
    QWidget* m_fuseTab;
    QComboBox* m_syncSystemCombo;
    QLineEdit* m_fuseMountPointEdit;
    QSpinBox* m_cacheSize;
    QLabel* m_cacheUsageLabel;
    QComboBox* m_nativeDocModeCombo;
    QPushButton* m_clearCacheButton;

    // Misc tab widgets
    QWidget* m_miscTab;
    QCheckBox* m_startOnLoginCheck;
    QCheckBox* m_showNotificationsCheck;
    QCheckBox* m_autoPauseCheck;
    QComboBox* m_themeOverrideCombo;
    QCheckBox* m_debugModeCheck;

    // Dialog buttons
    QPushButton* m_applyButton;
    QPushButton* m_cancelButton;
    QPushButton* m_okButton;

    // Snapshot of restart-required settings (captured on load)
    QString m_originalSyncFolder;
    QString m_originalSyncMode;
    QString m_originalDuplicateNameStrategy;
    QString m_originalConflictStrategy;
    QString m_originalSyncSystem;
    QString m_originalFuseMountPoint;
    int m_originalCacheSize = 0;
    QString m_originalNativeDocMode;

    bool checkRestartRequired() const;
    void promptRestart();
};

#endif  // SETTINGSWINDOW_H
