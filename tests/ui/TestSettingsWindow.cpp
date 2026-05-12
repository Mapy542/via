/**
 * @file TestSettingsWindow.cpp
 * @brief Regression tests for SettingsWindow behavior.
 */

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <QtTest/QtTest>

#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "auth/TokenStorage.h"
#include "sync/RuntimePauseController.h"
#include "ui/SettingsWindow.h"

class FakeCredentialStore : public SettingsCredentialStore {
   public:
    QString getClientId() const override { return clientId; }
    QString getClientSecret() const override { return clientSecret; }

    void saveCredentials(const QString& newClientId, const QString& newClientSecret) override {
        clientId = newClientId;
        clientSecret = newClientSecret;
    }

    QString clientId;
    QString clientSecret;
};

class FakeAuthenticatedAuthManager : public GoogleAuthManager {
    Q_OBJECT

   public:
    explicit FakeAuthenticatedAuthManager(TokenStorage* tokenStorage, QObject* parent = nullptr)
        : GoogleAuthManager(tokenStorage, parent) {}

    bool isAuthenticated() const override { return m_authenticated; }

    void setAuthenticated(bool authenticated) { m_authenticated = authenticated; }

   private:
    bool m_authenticated = true;
};

class FakeDriveClientForSettingsWindow : public GoogleDriveClient {
    Q_OBJECT

   public:
    enum class FailureMode {
        Auth,
        NonAuth,
    };

    explicit FakeDriveClientForSettingsWindow(GoogleAuthManager* authManager,
                                              QObject* parent = nullptr)
        : GoogleDriveClient(authManager, parent) {}

    void setFailureMode(FailureMode mode) { m_failureMode = mode; }

    void getAboutInfo() override {
        ++aboutInfoRequestCount;

        QTimer::singleShot(0, this, [this]() {
            switch (m_failureMode) {
                case FailureMode::Auth:
                    emit errorDetailed(QStringLiteral("getAboutInfo"),
                                       QStringLiteral("Unauthorized"), 401, QString(), QString());
                    emit authenticationFailure(QStringLiteral("getAboutInfo"), 401,
                                               QStringLiteral("Unauthorized"));
                    break;

                case FailureMode::NonAuth:
                    emit error(QStringLiteral("getAboutInfo"),
                               QStringLiteral("Internal Server Error"));
                    emit errorDetailed(QStringLiteral("getAboutInfo"),
                                       QStringLiteral("Internal Server Error"), 500, QString(),
                                       QString());
                    break;
            }
        });
    }

    int aboutInfoRequestCount = 0;

   private:
    FailureMode m_failureMode = FailureMode::Auth;
};

class TestSettingsWindow : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void init();
    void cleanup();

    void testReopenShowsStoredIdAndKeepsStoredSecretOnResave();
    void testAutoPauseCheckboxPersistsAndUpdatesController();
    void testWindowStaysUsableWhenAboutInfoAuthFailsOnOpen();
    void testWindowStaysUsableWhenAboutInfoNonAuthFailsOnOpen();
    void testMirrorEnabledKeepsNativeDocModeEditableWhenFuseDisabled();
    void testBothSyncSystemsCanBeDisabled();
    void testDuplicateNameStrategyLivesInCommonTabAndStaysEditable();
    void testMirrorPerformanceControlsPersistWithoutRestartPrompt();
    void testNativeDocModeRestartPromptMentionsMirrorRebuild();

   private:
    static void acceptNextMessageBox();
};

void TestSettingsWindow::initTestCase() {
    QCoreApplication::setOrganizationName("ViaTests");
    QCoreApplication::setApplicationName("TestSettingsWindow");
}

void TestSettingsWindow::init() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

void TestSettingsWindow::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

void TestSettingsWindow::acceptNextMessageBox() {
    QTimer::singleShot(0, []() {
        if (auto* messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            messageBox->accept();
        }
    });
}

void TestSettingsWindow::testReopenShowsStoredIdAndKeepsStoredSecretOnResave() {
    FakeCredentialStore store;

    SettingsWindow firstWindow(nullptr, nullptr, nullptr, &store);
    auto* firstClientIdEdit = firstWindow.findChild<QLineEdit*>("settingsClientIdEdit");
    auto* firstClientSecretEdit = firstWindow.findChild<QLineEdit*>("settingsClientSecretEdit");
    auto* firstSaveButton = firstWindow.findChild<QPushButton*>("settingsSaveCredentialsButton");

    QVERIFY(firstClientIdEdit != nullptr);
    QVERIFY(firstClientSecretEdit != nullptr);
    QVERIFY(firstSaveButton != nullptr);

    firstClientIdEdit->setText("client-id-1");
    firstClientSecretEdit->setText("secret-1");

    acceptNextMessageBox();
    firstSaveButton->click();

    QCOMPARE(store.clientId, QStringLiteral("client-id-1"));
    QCOMPARE(store.clientSecret, QStringLiteral("secret-1"));
    QCOMPARE(firstClientSecretEdit->text(), QString());
    QCOMPARE(firstClientSecretEdit->placeholderText(), QStringLiteral("••••••••••••••••"));

    SettingsWindow reopenedWindow(nullptr, nullptr, nullptr, &store);
    auto* reopenedClientIdEdit = reopenedWindow.findChild<QLineEdit*>("settingsClientIdEdit");
    auto* reopenedClientSecretEdit =
        reopenedWindow.findChild<QLineEdit*>("settingsClientSecretEdit");
    auto* reopenedSaveButton =
        reopenedWindow.findChild<QPushButton*>("settingsSaveCredentialsButton");

    QVERIFY(reopenedClientIdEdit != nullptr);
    QVERIFY(reopenedClientSecretEdit != nullptr);
    QVERIFY(reopenedSaveButton != nullptr);

    QCOMPARE(reopenedClientIdEdit->text(), QStringLiteral("client-id-1"));
    QCOMPARE(reopenedClientSecretEdit->text(), QString());
    QCOMPARE(reopenedClientSecretEdit->placeholderText(), QStringLiteral("••••••••••••••••"));

    reopenedClientIdEdit->setText("client-id-2");
    reopenedClientSecretEdit->clear();

    acceptNextMessageBox();
    reopenedSaveButton->click();

    QCOMPARE(store.clientId, QStringLiteral("client-id-2"));
    QCOMPARE(store.clientSecret, QStringLiteral("secret-1"));
    QCOMPARE(reopenedClientSecretEdit->text(), QString());
    QCOMPARE(reopenedClientSecretEdit->placeholderText(), QStringLiteral("••••••••••••••••"));

    QSettings settings;
    QCOMPARE(settings.value("auth/clientIdDisplay").toString(), QStringLiteral("client-id-2"));
    QVERIFY(!settings.contains("auth/clientSecret"));
}

void TestSettingsWindow::testAutoPauseCheckboxPersistsAndUpdatesController() {
    RuntimePauseController controller;
    controller.setAutoPauseReasonActive(RuntimePauseController::AutoPauseReason::Offline, true);
    QVERIFY(controller.isEffectivelyPaused());

    SettingsWindow window(nullptr, nullptr, nullptr, nullptr, &controller);
    auto* autoPauseCheck = window.findChild<QCheckBox*>("settingsAutoPauseCheck");

    QVERIFY(autoPauseCheck != nullptr);
    QVERIFY(autoPauseCheck->isChecked());

    autoPauseCheck->setChecked(false);
    window.saveSettings();

    {
        QSettings settings;
        QCOMPARE(settings.value("advanced/autoPauseEnabled", true).toBool(), false);
    }
    QVERIFY(!controller.isAutoPauseEnabled());
    QVERIFY(controller.isDriveApiAllowed());

    SettingsWindow reopenedWindow(nullptr, nullptr, nullptr, nullptr, &controller);
    auto* reopenedAutoPauseCheck = reopenedWindow.findChild<QCheckBox*>("settingsAutoPauseCheck");

    QVERIFY(reopenedAutoPauseCheck != nullptr);
    QVERIFY(!reopenedAutoPauseCheck->isChecked());

    reopenedAutoPauseCheck->setChecked(true);
    reopenedWindow.saveSettings();

    {
        QSettings settings;
        QCOMPARE(settings.value("advanced/autoPauseEnabled", true).toBool(), true);
    }
    QVERIFY(controller.isAutoPauseEnabled());
    QVERIFY(controller.isEffectivelyPaused());
    QCOMPARE(controller.effectiveStatusText(), QStringLiteral("Offline"));
}

void TestSettingsWindow::testWindowStaysUsableWhenAboutInfoAuthFailsOnOpen() {
    TokenStorage tokenStorage;
    FakeAuthenticatedAuthManager authManager(&tokenStorage);
    FakeDriveClientForSettingsWindow driveClient(&authManager);
    driveClient.setFailureMode(FakeDriveClientForSettingsWindow::FailureMode::Auth);

    SettingsWindow window(&authManager, &driveClient);
    auto* storageLabel = window.findChild<QLabel*>("settingsStorageInfoLabel");
    auto* clientIdEdit = window.findChild<QLineEdit*>("settingsClientIdEdit");

    QVERIFY(storageLabel != nullptr);
    QVERIFY(clientIdEdit != nullptr);

    window.show();

    QTRY_VERIFY(window.isVisible());
    QTRY_COMPARE(driveClient.aboutInfoRequestCount, 1);
    QTRY_COMPARE(storageLabel->text(),
                 QStringLiteral("Unable to retrieve storage info: Unauthorized"));

    QVERIFY(window.isVisible());
    QVERIFY(window.isEnabled());
    QVERIFY(clientIdEdit->isEnabled());
}

void TestSettingsWindow::testWindowStaysUsableWhenAboutInfoNonAuthFailsOnOpen() {
    TokenStorage tokenStorage;
    FakeAuthenticatedAuthManager authManager(&tokenStorage);
    FakeDriveClientForSettingsWindow driveClient(&authManager);
    driveClient.setFailureMode(FakeDriveClientForSettingsWindow::FailureMode::NonAuth);

    SettingsWindow window(&authManager, &driveClient);
    auto* storageLabel = window.findChild<QLabel*>("settingsStorageInfoLabel");
    auto* clientIdEdit = window.findChild<QLineEdit*>("settingsClientIdEdit");

    QVERIFY(storageLabel != nullptr);
    QVERIFY(clientIdEdit != nullptr);

    window.show();

    QTRY_VERIFY(window.isVisible());
    QTRY_COMPARE(driveClient.aboutInfoRequestCount, 1);
    QTRY_COMPARE(storageLabel->text(),
                 QStringLiteral("Unable to retrieve storage info: Internal Server Error"));

    QVERIFY(window.isVisible());
    QVERIFY(window.isEnabled());
    QVERIFY(clientIdEdit->isEnabled());
}

void TestSettingsWindow::testMirrorEnabledKeepsNativeDocModeEditableWhenFuseDisabled() {
    SettingsWindow window(nullptr, nullptr);
    auto* mirrorEnabledCheck = window.findChild<QCheckBox*>("settingsMirrorEnabledCheck");
    auto* fuseEnabledCheck = window.findChild<QCheckBox*>("settingsFuseEnabledCheck");
    auto* nativeDocCombo = window.findChild<QComboBox*>("settingsNativeDocModeCombo");
    auto* nativeDocInfoLabel = window.findChild<QLabel*>("settingsNativeDocModeInfoLabel");

    QVERIFY(mirrorEnabledCheck != nullptr);
    QVERIFY(fuseEnabledCheck != nullptr);
    QVERIFY(nativeDocCombo != nullptr);
    QVERIFY(nativeDocInfoLabel != nullptr);

    mirrorEnabledCheck->setChecked(true);
    fuseEnabledCheck->setChecked(false);

    QVERIFY(nativeDocCombo->isEnabled());
    QVERIFY(nativeDocInfoLabel->text().contains(QStringLiteral("mirror sync and FUSE")));
}

void TestSettingsWindow::testBothSyncSystemsCanBeDisabled() {
    SettingsWindow window(nullptr, nullptr);
    auto* mirrorEnabledCheck = window.findChild<QCheckBox*>("settingsMirrorEnabledCheck");
    auto* fuseEnabledCheck = window.findChild<QCheckBox*>("settingsFuseEnabledCheck");
    auto* nativeDocCombo = window.findChild<QComboBox*>("settingsNativeDocModeCombo");

    QVERIFY(mirrorEnabledCheck != nullptr);
    QVERIFY(fuseEnabledCheck != nullptr);
    QVERIFY(nativeDocCombo != nullptr);

    mirrorEnabledCheck->setChecked(false);
    fuseEnabledCheck->setChecked(false);

    QVERIFY(!nativeDocCombo->isEnabled());

    window.saveSettings();

    QSettings settings;
    QCOMPARE(settings.value("advanced/syncSystem").toString(), QStringLiteral("none"));
}

void TestSettingsWindow::testDuplicateNameStrategyLivesInCommonTabAndStaysEditable() {
    SettingsWindow window(nullptr, nullptr);
    auto* tabWidget = window.findChild<QTabWidget*>();
    auto* mirrorEnabledCheck = window.findChild<QCheckBox*>("settingsMirrorEnabledCheck");
    auto* fuseEnabledCheck = window.findChild<QCheckBox*>("settingsFuseEnabledCheck");
    auto* duplicateNameCombo = window.findChild<QComboBox*>("settingsDuplicateNameCombo");

    QVERIFY(tabWidget != nullptr);
    QVERIFY(mirrorEnabledCheck != nullptr);
    QVERIFY(fuseEnabledCheck != nullptr);
    QVERIFY(duplicateNameCombo != nullptr);

    bool foundCommonTab = false;
    for (int index = 0; index < tabWidget->count(); ++index) {
        if (tabWidget->tabText(index) == QStringLiteral("Common")) {
            foundCommonTab = true;
            break;
        }
    }
    QVERIFY(foundCommonTab);

    mirrorEnabledCheck->setChecked(false);
    fuseEnabledCheck->setChecked(false);

    QVERIFY(duplicateNameCombo->isEnabled());

    const int numericSuffixIndex = duplicateNameCombo->findData(QStringLiteral("numeric-suffix"));
    QVERIFY(numericSuffixIndex >= 0);
    duplicateNameCombo->setCurrentIndex(numericSuffixIndex);

    window.saveSettings();

    {
        QSettings settings;
        QCOMPARE(settings.value("sync/duplicateNameStrategy").toString(),
                 QStringLiteral("numeric-suffix"));
    }

    SettingsWindow reopenedWindow(nullptr, nullptr);
    auto* reopenedDuplicateNameCombo =
        reopenedWindow.findChild<QComboBox*>("settingsDuplicateNameCombo");

    QVERIFY(reopenedDuplicateNameCombo != nullptr);
    QCOMPARE(reopenedDuplicateNameCombo->currentData().toString(),
             QStringLiteral("numeric-suffix"));
}

void TestSettingsWindow::testMirrorPerformanceControlsPersistWithoutRestartPrompt() {
    SettingsWindow window(nullptr, nullptr);
    auto* dormantTimeSpin = window.findChild<QSpinBox*>("settingsMirrorDormantTimeSpin");
    auto* dutyCycleSpin = window.findChild<QSpinBox*>("settingsMirrorDutyCycleSpin");
    auto* applyButton = window.findChild<QPushButton*>("settingsApplyButton");

    QVERIFY(dormantTimeSpin != nullptr);
    QVERIFY(dutyCycleSpin != nullptr);
    QVERIFY(applyButton != nullptr);

    window.show();
    QTRY_VERIFY(window.isVisible());

    dormantTimeSpin->setValue(250);
    dutyCycleSpin->setValue(35);

    bool promptSeen = false;
    QTimer::singleShot(0, [&promptSeen]() {
        if (auto* messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            promptSeen = true;
            messageBox->reject();
        }
    });

    applyButton->click();

    QVERIFY(!promptSeen);

    {
        QSettings settings;
        QCOMPARE(settings.value("sync/mirrorDormantTimeMs").toInt(), 250);
        QCOMPARE(settings.value("sync/mirrorDutyCyclePct").toInt(), 35);
    }

    SettingsWindow reopenedWindow(nullptr, nullptr);
    auto* reopenedDormantTimeSpin =
        reopenedWindow.findChild<QSpinBox*>("settingsMirrorDormantTimeSpin");
    auto* reopenedDutyCycleSpin =
        reopenedWindow.findChild<QSpinBox*>("settingsMirrorDutyCycleSpin");

    QVERIFY(reopenedDormantTimeSpin != nullptr);
    QVERIFY(reopenedDutyCycleSpin != nullptr);
    QCOMPARE(reopenedDormantTimeSpin->value(), 250);
    QCOMPARE(reopenedDutyCycleSpin->value(), 35);
}

void TestSettingsWindow::testNativeDocModeRestartPromptMentionsMirrorRebuild() {
    SettingsWindow window(nullptr, nullptr);
    auto* mirrorEnabledCheck = window.findChild<QCheckBox*>("settingsMirrorEnabledCheck");
    auto* fuseEnabledCheck = window.findChild<QCheckBox*>("settingsFuseEnabledCheck");
    auto* nativeDocCombo = window.findChild<QComboBox*>("settingsNativeDocModeCombo");
    auto* applyButton = window.findChild<QPushButton*>("settingsApplyButton");

    QVERIFY(mirrorEnabledCheck != nullptr);
    QVERIFY(fuseEnabledCheck != nullptr);
    QVERIFY(nativeDocCombo != nullptr);
    QVERIFY(applyButton != nullptr);

    window.show();
    QTRY_VERIFY(window.isVisible());

    mirrorEnabledCheck->setChecked(true);
    fuseEnabledCheck->setChecked(false);

    const int browserShortcutIndex = nativeDocCombo->findData(QStringLiteral("browser-shortcut"));
    QVERIFY(browserShortcutIndex >= 0);
    nativeDocCombo->setCurrentIndex(browserShortcutIndex);

    bool promptSeen = false;
    QString promptText;
    QString promptInfo;
    QTimer::singleShot(0, [&promptSeen, &promptText, &promptInfo]() {
        if (auto* messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            promptSeen = true;
            promptText = messageBox->text();
            promptInfo = messageBox->informativeText();
            messageBox->reject();
        }
    });

    applyButton->click();

    QVERIFY(promptSeen);
    QVERIFY(promptText.contains(QStringLiteral("restart to take effect"), Qt::CaseInsensitive));
    QVERIFY(promptInfo.contains(QStringLiteral("native-document artifacts")));
    QVERIFY(promptInfo.contains(QStringLiteral("sync folder")));
    QVERIFY(promptInfo.contains(QStringLiteral("next launch")));
}

QTEST_MAIN(TestSettingsWindow)
#include "TestSettingsWindow.moc"