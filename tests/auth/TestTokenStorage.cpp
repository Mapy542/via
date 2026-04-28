/**
 * @file TestTokenStorage.cpp
 * @brief Tests for the TokenStorage fallback file backend
 */

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <latch>
#include <thread>

#include "auth/TokenStorage.h"

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

class TestTokenStorage : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void testFallbackRoundTripUsesJsonFile();
    void testDeletingOneSecretPreservesOtherSecrets();
    void testDeletingLastSecretRemovesFallbackFile();
    void testConcurrentFallbackMutationsStayConsistent();

   private:
    QString fallbackFilePath() const;
    QJsonObject readFallbackObject() const;

    QTemporaryDir* m_tempDir = nullptr;
};

void TestTokenStorage::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());

    qputenv("HOME", m_tempDir->path().toUtf8());
    qputenv("XDG_DATA_HOME", (m_tempDir->path() + QStringLiteral("/xdg-data")).toUtf8());
    qputenv("VIA_FORCE_TOKEN_FALLBACK", "1");
    QStandardPaths::setTestModeEnabled(true);
}

void TestTokenStorage::cleanup() {
    QStandardPaths::setTestModeEnabled(false);
    qunsetenv("VIA_FORCE_TOKEN_FALLBACK");
    qunsetenv("XDG_DATA_HOME");

    delete m_tempDir;
    m_tempDir = nullptr;
}

QString TestTokenStorage::fallbackFilePath() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/secure_tokens.json");
}

QJsonObject TestTokenStorage::readFallbackObject() const {
    QFile file(fallbackFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to read fallback token file in test:" << fallbackFilePath();
        return QJsonObject();
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "Fallback token file was not valid JSON in test:" << fallbackFilePath()
                   << parseError.errorString();
        return QJsonObject();
    }

    return document.object();
}

void TestTokenStorage::testFallbackRoundTripUsesJsonFile() {
    const QDateTime expiry = QDateTime::currentDateTimeUtc().addSecs(3600);

    TokenStorage writer;
    writer.saveTokens(QStringLiteral("access-token"), QStringLiteral("refresh-token"), expiry);
    writer.saveCredentials(QStringLiteral("client-id"), QStringLiteral("client-secret"));

    TokenStorage reader;
    QCOMPARE(reader.getAccessToken(), QStringLiteral("access-token"));
    QCOMPARE(reader.getRefreshToken(), QStringLiteral("refresh-token"));
    QCOMPARE(reader.getClientId(), QStringLiteral("client-id"));
    QCOMPARE(reader.getClientSecret(), QStringLiteral("client-secret"));

    QFile file(fallbackFilePath());
    QVERIFY(file.exists());

    const QJsonObject root = readFallbackObject();
    QCOMPARE(root.value(QStringLiteral("accessToken")).toString(), QStringLiteral("access-token"));
    QCOMPARE(root.value(QStringLiteral("refreshToken")).toString(),
             QStringLiteral("refresh-token"));
    QCOMPARE(root.value(QStringLiteral("clientId")).toString(), QStringLiteral("client-id"));
    QCOMPARE(root.value(QStringLiteral("clientSecret")).toString(),
             QStringLiteral("client-secret"));

#ifdef Q_OS_UNIX
    struct stat fileStatus{};
    QVERIFY(::stat(QFile::encodeName(fallbackFilePath()).constData(), &fileStatus) == 0);
    QCOMPARE(static_cast<unsigned int>(fileStatus.st_mode & 0777U),
             static_cast<unsigned int>(S_IRUSR | S_IWUSR));
#endif
}

void TestTokenStorage::testDeletingOneSecretPreservesOtherSecrets() {
    const QDateTime expiry = QDateTime::currentDateTimeUtc().addSecs(3600);

    TokenStorage storage;
    storage.saveTokens(QStringLiteral("access-token"), QStringLiteral("refresh-token"), expiry);
    storage.saveCredentials(QStringLiteral("client-id"), QStringLiteral("client-secret"));

    storage.saveCredentials(QStringLiteral("client-id"), QString());

    const QJsonObject root = readFallbackObject();
    QCOMPARE(root.value(QStringLiteral("accessToken")).toString(), QStringLiteral("access-token"));
    QCOMPARE(root.value(QStringLiteral("refreshToken")).toString(),
             QStringLiteral("refresh-token"));
    QCOMPARE(root.value(QStringLiteral("clientId")).toString(), QStringLiteral("client-id"));
    QVERIFY(!root.contains(QStringLiteral("clientSecret")));

    TokenStorage reader;
    QCOMPARE(reader.getAccessToken(), QStringLiteral("access-token"));
    QCOMPARE(reader.getRefreshToken(), QStringLiteral("refresh-token"));
    QCOMPARE(reader.getClientId(), QStringLiteral("client-id"));
    QVERIFY(reader.getClientSecret().isEmpty());
}

void TestTokenStorage::testDeletingLastSecretRemovesFallbackFile() {
    TokenStorage storage;
    storage.saveCredentials(QStringLiteral("client-id"), QStringLiteral("client-secret"));

    QFile file(fallbackFilePath());
    QVERIFY(file.exists());

    storage.saveCredentials(QString(), QString());

    QVERIFY(!QFile::exists(fallbackFilePath()));

    TokenStorage reader;
    QVERIFY(reader.getClientId().isEmpty());
    QVERIFY(reader.getClientSecret().isEmpty());
}

void TestTokenStorage::testConcurrentFallbackMutationsStayConsistent() {
    constexpr int rounds = 25;

    for (int round = 0; round < rounds; ++round) {
        QFile::remove(fallbackFilePath());

        const QString accessToken = QStringLiteral("access-%1").arg(round);
        const QString refreshToken = QStringLiteral("refresh-%1").arg(round);
        const QString clientId = QStringLiteral("client-%1").arg(round);
        const QString clientSecret = QStringLiteral("secret-%1").arg(round);

        std::latch readyToWrite(2);
        std::latch startWriting(1);

        std::thread tokenWriter([&]() {
            TokenStorage storage;
            readyToWrite.count_down();
            startWriting.wait();
            storage.saveTokens(accessToken, refreshToken,
                               QDateTime::currentDateTimeUtc().addSecs(3600));
        });

        std::thread credentialWriter([&]() {
            TokenStorage storage;
            readyToWrite.count_down();
            startWriting.wait();
            storage.saveCredentials(clientId, clientSecret);
        });

        readyToWrite.wait();
        startWriting.count_down();
        tokenWriter.join();
        credentialWriter.join();

        QJsonObject root = readFallbackObject();
        QCOMPARE(root.value(QStringLiteral("accessToken")).toString(), accessToken);
        QCOMPARE(root.value(QStringLiteral("refreshToken")).toString(), refreshToken);
        QCOMPARE(root.value(QStringLiteral("clientId")).toString(), clientId);
        QCOMPARE(root.value(QStringLiteral("clientSecret")).toString(), clientSecret);

        const QString updatedClientId = QStringLiteral("client-updated-%1").arg(round);
        const QString updatedClientSecret = QStringLiteral("secret-updated-%1").arg(round);

        std::latch readyToMutate(2);
        std::latch startMutating(1);

        std::thread tokenDeleter([&]() {
            TokenStorage storage;
            readyToMutate.count_down();
            startMutating.wait();
            storage.clearTokens();
        });

        std::thread credentialUpdater([&]() {
            TokenStorage storage;
            readyToMutate.count_down();
            startMutating.wait();
            storage.saveCredentials(updatedClientId, updatedClientSecret);
        });

        readyToMutate.wait();
        startMutating.count_down();
        tokenDeleter.join();
        credentialUpdater.join();

        root = readFallbackObject();
        QVERIFY(!root.contains(QStringLiteral("accessToken")));
        QVERIFY(!root.contains(QStringLiteral("refreshToken")));
        QCOMPARE(root.value(QStringLiteral("clientId")).toString(), updatedClientId);
        QCOMPARE(root.value(QStringLiteral("clientSecret")).toString(), updatedClientSecret);
    }
}

QTEST_MAIN(TestTokenStorage)
#include "TestTokenStorage.moc"