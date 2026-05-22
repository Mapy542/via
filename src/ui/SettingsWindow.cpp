/**
 * @file SettingsWindow.cpp
 * @brief Implementation of the settings window
 */

#include "SettingsWindow.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>

#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "auth/TokenStorage.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncSettings.h"
#include "utils/AutostartManager.h"

namespace {
constexpr qint64 BYTES_PER_MEGABYTE = 1024LL * 1024LL;

QString formatMegabytes(qint64 bytes) {
    const double megabytes = static_cast<double>(bytes) / static_cast<double>(BYTES_PER_MEGABYTE);
    return QString::number(megabytes, 'f', megabytes >= 100.0 ? 0 : 1);
}

QString fuseCacheTooltip() {
    return QStringLiteral(
        "Target size for evictable FUSE cache files. Pending uploads are stored separately and "
        "excluded from this tracker.");
}

QString emptySecretPlaceholder() {
    return QStringLiteral("Enter your OAuth Client Secret");
}

QString storedSecretPlaceholder() {
    return QStringLiteral("••••••••••••••••");
}

bool mirrorEnabledForSyncSystem(const QString& syncSystem) {
    return syncSystem == QStringLiteral("mirror-only") || syncSystem == QStringLiteral("both");
}

bool fuseEnabledForSyncSystem(const QString& syncSystem) {
    return syncSystem == QStringLiteral("fuse-only") || syncSystem == QStringLiteral("both");
}

QString syncSystemFromEnabledFlags(bool mirrorEnabled, bool fuseEnabled) {
    if (mirrorEnabled && fuseEnabled) {
        return QStringLiteral("both");
    }
    if (mirrorEnabled) {
        return QStringLiteral("mirror-only");
    }
    if (fuseEnabled) {
        return QStringLiteral("fuse-only");
    }
    return QStringLiteral("none");
}

QString normalizeSyncSystem(const QString& syncSystem) {
    if (syncSystem == QStringLiteral("mirror-only") || syncSystem == QStringLiteral("fuse-only") ||
        syncSystem == QStringLiteral("both") || syncSystem == QStringLiteral("none")) {
        return syncSystem;
    }
    return QStringLiteral("mirror-only");
}

class TokenStorageCredentialStore final : public SettingsCredentialStore {
   public:
    QString getClientId() const override { return m_storage.getClientId(); }
    QString getClientSecret() const override { return m_storage.getClientSecret(); }

    void saveCredentials(const QString& clientId, const QString& clientSecret) override {
        m_storage.saveCredentials(clientId, clientSecret);
    }

   private:
    TokenStorage m_storage;
};
}  // namespace

SettingsWindow::SettingsWindow(GoogleAuthManager* authManager, GoogleDriveClient* driveClient,
                               QWidget* parent, SettingsCredentialStore* credentialStore,
                               RuntimePauseController* pauseController)
    : QDialog(parent),
      m_authManager(authManager),
      m_driveClient(driveClient),
      m_pauseController(pauseController),
      m_ownedCredentialStore(
          credentialStore == nullptr ? std::make_unique<TokenStorageCredentialStore>() : nullptr),
      m_credentialStore(credentialStore != nullptr ? credentialStore
                                                   : m_ownedCredentialStore.get()) {
    setWindowTitle("Settings");
    setMinimumSize(500, 450);
    resize(600, 500);
    setModal(false);

    setupUi();
    loadSettings();
}

SettingsWindow::~SettingsWindow() = default;

void SettingsWindow::setupUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // Create tab widget
    m_tabWidget = new QTabWidget(this);

    setupLoginTab();
    setupMirrorTab();
    setupCommonTab();
    setupFuseTab();
    setupMiscTab();

    m_tabWidget->addTab(m_loginTab, "Login");
    m_tabWidget->addTab(m_mirrorTab, "Mirror");
    m_tabWidget->addTab(m_commonTab, "Common");
    m_tabWidget->addTab(m_fuseTab, "Fuse");
    m_tabWidget->addTab(m_miscTab, "Misc");

    mainLayout->addWidget(m_tabWidget);

    // Dialog buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_applyButton = new QPushButton("Apply", this);
    m_applyButton->setObjectName("settingsApplyButton");
    m_cancelButton = new QPushButton("Cancel", this);
    m_okButton = new QPushButton("OK", this);
    m_okButton->setDefault(true);

    buttonLayout->addWidget(m_applyButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addWidget(m_okButton);

    mainLayout->addLayout(buttonLayout);

    // Connect buttons
    connect(m_applyButton, &QPushButton::clicked, this, &SettingsWindow::onApplyClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &SettingsWindow::onCancelClicked);
    connect(m_okButton, &QPushButton::clicked, this, [this]() {
        onApplyClicked();
        // Only accept (close) when no restart is happening — promptRestart
        // calls QApplication::quit() when the user chooses "Restart Now",
        // so accept() would never run in that case anyway.  For "Later"
        // (or no restart needed) we close the dialog normally.
        accept();
    });
}

void SettingsWindow::setupLoginTab() {
    m_loginTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_loginTab);

    // API Credentials group - MUST be configured first
    QGroupBox* apiGroup = new QGroupBox("Google API Credentials", m_loginTab);
    QVBoxLayout* apiLayout = new QVBoxLayout(apiGroup);

    QLabel* apiInfoLabel = new QLabel(
        "To use Via, you need to create OAuth 2.0 credentials in the "
        "<a href='https://console.cloud.google.com/apis/credentials'>Google Cloud Console</a>.\n"
        "Enable the Google Drive API and create OAuth Client ID credentials (Desktop app type).",
        m_loginTab);
    apiInfoLabel->setWordWrap(true);
    apiInfoLabel->setOpenExternalLinks(true);
    apiInfoLabel->setTextFormat(Qt::RichText);
    apiLayout->addWidget(apiInfoLabel);

    QFormLayout* credentialsForm = new QFormLayout();
    m_clientIdEdit = new QLineEdit(m_loginTab);
    m_clientIdEdit->setObjectName("settingsClientIdEdit");
    m_clientIdEdit->setPlaceholderText("Enter your OAuth Client ID");
    m_clientIdEdit->setEchoMode(QLineEdit::Normal);
    credentialsForm->addRow("Client ID:", m_clientIdEdit);

    m_clientSecretEdit = new QLineEdit(m_loginTab);
    m_clientSecretEdit->setObjectName("settingsClientSecretEdit");
    m_clientSecretEdit->setPlaceholderText(emptySecretPlaceholder());
    m_clientSecretEdit->setEchoMode(QLineEdit::Password);
    credentialsForm->addRow("Client Secret:", m_clientSecretEdit);
    apiLayout->addLayout(credentialsForm);

    QHBoxLayout* saveCredentialsLayout = new QHBoxLayout();
    m_saveCredentialsButton = new QPushButton("Save API Credentials", m_loginTab);
    m_saveCredentialsButton->setObjectName("settingsSaveCredentialsButton");
    saveCredentialsLayout->addWidget(m_saveCredentialsButton);
    saveCredentialsLayout->addStretch();
    apiLayout->addLayout(saveCredentialsLayout);

    layout->addWidget(apiGroup);

    // Account info group
    QGroupBox* accountGroup = new QGroupBox("Google Account", m_loginTab);
    QVBoxLayout* accountLayout = new QVBoxLayout(accountGroup);

    m_accountStatus = new QLabel("Not signed in", m_loginTab);
    m_accountStatus->setStyleSheet("QLabel { font-size: 14px; }");
    accountLayout->addWidget(m_accountStatus);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    m_loginButton = new QPushButton("Sign In with Google", m_loginTab);
    m_logoutButton = new QPushButton("Sign Out", m_loginTab);

    buttonLayout->addWidget(m_loginButton);
    buttonLayout->addWidget(m_logoutButton);
    buttonLayout->addStretch();
    accountLayout->addLayout(buttonLayout);

    layout->addWidget(accountGroup);

    // Storage info group
    QGroupBox* storageGroup = new QGroupBox("Storage", m_loginTab);
    QVBoxLayout* storageLayout = new QVBoxLayout(storageGroup);

    m_storageLabel = new QLabel("Retrieving storage info...", m_loginTab);
    m_storageLabel->setObjectName("settingsStorageInfoLabel");
    storageLayout->addWidget(m_storageLabel);

    layout->addWidget(storageGroup);
    layout->addStretch();

    // Connect save credentials button
    connect(m_saveCredentialsButton, &QPushButton::clicked, this,
            &SettingsWindow::onSaveCredentialsClicked);

    // Connect buttons
    connect(m_loginButton, &QPushButton::clicked, this, [this]() {
        if (m_authManager) {
            m_authManager->authenticate();
        }
    });

    connect(m_logoutButton, &QPushButton::clicked, this, [this]() {
        if (m_authManager) {
            QMessageBox::StandardButton reply =
                QMessageBox::question(this, "Sign Out", "Are you sure you want to sign out?",
                                      QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::Yes) {
                m_authManager->logout();
            }
        }
    });

    // Update visibility based on auth state
    if (m_authManager) {
        bool authenticated = m_authManager->isAuthenticated();
        m_loginButton->setVisible(!authenticated);
        m_logoutButton->setVisible(authenticated);
        if (authenticated) {
            m_accountStatus->setText("Signed in to Google Drive");
        }

        connect(m_authManager, &GoogleAuthManager::authenticated, this, [this]() {
            m_accountStatus->setText("Signed in to Google Drive");
            m_loginButton->setVisible(false);
            m_logoutButton->setVisible(true);
            updateStorageInfo();
        });

        connect(m_authManager, &GoogleAuthManager::loggedOut, this, [this]() {
            m_accountStatus->setText("Not signed in");
            m_loginButton->setVisible(true);
            m_logoutButton->setVisible(false);
            setStorageInfoState(StorageInfoState::Unavailable);
        });
    }

    // Connect to storage info and user info signals
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::aboutInfoReceived, this,
                &SettingsWindow::onStorageInfoReceived);

        connect(m_driveClient, &GoogleDriveClient::userInfoReceived, this,
                [this](const QString& displayName, const QString& emailAddress) {
                    QString statusText = "Signed in";
                    if (!displayName.isEmpty() && !emailAddress.isEmpty()) {
                        statusText = QString("Signed in as %1 (%2)").arg(displayName, emailAddress);
                    } else if (!emailAddress.isEmpty()) {
                        statusText = QString("Signed in as %1").arg(emailAddress);
                    } else if (!displayName.isEmpty()) {
                        statusText = QString("Signed in as %1").arg(displayName);
                    }
                    m_accountStatus->setText(statusText);
                });

        connect(m_driveClient, &GoogleDriveClient::errorDetailed, this,
                [this](const QString& operation, const QString& error, int, const QString&,
                       const QString&) {
                    if (operation == "getAboutInfo") {
                        if (m_authManager && m_authManager->isAuthenticated()) {
                            setStorageInfoState(StorageInfoState::Failure, error);
                        } else {
                            setStorageInfoState(StorageInfoState::Unavailable);
                        }
                    }
                });
    }

    if (m_driveClient && m_authManager && m_authManager->isAuthenticated()) {
        setStorageInfoState(StorageInfoState::Loading);
    } else {
        setStorageInfoState(StorageInfoState::Unavailable);
    }
}

void SettingsWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);

    if (m_authManager && m_authManager->isAuthenticated()) {
        QTimer::singleShot(0, this, [this]() {
            if (isVisible() && m_authManager && m_authManager->isAuthenticated()) {
                updateStorageInfo();
            }
        });
    }

    refreshCacheUsageTracker();
}

void SettingsWindow::refreshCacheUsageTracker() {
    if (!m_cacheUsageLabel || !m_cacheSize) {
        return;
    }

    const qint64 currentBytes = scanFuseCacheUsageBytes();
    m_cacheUsageLabel->setText(QString("Current: %1 / %2 MB")
                                   .arg(formatMegabytes(currentBytes))
                                   .arg(m_cacheSize->value()));
}

qint64 SettingsWindow::scanFuseCacheUsageBytes() const {
    const QString cacheRoot =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/Via/files";
    QDir cacheDir(cacheRoot);
    if (!cacheDir.exists()) {
        return 0;
    }

    qint64 totalBytes = 0;
    QDirIterator it(cacheRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        totalBytes += it.fileInfo().size();
    }

    return totalBytes;
}

void SettingsWindow::setupMirrorTab() {
    m_mirrorTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_mirrorTab);

    m_mirrorEnabledCheck = new QCheckBox("Enable mirror sync", m_mirrorTab);
    m_mirrorEnabledCheck->setObjectName("settingsMirrorEnabledCheck");
    layout->addWidget(m_mirrorEnabledCheck);

    // Sync folder group
    QGroupBox* folderGroup = new QGroupBox("Sync Folder", m_mirrorTab);
    QVBoxLayout* folderLayout = new QVBoxLayout(folderGroup);

    QLabel* folderLabel = new QLabel("Local folder for Google Drive files:", m_mirrorTab);
    folderLayout->addWidget(folderLabel);

    QHBoxLayout* folderPathLayout = new QHBoxLayout();
    m_syncFolderEdit = new QLineEdit(m_mirrorTab);
    m_syncFolderEdit->setPlaceholderText(QDir::homePath() + "/GoogleDrive");
    m_browseFolderButton = new QPushButton("Browse...", m_mirrorTab);

    folderPathLayout->addWidget(m_syncFolderEdit, 1);
    folderPathLayout->addWidget(m_browseFolderButton);
    folderLayout->addLayout(folderPathLayout);

    layout->addWidget(folderGroup);

    // Conflict resolution group
    QGroupBox* conflictGroup = new QGroupBox("Conflict Resolution", m_mirrorTab);
    QVBoxLayout* conflictLayout = new QVBoxLayout(conflictGroup);

    QLabel* conflictLabel =
        new QLabel("When a file is modified both locally and remotely:", m_mirrorTab);
    conflictLayout->addWidget(conflictLabel);

    QHBoxLayout* conflictComboLayout = new QHBoxLayout();
    m_conflictResolutionCombo = new QComboBox(m_mirrorTab);
    m_conflictResolutionCombo->addItem("Keep both versions (creates conflict copy)", "keep-both");
    m_conflictResolutionCombo->addItem("Always keep local version", "keep-local");
    m_conflictResolutionCombo->addItem("Always keep remote version", "keep-remote");
    m_conflictResolutionCombo->addItem("Keep newest (by modification time)", "keep-newest");
    m_conflictResolutionCombo->addItem("Ask me each time", "ask-user");
    conflictComboLayout->addWidget(m_conflictResolutionCombo);
    conflictComboLayout->addStretch();
    conflictLayout->addLayout(conflictComboLayout);

    QLabel* conflictInfoLabel = new QLabel(
        "<i>When 'Keep both' is selected, the local version is renamed with "
        "'(local conflict DATE)' and the remote version is downloaded.<br>"
        "When 'Keep newest' is selected, the file with the most recent modification "
        "time wins. If both were modified since last sync, both versions are kept.</i>",
        m_mirrorTab);
    conflictInfoLabel->setWordWrap(true);
    conflictInfoLabel->setTextFormat(Qt::RichText);
    conflictLayout->addWidget(conflictInfoLabel);

    layout->addWidget(conflictGroup);

    QGroupBox* performanceGroup = new QGroupBox("Performance", m_mirrorTab);
    QFormLayout* performanceLayout = new QFormLayout(performanceGroup);

    m_mirrorDormantTimeSpin = new QSpinBox(m_mirrorTab);
    m_mirrorDormantTimeSpin->setObjectName("settingsMirrorDormantTimeSpin");
    m_mirrorDormantTimeSpin->setRange(SyncSettings::MIN_MIRROR_DORMANT_TIME_MS,
                                      SyncSettings::MAX_MIRROR_DORMANT_TIME_MS);
    m_mirrorDormantTimeSpin->setSingleStep(50);
    m_mirrorDormantTimeSpin->setSuffix(" ms");
    m_mirrorDormantTimeSpin->setSpecialValueText("Off");
    m_mirrorDormantTimeSpin->setToolTip(
        "How long mirror sync stays dormant after using its active budget.");
    performanceLayout->addRow("Dormant time:", m_mirrorDormantTimeSpin);

    m_mirrorDutyCycleSpin = new QSpinBox(m_mirrorTab);
    m_mirrorDutyCycleSpin->setObjectName("settingsMirrorDutyCycleSpin");
    m_mirrorDutyCycleSpin->setRange(SyncSettings::MIN_MIRROR_DUTY_CYCLE_PERCENT,
                                    SyncSettings::MAX_MIRROR_DUTY_CYCLE_PERCENT);
    m_mirrorDutyCycleSpin->setSuffix(" %");
    m_mirrorDutyCycleSpin->setToolTip("How much worker time mirror sync can use before it yields.");
    performanceLayout->addRow("Duty cycle:", m_mirrorDutyCycleSpin);

    QLabel* performanceInfoLabel = new QLabel(
        "<i>Use a non-zero dormant time and lower duty cycle to reduce sustained mirror-sync "
        "CPU and disk usage during large reconnaissance passes. 100% duty cycle preserves the "
        "current always-on behavior.</i>",
        m_mirrorTab);
    performanceInfoLabel->setObjectName("settingsMirrorPerformanceInfoLabel");
    performanceInfoLabel->setWordWrap(true);
    performanceInfoLabel->setTextFormat(Qt::RichText);
    performanceLayout->addRow(performanceInfoLabel);

    layout->addWidget(performanceGroup);
    layout->addStretch();

    // Connect buttons
    connect(m_browseFolderButton, &QPushButton::clicked, this,
            &SettingsWindow::onBrowseFolderClicked);
    connect(m_mirrorEnabledCheck, &QCheckBox::toggled, this,
            &SettingsWindow::updateSyncSystemWidgets);
}

void SettingsWindow::setupCommonTab() {
    m_commonTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_commonTab);

    QGroupBox* syncModeGroup = new QGroupBox("Sync Mode", m_commonTab);
    QVBoxLayout* syncModeLayout = new QVBoxLayout(syncModeGroup);

    QLabel* syncModeLabel =
        new QLabel("Choose how mirror sync and FUSE handle Drive mutations:", m_commonTab);
    syncModeLabel->setObjectName("settingsSyncModeLabel");
    syncModeLabel->setWordWrap(true);
    syncModeLayout->addWidget(syncModeLabel);

    QHBoxLayout* syncModeComboLayout = new QHBoxLayout();
    m_syncModeCombo = new QComboBox(m_commonTab);
    m_syncModeCombo->setObjectName("settingsSyncModeCombo");
    m_syncModeCombo->addItem("Keep Newest (bidirectional sync)", "keep-newest");
    m_syncModeCombo->addItem("Remote Read-Only (download only, never upload)", "remote-read-only");
    m_syncModeCombo->addItem("Remote No Delete (sync but don't delete remote files)",
                             "remote-no-delete");
    syncModeComboLayout->addWidget(m_syncModeCombo);
    syncModeComboLayout->addStretch();
    syncModeLayout->addLayout(syncModeComboLayout);

    QLabel* syncModeInfoLabel = new QLabel(
        "<i>This shared setting applies to mirror sync and FUSE.<br>"
        "<b>Keep Newest:</b> Full bidirectional sync - uploads and downloads based on "
        "modification time.<br>"
        "<b>Remote Read-Only:</b> Mirror never uploads local changes, and FUSE blocks "
        "Drive-mutating operations.<br>"
        "<b>Remote No Delete:</b> Mirror keeps syncing but preserves remote files, and FUSE "
        "blocks delete/trash operations.</i>",
        m_commonTab);
    syncModeInfoLabel->setObjectName("settingsSyncModeInfoLabel");
    syncModeInfoLabel->setWordWrap(true);
    syncModeInfoLabel->setTextFormat(Qt::RichText);
    syncModeLayout->addWidget(syncModeInfoLabel);

    layout->addWidget(syncModeGroup);

    QGroupBox* duplicateNamesGroup = new QGroupBox("Duplicate File Endings", m_commonTab);
    QVBoxLayout* duplicateNamesLayout = new QVBoxLayout(duplicateNamesGroup);

    QLabel* duplicateNamesLabel = new QLabel(
        "Choose how Via renames duplicate Drive files when the original local name is already "
        "claimed:",
        m_commonTab);
    duplicateNamesLabel->setWordWrap(true);
    duplicateNamesLayout->addWidget(duplicateNamesLabel);

    QHBoxLayout* duplicateComboLayout = new QHBoxLayout();
    m_duplicateNameCombo = new QComboBox(m_commonTab);
    m_duplicateNameCombo->setObjectName("settingsDuplicateNameCombo");
    m_duplicateNameCombo->addItem("Append Drive file ID (example: report_abcd1234.txt)",
                                  "file-id-suffix");
    m_duplicateNameCombo->addItem("Append numbered suffix (example: report (1).txt)",
                                  "numeric-suffix");
    duplicateComboLayout->addWidget(m_duplicateNameCombo);
    duplicateComboLayout->addStretch();
    duplicateNamesLayout->addLayout(duplicateComboLayout);

    QLabel* duplicateInfoLabel = new QLabel(
        "<i>This naming rule applies anywhere Via materializes duplicate Drive files, including "
        "mirror sync and the FUSE view.</i>",
        m_commonTab);
    duplicateInfoLabel->setWordWrap(true);
    duplicateInfoLabel->setTextFormat(Qt::RichText);
    duplicateNamesLayout->addWidget(duplicateInfoLabel);

    layout->addWidget(duplicateNamesGroup);

    QGroupBox* nativeDocGroup = new QGroupBox("Native Documents", m_commonTab);
    QVBoxLayout* nativeDocLayout = new QVBoxLayout(nativeDocGroup);

    QLabel* nativeDocLabel =
        new QLabel("Choose how Google-native docs are materialized locally:", m_commonTab);
    nativeDocLabel->setObjectName("settingsNativeDocModeLabel");
    nativeDocLabel->setWordWrap(true);
    nativeDocLayout->addWidget(nativeDocLabel);

    QHBoxLayout* nativeDocComboLayout = new QHBoxLayout();
    m_nativeDocModeCombo = new QComboBox(m_commonTab);
    m_nativeDocModeCombo->setObjectName("settingsNativeDocModeCombo");
    m_nativeDocModeCombo->addItem("Hide (don't materialize locally)", "hide");
    m_nativeDocModeCombo->addItem("Browser shortcuts (.gdoc, .gsheet, ...)", "browser-shortcut");
    m_nativeDocModeCombo->addItem("OpenDocument snapshots (.odt, .ods, ...)", "open-document");
    m_nativeDocModeCombo->addItem("Text snapshots (.md, .csv, ...)", "text");
    nativeDocComboLayout->addWidget(m_nativeDocModeCombo);
    nativeDocComboLayout->addStretch();
    nativeDocLayout->addLayout(nativeDocComboLayout);

    QLabel* nativeDocInfoLabel = new QLabel(
        "<i>This shared setting applies to mirror sync and FUSE. Native-document shortcuts "
        "and exports are always materialized read-only.</i>",
        m_commonTab);
    nativeDocInfoLabel->setObjectName("settingsNativeDocModeInfoLabel");
    nativeDocInfoLabel->setWordWrap(true);
    nativeDocInfoLabel->setTextFormat(Qt::RichText);
    nativeDocLayout->addWidget(nativeDocInfoLabel);

    layout->addWidget(nativeDocGroup);
    layout->addStretch();
}

void SettingsWindow::setupFuseTab() {
    m_fuseTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_fuseTab);

    m_fuseEnabledCheck = new QCheckBox("Enable FUSE sync", m_fuseTab);
    m_fuseEnabledCheck->setObjectName("settingsFuseEnabledCheck");
    layout->addWidget(m_fuseEnabledCheck);

    QGroupBox* mountGroup = new QGroupBox("Mount Point", m_fuseTab);
    QVBoxLayout* mountGroupLayout = new QVBoxLayout(mountGroup);

    QHBoxLayout* mountLayout = new QHBoxLayout();
    mountLayout->addWidget(new QLabel("Mount point:", m_fuseTab));
    m_fuseMountPointEdit = new QLineEdit(m_fuseTab);
    m_fuseMountPointEdit->setPlaceholderText(QDir::homePath() + "/GoogleDriveFuse");
    m_fuseMountPointEdit->setEnabled(false);
    mountLayout->addWidget(m_fuseMountPointEdit);
    mountGroupLayout->addLayout(mountLayout);

    layout->addWidget(mountGroup);

    QGroupBox* cacheGroup = new QGroupBox("Cache and Maintenance", m_fuseTab);
    QVBoxLayout* cacheGroupLayout = new QVBoxLayout(cacheGroup);

    QHBoxLayout* cacheSizeLayout = new QHBoxLayout();
    QLabel* cacheSizeLabel = new QLabel("Evictable FUSE cache target:", m_fuseTab);
    cacheSizeLabel->setToolTip(fuseCacheTooltip());
    cacheSizeLayout->addWidget(cacheSizeLabel);
    m_cacheSize = new QSpinBox(m_fuseTab);
    m_cacheSize->setRange(100, 100000);
    m_cacheSize->setValue(5000);
    m_cacheSize->setSuffix(" MB");
    m_cacheSize->setEnabled(false);
    m_cacheSize->setToolTip(fuseCacheTooltip());
    cacheSizeLayout->addWidget(m_cacheSize);

    m_cacheUsageLabel = new QLabel("Current: 0 / 5000 MB", m_fuseTab);
    m_cacheUsageLabel->setToolTip(fuseCacheTooltip());
    cacheSizeLayout->addWidget(m_cacheUsageLabel);
    cacheSizeLayout->addStretch();
    cacheGroupLayout->addLayout(cacheSizeLayout);

    QLabel* cacheInfoLabel = new QLabel(
        "<i>This target applies only to evictable cached files. Pending uploads are stored "
        "separately and are not counted here. Very large single files may temporarily exceed "
        "the target while Via keeps a required local backing file.</i>",
        m_fuseTab);
    cacheInfoLabel->setWordWrap(true);
    cacheInfoLabel->setTextFormat(Qt::RichText);
    cacheGroupLayout->addWidget(cacheInfoLabel);

    m_clearCacheButton = new QPushButton("Restart and Clear Cache", m_fuseTab);
    m_clearCacheButton->setEnabled(false);
    QHBoxLayout* clearLayout = new QHBoxLayout();
    clearLayout->addWidget(m_clearCacheButton);
    clearLayout->addStretch();
    cacheGroupLayout->addLayout(clearLayout);

    layout->addWidget(cacheGroup);
    layout->addStretch();

    connect(m_fuseEnabledCheck, &QCheckBox::toggled, this,
            &SettingsWindow::updateSyncSystemWidgets);
    connect(m_cacheSize, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { refreshCacheUsageTracker(); });
    updateSyncSystemWidgets();
    refreshCacheUsageTracker();

    connect(m_clearCacheButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "Restart and Clear Cache",
            "Via needs to restart to clear cached FUSE files and metadata safely.\n\n"
            "Pending uploads will be preserved.\n\n"
            "Restart now and clear the cache on next launch?",
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            emit clearCacheRequested();
        }
    });
}

void SettingsWindow::setupMiscTab() {
    m_miscTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_miscTab);

    // Startup group
    QGroupBox* startupGroup = new QGroupBox("Startup", m_miscTab);
    QVBoxLayout* startupLayout = new QVBoxLayout(startupGroup);

    m_startOnLoginCheck = new QCheckBox("Start Via on system login", m_miscTab);
    startupLayout->addWidget(m_startOnLoginCheck);

    layout->addWidget(startupGroup);

    // Appearance group
    QGroupBox* appearanceGroup = new QGroupBox("Appearance", m_miscTab);
    QVBoxLayout* appearanceLayout = new QVBoxLayout(appearanceGroup);

    QHBoxLayout* themeLayout = new QHBoxLayout();
    themeLayout->addWidget(new QLabel("Icon theme:", m_miscTab));
    m_themeOverrideCombo = new QComboBox(m_miscTab);
    m_themeOverrideCombo->addItem("Auto (follow system)", 0);
    m_themeOverrideCombo->addItem("Light icons (for light backgrounds)", 1);
    m_themeOverrideCombo->addItem("Dark icons (for dark backgrounds)", 2);
    themeLayout->addWidget(m_themeOverrideCombo);
    themeLayout->addStretch();
    appearanceLayout->addLayout(themeLayout);

    layout->addWidget(appearanceGroup);

    // Notifications group
    QGroupBox* notifyGroup = new QGroupBox("Notifications", m_miscTab);
    QVBoxLayout* notifyLayout = new QVBoxLayout(notifyGroup);

    m_showNotificationsCheck = new QCheckBox("Show desktop notifications", m_miscTab);
    m_showNotificationsCheck->setChecked(true);
    notifyLayout->addWidget(m_showNotificationsCheck);

    layout->addWidget(notifyGroup);

    QGroupBox* pauseGroup = new QGroupBox("Runtime Pause", m_miscTab);
    QVBoxLayout* pauseLayout = new QVBoxLayout(pauseGroup);

    m_autoPauseCheck = new QCheckBox(
        "Automatically pause sync when offline, on metered networks, or in power saver mode",
        m_miscTab);
    m_autoPauseCheck->setObjectName("settingsAutoPauseCheck");
    m_autoPauseCheck->setChecked(true);
    pauseLayout->addWidget(m_autoPauseCheck);

    QLabel* pauseInfoLabel = new QLabel(
        "When disabled, Via keeps syncing even if those runtime conditions are active.", m_miscTab);
    pauseInfoLabel->setWordWrap(true);
    pauseLayout->addWidget(pauseInfoLabel);

    layout->addWidget(pauseGroup);

    // Debug group
    QGroupBox* debugGroup = new QGroupBox("Debug", m_miscTab);
    QVBoxLayout* debugLayout = new QVBoxLayout(debugGroup);

    m_debugModeCheck = new QCheckBox("Enable debug logging", m_miscTab);
    debugLayout->addWidget(m_debugModeCheck);

    layout->addWidget(debugGroup);
    layout->addStretch();
}

void SettingsWindow::updateClientSecretPlaceholder(bool hasStoredSecret) {
    if (!m_clientSecretEdit) {
        return;
    }

    m_clientSecretEdit->setPlaceholderText(hasStoredSecret ? storedSecretPlaceholder()
                                                           : emptySecretPlaceholder());
}

QString SettingsWindow::currentSyncSystem() const {
    const bool mirrorEnabled = m_mirrorEnabledCheck && m_mirrorEnabledCheck->isChecked();
    const bool fuseEnabled = m_fuseEnabledCheck && m_fuseEnabledCheck->isChecked();
    return syncSystemFromEnabledFlags(mirrorEnabled, fuseEnabled);
}

void SettingsWindow::setCurrentSyncSystem(const QString& syncSystem) {
    const QString normalizedSyncSystem = normalizeSyncSystem(syncSystem);
    const QSignalBlocker mirrorBlocker(m_mirrorEnabledCheck);
    const QSignalBlocker fuseBlocker(m_fuseEnabledCheck);

    m_mirrorEnabledCheck->setChecked(mirrorEnabledForSyncSystem(normalizedSyncSystem));
    m_fuseEnabledCheck->setChecked(fuseEnabledForSyncSystem(normalizedSyncSystem));
    updateSyncSystemWidgets();
}

void SettingsWindow::updateSyncSystemWidgets() {
    const bool mirrorEnabled = m_mirrorEnabledCheck && m_mirrorEnabledCheck->isChecked();
    const bool fuseEnabled = m_fuseEnabledCheck && m_fuseEnabledCheck->isChecked();

    m_syncFolderEdit->setEnabled(mirrorEnabled);
    m_browseFolderButton->setEnabled(mirrorEnabled);
    m_conflictResolutionCombo->setEnabled(mirrorEnabled);

    m_fuseMountPointEdit->setEnabled(fuseEnabled);
    m_cacheSize->setEnabled(fuseEnabled);
    m_cacheUsageLabel->setEnabled(fuseEnabled);
    m_clearCacheButton->setEnabled(fuseEnabled);

    m_nativeDocModeCombo->setEnabled(true);
}

void SettingsWindow::captureRestartSettingSnapshots() {
    m_originalSyncFolder = m_syncFolderEdit->text();
    m_originalSyncMode = m_syncModeCombo->currentData().toString();
    m_originalDuplicateNameStrategy = m_duplicateNameCombo->currentData().toString();
    m_originalConflictStrategy = m_conflictResolutionCombo->currentData().toString();
    m_originalSyncSystem = currentSyncSystem();
    m_originalFuseMountPoint = m_fuseMountPointEdit->text();
    m_originalCacheSize = m_cacheSize->value();
    m_originalNativeDocMode = m_nativeDocModeCombo->currentData().toString();
}

void SettingsWindow::loadSettings() {
    // Load API credentials from the secure credential store.
    m_clientIdEdit->setText(m_credentialStore->getClientId());
    m_clientSecretEdit->clear();
    updateClientSecretPlaceholder(!m_credentialStore->getClientSecret().isEmpty());

    // Sync settings
    m_syncFolderEdit->setText(
        m_settings.value("sync/folder", QDir::homePath() + "/GoogleDrive").toString());

    // Sync mode setting (string IDs with numeric fallback)
    const auto setComboById = [](QComboBox* combo, const QString& id) {
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemData(i).toString() == id) {
                combo->setCurrentIndex(i);
                return true;
            }
        }
        return false;
    };

    QString syncModeId = m_settings.value("sync/syncMode", "").toString();
    if (syncModeId.isEmpty()) {
        bool ok = false;
        int legacyMode = m_settings.value("sync/syncMode", 0).toInt(&ok);
        if (ok) {
            switch (legacyMode) {
                case 0:
                    syncModeId = "keep-newest";
                    break;
                case 1:
                    syncModeId = "remote-read-only";
                    break;
                case 2:
                    syncModeId = "remote-no-delete";
                    break;
                default:
                    syncModeId = "keep-newest";
                    break;
            }
        }
    }
    if (!setComboById(m_syncModeCombo, syncModeId)) {
        setComboById(m_syncModeCombo, "keep-newest");
    }

    QString duplicateStrategyId =
        m_settings.value("sync/duplicateNameStrategy", "file-id-suffix").toString();
    if (!setComboById(m_duplicateNameCombo, duplicateStrategyId)) {
        setComboById(m_duplicateNameCombo, "file-id-suffix");
    }

    // Conflict resolution setting (string IDs with numeric fallback)
    QString conflictId = m_settings.value("sync/conflictStrategy", "").toString();
    if (conflictId.isEmpty()) {
        bool ok = false;
        int legacyStrategy = m_settings.value("sync/conflictStrategy", 0).toInt(&ok);
        if (ok) {
            switch (legacyStrategy) {
                case 0:
                    conflictId = "keep-both";
                    break;
                case 1:
                    conflictId = "keep-local";
                    break;
                case 2:
                    conflictId = "keep-remote";
                    break;
                case 3:
                    conflictId = "keep-newest";
                    break;
                case 4:
                    conflictId = "ask-user";
                    break;
                default:
                    conflictId = "keep-both";
                    break;
            }
        }
    }
    if (!setComboById(m_conflictResolutionCombo, conflictId)) {
        setComboById(m_conflictResolutionCombo, "keep-both");
    }

    m_mirrorDormantTimeSpin->setValue(SyncSettings::normalizeMirrorDormantTimeMs(
        m_settings.value("sync/mirrorDormantTimeMs", SyncSettings::DEFAULT_MIRROR_DORMANT_TIME_MS)
            .toInt()));
    m_mirrorDutyCycleSpin->setValue(SyncSettings::normalizeMirrorDutyCyclePercent(
        m_settings.value("sync/mirrorDutyCyclePct", SyncSettings::DEFAULT_MIRROR_DUTY_CYCLE_PERCENT)
            .toInt()));

    // Misc settings
    m_startOnLoginCheck->setChecked(m_settings.value("advanced/startOnLogin", false).toBool());
    m_showNotificationsCheck->setChecked(
        m_settings.value("advanced/showNotifications", true).toBool());
    m_autoPauseCheck->setChecked(m_settings.value("advanced/autoPauseEnabled", true).toBool());

    // Theme override setting
    int themeOverride = m_settings.value("advanced/themeOverride", 0).toInt();
    for (int i = 0; i < m_themeOverrideCombo->count(); ++i) {
        if (m_themeOverrideCombo->itemData(i).toInt() == themeOverride) {
            m_themeOverrideCombo->setCurrentIndex(i);
            break;
        }
    }

    // Fuse settings
    QString syncSystem = m_settings.value("advanced/syncSystem", "").toString();
    if (syncSystem.isEmpty()) {
        // Migrate from old enableFuse boolean
        bool legacyFuse = m_settings.value("advanced/enableFuse", false).toBool();
        syncSystem = legacyFuse ? "both" : "mirror-only";
    }
    setCurrentSyncSystem(syncSystem);
    m_fuseMountPointEdit->setText(
        m_settings.value("advanced/fuseMountPoint", QDir::homePath() + "/GoogleDriveFuse")
            .toString());
    m_cacheSize->setValue(m_settings.value("advanced/cacheSize", 5000).toInt());
    m_debugModeCheck->setChecked(m_settings.value("advanced/debugMode", false).toBool());

    // Native doc mode (mirror sync and FUSE)
    {
        QString nativeDocId = m_settings.value("advanced/nativeDocMode", "hide").toString();
        for (int i = 0; i < m_nativeDocModeCombo->count(); ++i) {
            if (m_nativeDocModeCombo->itemData(i).toString() == nativeDocId) {
                m_nativeDocModeCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    captureRestartSettingSnapshots();

    refreshCacheUsageTracker();
}

void SettingsWindow::saveSettings() {
    // Mirror settings
    m_settings.setValue("sync/folder", m_syncFolderEdit->text());
    m_settings.setValue("sync/syncMode", m_syncModeCombo->currentData().toString());
    m_settings.setValue("sync/duplicateNameStrategy",
                        m_duplicateNameCombo->currentData().toString());
    m_settings.setValue("sync/conflictStrategy",
                        m_conflictResolutionCombo->currentData().toString());
    m_settings.setValue("sync/mirrorDormantTimeMs", SyncSettings::normalizeMirrorDormantTimeMs(
                                                        m_mirrorDormantTimeSpin->value()));
    m_settings.setValue("sync/mirrorDutyCyclePct", SyncSettings::normalizeMirrorDutyCyclePercent(
                                                       m_mirrorDutyCycleSpin->value()));

    // Misc settings
    m_settings.setValue("advanced/startOnLogin", m_startOnLoginCheck->isChecked());
    AutostartManager::setAutostart(m_startOnLoginCheck->isChecked());
    m_settings.setValue("advanced/showNotifications", m_showNotificationsCheck->isChecked());
    m_settings.setValue("advanced/autoPauseEnabled", m_autoPauseCheck->isChecked());
    m_settings.setValue("advanced/themeOverride", m_themeOverrideCombo->currentData().toInt());

    // Fuse settings
    m_settings.setValue("advanced/syncSystem", currentSyncSystem());
    m_settings.setValue("advanced/fuseMountPoint", m_fuseMountPointEdit->text());
    m_settings.setValue("advanced/cacheSize", m_cacheSize->value());
    m_settings.setValue("advanced/nativeDocMode", m_nativeDocModeCombo->currentData().toString());
    m_settings.setValue("advanced/debugMode", m_debugModeCheck->isChecked());

    m_settings.sync();

    if (m_pauseController) {
        m_pauseController->setAutoPauseEnabled(m_autoPauseCheck->isChecked());
    }

    emit settingsChanged();
}

void SettingsWindow::onApplyClicked() {
    saveSettings();
    promptRestart();
}

void SettingsWindow::onCancelClicked() {
    loadSettings();
    reject();
}

void SettingsWindow::onBrowseFolderClicked() {
    QString folder = QFileDialog::getExistingDirectory(
        this, "Select Sync Folder",
        m_syncFolderEdit->text().isEmpty() ? QDir::homePath() : m_syncFolderEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!folder.isEmpty()) {
        m_syncFolderEdit->setText(folder);
    }
}

void SettingsWindow::onSaveCredentialsClicked() {
    QString clientId = m_clientIdEdit->text().trimmed();
    QString clientSecret = m_clientSecretEdit->text().trimmed();
    const QString existingSecret = m_credentialStore->getClientSecret();

    if (clientId.isEmpty()) {
        QMessageBox::warning(this, "Missing Client ID", "Please enter your OAuth Client ID.");
        return;
    }

    if (clientSecret.isEmpty() && existingSecret.isEmpty()) {
        QMessageBox::warning(this, "Missing Client Secret",
                             "Please enter your OAuth Client Secret.");
        return;
    }

    // Save credentials via TokenStorage (which encodes them)
    // We store the display version of client ID for showing in UI
    m_settings.setValue("auth/clientIdDisplay", clientId);

    const QString secretToSave = clientSecret.isEmpty() ? existingSecret : clientSecret;

    // If the user leaves the secret field empty, preserve the stored secret.
    m_credentialStore->saveCredentials(clientId, secretToSave);

    // Update the auth manager with new credentials
    if (m_authManager) {
        m_authManager->setCredentials(clientId, secretToSave);
    }

    // Clear the secret field for security
    m_clientSecretEdit->clear();
    updateClientSecretPlaceholder(!secretToSave.isEmpty());

    QMessageBox::information(this, "Credentials Saved",
                             "Your Google API credentials have been saved.\n\n"
                             "You can now sign in with your Google account.");
}

void SettingsWindow::updateStorageInfo() {
    if (m_driveClient && m_authManager && m_authManager->isAuthenticated()) {
        setStorageInfoState(StorageInfoState::Loading);
        m_driveClient->getAboutInfo();
    } else {
        setStorageInfoState(StorageInfoState::Unavailable);
    }
}

void SettingsWindow::onStorageInfoReceived(qint64 storageUsed, qint64 storageLimit) {
    if (!m_authManager || !m_authManager->isAuthenticated()) {
        setStorageInfoState(StorageInfoState::Unavailable);
        return;
    }

    setStorageInfoState(StorageInfoState::Success, QString(), storageUsed, storageLimit);
}

void SettingsWindow::setStorageInfoState(StorageInfoState state, const QString& detail,
                                         qint64 storageUsed, qint64 storageLimit) {
    if (!m_storageLabel) {
        return;
    }

    switch (state) {
        case StorageInfoState::Loading:
            m_storageLabel->setText("Retrieving storage info...");
            return;

        case StorageInfoState::Unavailable:
            m_storageLabel->setText("Not available");
            return;

        case StorageInfoState::Failure:
            if (detail.isEmpty()) {
                m_storageLabel->setText("Unable to retrieve storage info");
            } else {
                m_storageLabel->setText(QString("Unable to retrieve storage info: %1").arg(detail));
            }
            return;

        case StorageInfoState::Success:
            break;
    }

    auto formatBytes = [](qint64 bytes) -> QString {
        const qint64 KB = 1024;
        const qint64 MB = KB * 1024;
        const qint64 GB = MB * 1024;
        const qint64 TB = GB * 1024;

        if (bytes >= TB) {
            return QString::number(bytes / (double)TB, 'f', 2) + " TB";
        } else if (bytes >= GB) {
            return QString::number(bytes / (double)GB, 'f', 2) + " GB";
        } else if (bytes >= MB) {
            return QString::number(bytes / (double)MB, 'f', 2) + " MB";
        } else if (bytes >= KB) {
            return QString::number(bytes / (double)KB, 'f', 2) + " KB";
        }

        return QString::number(bytes) + " B";
    };

    const QString usedStr = formatBytes(storageUsed);
    const QString limitStr = formatBytes(storageLimit);
    const double percentUsed = storageLimit > 0 ? (storageUsed * 100.0 / storageLimit) : 0.0;

    m_storageLabel->setText(QString("Storage usage: %1 / %2 (%3%)")
                                .arg(usedStr)
                                .arg(limitStr)
                                .arg(QString::number(percentUsed, 'f', 1)));
}

bool SettingsWindow::checkRestartRequired() const {
    if (m_syncFolderEdit->text() != m_originalSyncFolder)
        return true;
    if (m_duplicateNameCombo->currentData().toString() != m_originalDuplicateNameStrategy)
        return true;
    if (m_conflictResolutionCombo->currentData().toString() != m_originalConflictStrategy)
        return true;
    if (currentSyncSystem() != m_originalSyncSystem)
        return true;
    if (m_fuseMountPointEdit->text() != m_originalFuseMountPoint)
        return true;
    if (m_cacheSize->value() != m_originalCacheSize)
        return true;
    if (m_nativeDocModeCombo->currentData().toString() != m_originalNativeDocMode)
        return true;
    return false;
}

void SettingsWindow::promptRestart() {
    if (!checkRestartRequired()) {
        return;
    }

    const bool nativeDocModeChanged =
        m_nativeDocModeCombo->currentData().toString() != m_originalNativeDocMode;
    const QString syncSystem = currentSyncSystem();

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Restart Required");
    msgBox.setText("One or more settings you changed require a restart to take effect.");
    if (nativeDocModeChanged) {
        QString rebuildMessage;
        if (syncSystem == "both") {
            rebuildMessage =
                "Via will rebuild local native-document artifacts in the sync folder and refresh "
                "the FUSE view on next launch.";
        } else if (syncSystem == "mirror-only") {
            rebuildMessage =
                "Via will rebuild local native-document artifacts in the sync folder on next "
                "launch.";
        } else if (syncSystem == "none") {
            rebuildMessage =
                "Via will apply the updated native-document representation the next time mirror "
                "or FUSE sync is enabled.";
        } else {
            rebuildMessage =
                "Via will refresh native-document representation in the FUSE view on next "
                "launch.";
        }

        msgBox.setInformativeText(rebuildMessage + "\n\nWould you like to restart Via now?");
    } else {
        msgBox.setInformativeText("Would you like to restart Via now?");
    }
    msgBox.setIcon(QMessageBox::Question);
    QPushButton* restartButton = msgBox.addButton("Restart Now", QMessageBox::AcceptRole);
    QPushButton* laterButton = msgBox.addButton("Later", QMessageBox::RejectRole);
    msgBox.setDefaultButton(restartButton);
    Q_UNUSED(laterButton);

    msgBox.exec();

    if (msgBox.clickedButton() == restartButton) {
        emit restartRequested();
    } else {
        // Update snapshots so the prompt doesn't re-trigger for the same change
        captureRestartSettingSnapshots();
    }
}
