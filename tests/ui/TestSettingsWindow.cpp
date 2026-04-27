/**
 * @file TestSettingsWindow.cpp
 * @brief Regression tests for credential persistence in SettingsWindow.
 */

#include <QApplication>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <QtTest/QtTest>

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

class TestSettingsWindow : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void init();
    void cleanup();

    void testReopenShowsStoredIdAndKeepsStoredSecretOnResave();

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

    SettingsWindow firstWindow(nullptr, nullptr, nullptr, nullptr, nullptr, &store);
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

    SettingsWindow reopenedWindow(nullptr, nullptr, nullptr, nullptr, nullptr, &store);
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

QTEST_MAIN(TestSettingsWindow)
#include "TestSettingsWindow.moc"