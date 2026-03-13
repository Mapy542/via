/**
 * @file SettingsWindow.cpp
 * @brief Implementation of the settings window
 */

#include "SettingsWindow.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QShowEvent>
#include <QStandardPaths>

#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "auth/TokenStorage.h"
#include "sync/ChangeProcessor.h"
#include "sync/SyncActionQueue.h"
#include "utils/AutostartManager.h"

SettingsWindow::SettingsWindow(GoogleAuthManager* authManager, SyncActionQueue* syncActionQueue,
                               ChangeProcessor* changeProcessor, GoogleDriveClient* driveClient, QWidget* parent)
    : QDialog(parent),
      m_authManager(authManager),
      m_syncActionQueue(syncActionQueue),
      m_changeProcessor(changeProcessor),
      m_driveClient(driveClient) {
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

    setupAccountTab();
    setupSyncTab();
    setupAdvancedTab();

    m_tabWidget->addTab(m_accountTab, "Account");
    m_tabWidget->addTab(m_syncTab, "Sync");
    m_tabWidget->addTab(m_advancedTab, "Advanced");

    mainLayout->addWidget(m_tabWidget);

    // Dialog buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_applyButton = new QPushButton("Apply", this);
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

void SettingsWindow::setupAccountTab() {
    m_accountTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_accountTab);

    // API Credentials group - MUST be configured first
    QGroupBox* apiGroup = new QGroupBox("Google API Credentials", m_accountTab);
    QVBoxLayout* apiLayout = new QVBoxLayout(apiGroup);

    QLabel* apiInfoLabel = new QLabel(
        "To use Via, you need to create OAuth 2.0 credentials in the "
        "<a href='https://console.cloud.google.com/apis/credentials'>Google Cloud Console</a>.\n"
        "Enable the Google Drive API and create OAuth Client ID credentials (Desktop app type).",
        m_accountTab);
    apiInfoLabel->setWordWrap(true);
    apiInfoLabel->setOpenExternalLinks(true);
    apiInfoLabel->setTextFormat(Qt::RichText);
    apiLayout->addWidget(apiInfoLabel);

    QFormLayout* credentialsForm = new QFormLayout();
    m_clientIdEdit = new QLineEdit(m_accountTab);
    m_clientIdEdit->setPlaceholderText("Enter your OAuth Client ID");
    m_clientIdEdit->setEchoMode(QLineEdit::Normal);
    credentialsForm->addRow("Client ID:", m_clientIdEdit);

    m_clientSecretEdit = new QLineEdit(m_accountTab);
    m_clientSecretEdit->setPlaceholderText("Enter your OAuth Client Secret");
    m_clientSecretEdit->setEchoMode(QLineEdit::Password);
    credentialsForm->addRow("Client Secret:", m_clientSecretEdit);
    apiLayout->addLayout(credentialsForm);

    QHBoxLayout* saveCredentialsLayout = new QHBoxLayout();
    m_saveCredentialsButton = new QPushButton("Save API Credentials", m_accountTab);
    saveCredentialsLayout->addWidget(m_saveCredentialsButton);
    saveCredentialsLayout->addStretch();
    apiLayout->addLayout(saveCredentialsLayout);

    layout->addWidget(apiGroup);

    // Account info group
    QGroupBox* accountGroup = new QGroupBox("Google Account", m_accountTab);
    QVBoxLayout* accountLayout = new QVBoxLayout(accountGroup);

    m_accountStatus = new QLabel("Not signed in", m_accountTab);
    m_accountStatus->setStyleSheet("QLabel { font-size: 14px; }");
    accountLayout->addWidget(m_accountStatus);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    m_loginButton = new QPushButton("Sign In with Google", m_accountTab);
    m_logoutButton = new QPushButton("Sign Out", m_accountTab);

    buttonLayout->addWidget(m_loginButton);
    buttonLayout->addWidget(m_logoutButton);
    buttonLayout->addStretch();
    accountLayout->addLayout(buttonLayout);

    layout->addWidget(accountGroup);

    // Storage info group
    QGroupBox* storageGroup = new QGroupBox("Storage", m_accountTab);
    QVBoxLayout* storageLayout = new QVBoxLayout(storageGroup);

    m_storageLabel = new QLabel("Retrieving storage info...", m_accountTab);
    storageLayout->addWidget(m_storageLabel);

    layout->addWidget(storageGroup);
    layout->addStretch();

    // Connect save credentials button
    connect(m_saveCredentialsButton, &QPushButton::clicked, this, &SettingsWindow::onSaveCredentialsClicked);

    // Connect buttons
    connect(m_loginButton, &QPushButton::clicked, this, [this]() {
        if (m_authManager) {
            m_authManager->authenticate();
        }
    });

    connect(m_logoutButton, &QPushButton::clicked, this, [this]() {
        if (m_authManager) {
            QMessageBox::StandardButton reply = QMessageBox::question(
                this, "Sign Out", "Are you sure you want to sign out?", QMessageBox::Yes | QMessageBox::No);
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
            m_storageLabel->setText("Not available");
        });
    }

    // Connect to storage info and user info signals
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::aboutInfoReceived, this, &SettingsWindow::onStorageInfoReceived);

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

        // Handle API errors for storage info
        connect(m_driveClient, &GoogleDriveClient::error, this, [this](const QString& operation, const QString& error) {
            if (operation == "getAboutInfo") {
                m_storageLabel->setText(QString("Unable to retrieve storage info: %1").arg(error));
            }
        });
    }
}

void SettingsWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);

    // Update storage info when window is shown
    if (m_authManager && m_authManager->isAuthenticated()) {
        updateStorageInfo();
    }
}

void SettingsWindow::setupSyncTab() {
    m_syncTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_syncTab);

    // Sync folder group
    QGroupBox* folderGroup = new QGroupBox("Sync Folder", m_syncTab);
    QVBoxLayout* folderLayout = new QVBoxLayout(folderGroup);

    QLabel* folderLabel = new QLabel("Local folder for Google Drive files:", m_syncTab);
    folderLayout->addWidget(folderLabel);

    QHBoxLayout* folderPathLayout = new QHBoxLayout();
    m_syncFolderEdit = new QLineEdit(m_syncTab);
    m_syncFolderEdit->setPlaceholderText(QDir::homePath() + "/GoogleDrive");
    m_browseFolderButton = new QPushButton("Browse...", m_syncTab);

    folderPathLayout->addWidget(m_syncFolderEdit, 1);
    folderPathLayout->addWidget(m_browseFolderButton);
    folderLayout->addLayout(folderPathLayout);

    layout->addWidget(folderGroup);

    // Sync mode group
    QGroupBox* syncModeGroup = new QGroupBox("Sync Mode", m_syncTab);
    QVBoxLayout* syncModeLayout = new QVBoxLayout(syncModeGroup);

    QLabel* syncModeLabel = new QLabel("Choose how files are synchronized:", m_syncTab);
    syncModeLayout->addWidget(syncModeLabel);

    QHBoxLayout* syncModeComboLayout = new QHBoxLayout();
    m_syncModeCombo = new QComboBox(m_syncTab);
    m_syncModeCombo->addItem("Keep Newest (bidirectional sync)", "keep-newest");
    m_syncModeCombo->addItem("Remote Read-Only (download only, never upload)", "remote-read-only");
    m_syncModeCombo->addItem("Remote No Delete (sync but don't delete remote files)", "remote-no-delete");
    syncModeComboLayout->addWidget(m_syncModeCombo);
    syncModeComboLayout->addStretch();
    syncModeLayout->addLayout(syncModeComboLayout);

    QLabel* syncModeInfoLabel = new QLabel(
        "<i><b>Keep Newest:</b> Full bidirectional sync - uploads and downloads based on "
        "modification time.<br>"
        "<b>Remote Read-Only:</b> Only downloads from remote, local changes are never uploaded.<br>"
        "<b>Remote No Delete:</b> Bidirectional sync but local file deletions won't delete remote "
        "files.</i>",
        m_syncTab);
    syncModeInfoLabel->setWordWrap(true);
    syncModeInfoLabel->setTextFormat(Qt::RichText);
    syncModeLayout->addWidget(syncModeInfoLabel);

    layout->addWidget(syncModeGroup);

    // Conflict resolution group
    QGroupBox* conflictGroup = new QGroupBox("Conflict Resolution", m_syncTab);
    QVBoxLayout* conflictLayout = new QVBoxLayout(conflictGroup);

    QLabel* conflictLabel = new QLabel("When a file is modified both locally and remotely:", m_syncTab);
    conflictLayout->addWidget(conflictLabel);

    QHBoxLayout* conflictComboLayout = new QHBoxLayout();
    m_conflictResolutionCombo = new QComboBox(m_syncTab);
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
        m_syncTab);
    conflictInfoLabel->setWordWrap(true);
    conflictInfoLabel->setTextFormat(Qt::RichText);
    conflictLayout->addWidget(conflictInfoLabel);

    layout->addWidget(conflictGroup);

    // Connect buttons
    connect(m_browseFolderButton, &QPushButton::clicked, this, &SettingsWindow::onBrowseFolderClicked);
}

void SettingsWindow::setupAdvancedTab() {
    m_advancedTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_advancedTab);

    // Startup group
    QGroupBox* startupGroup = new QGroupBox("Startup", m_advancedTab);
    QVBoxLayout* startupLayout = new QVBoxLayout(startupGroup);

    m_startOnLoginCheck = new QCheckBox("Start Via on system login", m_advancedTab);
    startupLayout->addWidget(m_startOnLoginCheck);

    layout->addWidget(startupGroup);

    // Appearance group
    QGroupBox* appearanceGroup = new QGroupBox("Appearance", m_advancedTab);
    QVBoxLayout* appearanceLayout = new QVBoxLayout(appearanceGroup);

    QHBoxLayout* themeLayout = new QHBoxLayout();
    themeLayout->addWidget(new QLabel("Icon theme:", m_advancedTab));
    m_themeOverrideCombo = new QComboBox(m_advancedTab);
    m_themeOverrideCombo->addItem("Auto (follow system)", 0);
    m_themeOverrideCombo->addItem("Light icons (for light backgrounds)", 1);
    m_themeOverrideCombo->addItem("Dark icons (for dark backgrounds)", 2);
    themeLayout->addWidget(m_themeOverrideCombo);
    themeLayout->addStretch();
    appearanceLayout->addLayout(themeLayout);

    layout->addWidget(appearanceGroup);

    // Notifications group
    QGroupBox* notifyGroup = new QGroupBox("Notifications", m_advancedTab);
    QVBoxLayout* notifyLayout = new QVBoxLayout(notifyGroup);

    m_showNotificationsCheck = new QCheckBox("Show desktop notifications", m_advancedTab);
    m_showNotificationsCheck->setChecked(true);
    notifyLayout->addWidget(m_showNotificationsCheck);

    layout->addWidget(notifyGroup);

    // FUSE / Sync System group
    QGroupBox* fuseGroup = new QGroupBox("Virtual File System (FUSE)", m_advancedTab);
    QVBoxLayout* fuseLayout = new QVBoxLayout(fuseGroup);

    QHBoxLayout* syncSystemLayout = new QHBoxLayout();
    syncSystemLayout->addWidget(new QLabel("Sync system:", m_advancedTab));
    m_syncSystemCombo = new QComboBox(m_advancedTab);
    m_syncSystemCombo->addItem("Mirror Only", "mirror-only");
    m_syncSystemCombo->addItem("FUSE Only", "fuse-only");
    m_syncSystemCombo->addItem("Both", "both");
    syncSystemLayout->addWidget(m_syncSystemCombo);
    syncSystemLayout->addStretch();
    fuseLayout->addLayout(syncSystemLayout);

    QHBoxLayout* mountLayout = new QHBoxLayout();
    mountLayout->addWidget(new QLabel("Mount point:", m_advancedTab));
    m_fuseMountPointEdit = new QLineEdit(m_advancedTab);
    m_fuseMountPointEdit->setPlaceholderText(QDir::homePath() + "/GoogleDriveFuse");
    m_fuseMountPointEdit->setEnabled(false);
    mountLayout->addWidget(m_fuseMountPointEdit);
    fuseLayout->addLayout(mountLayout);

    QHBoxLayout* cacheSizeLayout = new QHBoxLayout();
    cacheSizeLayout->addWidget(new QLabel("Maximum FUSE cache size:", m_advancedTab));
    m_cacheSize = new QSpinBox(m_advancedTab);
    m_cacheSize->setRange(100, 100000);
    m_cacheSize->setValue(5000);
    m_cacheSize->setSuffix(" MB");
    m_cacheSize->setEnabled(false);
    cacheSizeLayout->addWidget(m_cacheSize);
    cacheSizeLayout->addStretch();
    fuseLayout->addLayout(cacheSizeLayout);

    QPushButton* clearCacheButton = new QPushButton("Clear Cache", m_advancedTab);
    clearCacheButton->setEnabled(false);
    QHBoxLayout* clearLayout = new QHBoxLayout();
    clearLayout->addWidget(clearCacheButton);
    clearLayout->addStretch();
    fuseLayout->addLayout(clearLayout);

    layout->addWidget(fuseGroup);

    // Debug group
    QGroupBox* debugGroup = new QGroupBox("Debug", m_advancedTab);
    QVBoxLayout* debugLayout = new QVBoxLayout(debugGroup);

    m_debugModeCheck = new QCheckBox("Enable debug logging", m_advancedTab);
    debugLayout->addWidget(m_debugModeCheck);

    layout->addWidget(debugGroup);
    layout->addStretch();

    // Enable/disable FUSE-related widgets based on sync system selection
    auto updateFuseWidgets = [this, clearCacheButton]() {
        bool fuseEnabled = (m_syncSystemCombo->currentData().toString() != "mirror-only");
        m_fuseMountPointEdit->setEnabled(fuseEnabled);
        m_cacheSize->setEnabled(fuseEnabled);
        clearCacheButton->setEnabled(fuseEnabled);
    };
    connect(m_syncSystemCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateFuseWidgets);
    updateFuseWidgets();

    // Connect clear cache button
    connect(clearCacheButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::StandardButton reply =
            QMessageBox::question(this, "Clear Cache",
                                  "Are you sure you want to clear the cache?\n\n"
                                  "This will remove all cached file data but won't affect your files.",
                                  QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            // GPT5.3 #12: actually remove cached files
            QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            QDir cacheDir(cachePath);
            if (cacheDir.exists()) {
                // Remove all files / sub-dirs inside the cache dir
                for (const QString& entry : cacheDir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
                    QString fullPath = cacheDir.filePath(entry);
                    QFileInfo fi(fullPath);
                    if (fi.isDir()) {
                        QDir(fullPath).removeRecursively();
                    } else {
                        QFile::remove(fullPath);
                    }
                }
            }

            emit clearCacheRequested();

            QMessageBox::information(this, "Cache Cleared", "The cache has been cleared successfully.");
        }
    });
}

void SettingsWindow::loadSettings() {
    // Load API credentials from token storage (via QSettings auth/ keys)
    m_clientIdEdit->setText(m_settings.value("auth/clientIdDisplay", "").toString());
    // Note: We don't display the secret for security, just show placeholder if set
    if (!m_settings.value("auth/clientSecret").toString().isEmpty()) {
        m_clientSecretEdit->setPlaceholderText("••••••••••••••••");
    }

    // Sync settings
    m_syncFolderEdit->setText(m_settings.value("sync/folder", QDir::homePath() + "/GoogleDrive").toString());

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

    // Advanced settings
    m_startOnLoginCheck->setChecked(m_settings.value("advanced/startOnLogin", false).toBool());
    m_showNotificationsCheck->setChecked(m_settings.value("advanced/showNotifications", true).toBool());

    // Theme override setting
    int themeOverride = m_settings.value("advanced/themeOverride", 0).toInt();
    for (int i = 0; i < m_themeOverrideCombo->count(); ++i) {
        if (m_themeOverrideCombo->itemData(i).toInt() == themeOverride) {
            m_themeOverrideCombo->setCurrentIndex(i);
            break;
        }
    }

    // Sync system dropdown (migrate legacy enableFuse boolean)
    QString syncSystem = m_settings.value("advanced/syncSystem", "").toString();
    if (syncSystem.isEmpty()) {
        // Migrate from old enableFuse boolean
        bool legacyFuse = m_settings.value("advanced/enableFuse", false).toBool();
        syncSystem = legacyFuse ? "both" : "mirror-only";
    }
    if (!setComboById(m_syncSystemCombo, syncSystem)) {
        setComboById(m_syncSystemCombo, "mirror-only");
    }
    m_fuseMountPointEdit->setText(
        m_settings.value("advanced/fuseMountPoint", QDir::homePath() + "/GoogleDriveFuse").toString());
    m_cacheSize->setValue(m_settings.value("advanced/cacheSize", 5000).toInt());
    m_debugModeCheck->setChecked(m_settings.value("advanced/debugMode", false).toBool());

    // Capture snapshots of restart-required settings
    m_originalSyncFolder = m_syncFolderEdit->text();
    m_originalSyncMode = m_syncModeCombo->currentData().toString();
    m_originalConflictStrategy = m_conflictResolutionCombo->currentData().toString();
    m_originalSyncSystem = m_syncSystemCombo->currentData().toString();
    m_originalFuseMountPoint = m_fuseMountPointEdit->text();
    m_originalCacheSize = m_cacheSize->value();
}

void SettingsWindow::saveSettings() {
    // Sync settings
    m_settings.setValue("sync/folder", m_syncFolderEdit->text());
    m_settings.setValue("sync/syncMode", m_syncModeCombo->currentData().toString());
    m_settings.setValue("sync/conflictStrategy", m_conflictResolutionCombo->currentData().toString());

    // Advanced settings
    m_settings.setValue("advanced/startOnLogin", m_startOnLoginCheck->isChecked());
    AutostartManager::setAutostart(m_startOnLoginCheck->isChecked());
    m_settings.setValue("advanced/showNotifications", m_showNotificationsCheck->isChecked());
    m_settings.setValue("advanced/themeOverride", m_themeOverrideCombo->currentData().toInt());
    m_settings.setValue("advanced/syncSystem", m_syncSystemCombo->currentData().toString());
    m_settings.setValue("advanced/fuseMountPoint", m_fuseMountPointEdit->text());
    m_settings.setValue("advanced/cacheSize", m_cacheSize->value());
    m_settings.setValue("advanced/debugMode", m_debugModeCheck->isChecked());

    m_settings.sync();

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
        this, "Select Sync Folder", m_syncFolderEdit->text().isEmpty() ? QDir::homePath() : m_syncFolderEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!folder.isEmpty()) {
        m_syncFolderEdit->setText(folder);
    }
}

void SettingsWindow::onSaveCredentialsClicked() {
    QString clientId = m_clientIdEdit->text().trimmed();
    QString clientSecret = m_clientSecretEdit->text().trimmed();

    if (clientId.isEmpty()) {
        QMessageBox::warning(this, "Missing Client ID", "Please enter your OAuth Client ID.");
        return;
    }

    if (clientSecret.isEmpty() && m_settings.value("auth/clientSecret").toString().isEmpty()) {
        QMessageBox::warning(this, "Missing Client Secret", "Please enter your OAuth Client Secret.");
        return;
    }

    // Save credentials via TokenStorage (which encodes them)
    // We store the display version of client ID for showing in UI
    m_settings.setValue("auth/clientIdDisplay", clientId);

    // Use TokenStorage to properly encode and save credentials
    TokenStorage storage;

    // If client secret is not provided (placeholder shown), keep the existing one
    if (!clientSecret.isEmpty()) {
        storage.saveCredentials(clientId, clientSecret);
    } else {
        // Just update client ID, keep existing secret
        QString existingSecret = storage.getClientSecret();
        storage.saveCredentials(clientId, existingSecret);
    }

    // Update the auth manager with new credentials
    if (m_authManager) {
        QString actualSecret = clientSecret.isEmpty() ? storage.getClientSecret() : clientSecret;
        m_authManager->setCredentials(clientId, actualSecret);
    }

    // Clear the secret field for security
    m_clientSecretEdit->clear();
    m_clientSecretEdit->setPlaceholderText("••••••••••••••••");

    QMessageBox::information(this, "Credentials Saved",
                             "Your Google API credentials have been saved.\n\n"
                             "You can now sign in with your Google account.");
}

void SettingsWindow::updateStorageInfo() {
    if (m_driveClient && m_authManager && m_authManager->isAuthenticated()) {
        m_storageLabel->setText("Retrieving storage info...");
        m_driveClient->getAboutInfo();
    } else {
        m_storageLabel->setText("Not available");
    }
}

void SettingsWindow::onStorageInfoReceived(qint64 storageUsed, qint64 storageLimit) {
    // Format bytes to human-readable format
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
        } else {
            return QString::number(bytes) + " B";
        }
    };

    QString usedStr = formatBytes(storageUsed);
    QString limitStr = formatBytes(storageLimit);
    double percentUsed = storageLimit > 0 ? (storageUsed * 100.0 / storageLimit) : 0.0;

    m_storageLabel->setText(
        QString("Storage usage: %1 / %2 (%3%)").arg(usedStr).arg(limitStr).arg(QString::number(percentUsed, 'f', 1)));
}

bool SettingsWindow::checkRestartRequired() const {
    if (m_syncFolderEdit->text() != m_originalSyncFolder) return true;
    if (m_syncModeCombo->currentData().toString() != m_originalSyncMode) return true;
    if (m_conflictResolutionCombo->currentData().toString() != m_originalConflictStrategy) return true;
    if (m_syncSystemCombo->currentData().toString() != m_originalSyncSystem) return true;
    if (m_fuseMountPointEdit->text() != m_originalFuseMountPoint) return true;
    if (m_cacheSize->value() != m_originalCacheSize) return true;
    return false;
}

void SettingsWindow::promptRestart() {
    if (!checkRestartRequired()) {
        return;
    }

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Restart Required");
    msgBox.setText(
        "One or more settings you changed require a restart to take effect.\n\n"
        "Would you like to restart Via now?");
    msgBox.setIcon(QMessageBox::Question);
    QPushButton* restartButton = msgBox.addButton("Restart Now", QMessageBox::AcceptRole);
    QPushButton* laterButton = msgBox.addButton("Later", QMessageBox::RejectRole);
    msgBox.setDefaultButton(restartButton);
    Q_UNUSED(laterButton);

    msgBox.exec();

    if (msgBox.clickedButton() == restartButton) {
        // Spawn a new instance and quit the current one
        QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments());
        QApplication::quit();
    } else {
        // Update snapshots so the prompt doesn't re-trigger for the same change
        m_originalSyncFolder = m_syncFolderEdit->text();
        m_originalSyncMode = m_syncModeCombo->currentData().toString();
        m_originalConflictStrategy = m_conflictResolutionCombo->currentData().toString();
        m_originalSyncSystem = m_syncSystemCombo->currentData().toString();
        m_originalFuseMountPoint = m_fuseMountPointEdit->text();
        m_originalCacheSize = m_cacheSize->value();
    }
}
