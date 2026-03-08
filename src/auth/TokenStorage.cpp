/**
 * @file TokenStorage.cpp
 * @brief Implementation of secure token storage
 *
 * Primary backend: QtKeychain (OS keyring via libsecret / KWallet).
 * Fallback: 0600-permissioned JSON file in AppDataLocation.
 * Legacy: Migrates old XOR-obfuscated QSettings values on first use.
 */

#include "TokenStorage.h"

#include <qt6keychain/keychain.h>

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

// ── Static constants ────────────────────────────────────────────────────────

const QString TokenStorage::KEYCHAIN_SERVICE = QStringLiteral("Via");
const QString TokenStorage::ACCESS_TOKEN_KEY = QStringLiteral("accessToken");
const QString TokenStorage::REFRESH_TOKEN_KEY = QStringLiteral("refreshToken");
const QString TokenStorage::EXPIRY_KEY = QStringLiteral("auth/tokenExpiry");
const QString TokenStorage::CLIENT_ID_KEY = QStringLiteral("clientId");
const QString TokenStorage::CLIENT_SECRET_KEY = QStringLiteral("clientSecret");

// ── Construction / destruction ──────────────────────────────────────────────

TokenStorage::TokenStorage(QObject* parent)
    : QObject(parent), m_keychainAvailable(QKeychain::isAvailable()) {
    m_settings.setFallbacksEnabled(false);

    if (m_keychainAvailable) {
        qInfo() << "TokenStorage: OS keyring available, using secure storage";
    } else {
        qWarning() << "TokenStorage: OS keyring NOT available, using file-based fallback";
    }

    // Migrate legacy XOR-obfuscated values from QSettings (one-time)
    migrateFromLegacySettings();
}

TokenStorage::~TokenStorage() = default;

// ── Public API ──────────────────────────────────────────────────────────────

void TokenStorage::saveTokens(const QString& accessToken, const QString& refreshToken,
                              const QDateTime& expiry) {
    secureWrite(ACCESS_TOKEN_KEY, accessToken);
    secureWrite(REFRESH_TOKEN_KEY, refreshToken);
    // Expiry is not secret — keep in plain QSettings for easy reading
    m_settings.setValue(EXPIRY_KEY, expiry.toString(Qt::ISODate));
    m_settings.sync();

    qDebug() << "Tokens saved, expiry:" << expiry.toString(Qt::ISODate);
    emit tokensSaved();
}

QString TokenStorage::getAccessToken() const { return secureRead(ACCESS_TOKEN_KEY); }

QString TokenStorage::getRefreshToken() const { return secureRead(REFRESH_TOKEN_KEY); }

QDateTime TokenStorage::getTokenExpiry() const {
    QString expiryStr = m_settings.value(EXPIRY_KEY).toString();
    return QDateTime::fromString(expiryStr, Qt::ISODate);
}

bool TokenStorage::hasValidTokens() const { return !getRefreshToken().isEmpty(); }

bool TokenStorage::isTokenExpired() const {
    QDateTime expiry = getTokenExpiry();
    if (!expiry.isValid()) {
        return true;
    }

    // GoogleAuthManager stores expiry in UTC, so compare against UTC (DAT-02 fix)
    return expiry.addSecs(-60) <= QDateTime::currentDateTimeUtc();
}

void TokenStorage::clearTokens() {
    secureDelete(ACCESS_TOKEN_KEY);
    secureDelete(REFRESH_TOKEN_KEY);
    m_settings.remove(EXPIRY_KEY);
    m_settings.sync();

    qDebug() << "Tokens cleared";
    emit tokensCleared();
}

void TokenStorage::saveCredentials(const QString& clientId, const QString& clientSecret) {
    secureWrite(CLIENT_ID_KEY, clientId);
    secureWrite(CLIENT_SECRET_KEY, clientSecret);

    qDebug() << "OAuth credentials saved";
}

QString TokenStorage::getClientId() const { return secureRead(CLIENT_ID_KEY); }

QString TokenStorage::getClientSecret() const { return secureRead(CLIENT_SECRET_KEY); }

// ── Keychain helpers (synchronous wrappers around QtKeychain) ───────────────

bool TokenStorage::writeToKeychain(const QString& key, const QString& value) const {
    QKeychain::WritePasswordJob job(KEYCHAIN_SERVICE);
    job.setAutoDelete(false);
    job.setKey(key);
    job.setTextData(value);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError) {
        qWarning() << "Keychain write failed for" << key << ":" << job.errorString();
        return false;
    }
    return true;
}

QString TokenStorage::readFromKeychain(const QString& key) const {
    QKeychain::ReadPasswordJob job(KEYCHAIN_SERVICE);
    job.setAutoDelete(false);
    job.setKey(key);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() == QKeychain::EntryNotFound) {
        return QString();
    }

    if (job.error() != QKeychain::NoError) {
        qWarning() << "Keychain read failed for" << key << ":" << job.errorString();
        return QString();
    }

    return job.textData();
}

void TokenStorage::deleteFromKeychain(const QString& key) const {
    QKeychain::DeletePasswordJob job(KEYCHAIN_SERVICE);
    job.setAutoDelete(false);
    job.setKey(key);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        qWarning() << "Keychain delete failed for" << key << ":" << job.errorString();
    }
}

// ── File-based fallback (0600-permissioned JSON) ────────────────────────────

QString TokenStorage::fallbackFilePath() const {
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dataPath + QStringLiteral("/secure_tokens.json");
}

bool TokenStorage::writeToFallbackFile(const QString& key, const QString& value) const {
    QString path = fallbackFilePath();

    // Read existing JSON
    QJsonObject root;
    {
        QFile file(path);
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
        }
    }

    root[key] = value;

    // Ensure directory exists
    QDir().mkpath(QFileInfo(path).path());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Failed to open fallback token file for writing:" << path;
        return false;
    }

    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

QString TokenStorage::readFromFallbackFile(const QString& key) const {
    QFile file(fallbackFilePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    return root.value(key).toString();
}

void TokenStorage::deleteFromFallbackFile(const QString& key) const {
    QString path = fallbackFilePath();
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    if (!root.contains(key)) {
        return;
    }

    root.remove(key);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
}

// ── Combined read/write using best available backend ────────────────────────

void TokenStorage::secureWrite(const QString& key, const QString& value) const {
    if (value.isEmpty()) {
        secureDelete(key);
        return;
    }

    if (m_keychainAvailable) {
        if (writeToKeychain(key, value)) {
            return;
        }
        // Fall through to file-based fallback on keychain failure
        qWarning() << "Falling back to file-based storage for" << key;
    }

    writeToFallbackFile(key, value);
}

QString TokenStorage::secureRead(const QString& key) const {
    if (m_keychainAvailable) {
        QString val = readFromKeychain(key);
        if (!val.isEmpty()) {
            return val;
        }
    }

    // Try fallback file (also serves migration path)
    return readFromFallbackFile(key);
}

void TokenStorage::secureDelete(const QString& key) const {
    if (m_keychainAvailable) {
        deleteFromKeychain(key);
    }
    deleteFromFallbackFile(key);
}

// ── Legacy migration ────────────────────────────────────────────────────────

void TokenStorage::migrateFromLegacySettings() {
    // Legacy QSettings keys used XOR-obfuscated values stored under "auth/*"
    static const QString legacyAccessKey = QStringLiteral("auth/accessToken");
    static const QString legacyRefreshKey = QStringLiteral("auth/refreshToken");
    static const QString legacyClientIdKey = QStringLiteral("auth/clientId");
    static const QString legacyClientSecretKey = QStringLiteral("auth/clientSecret");

    bool hasLegacy =
        m_settings.contains(legacyAccessKey) || m_settings.contains(legacyRefreshKey) ||
        m_settings.contains(legacyClientIdKey) || m_settings.contains(legacyClientSecretKey);

    if (!hasLegacy) {
        return;
    }

    qInfo() << "TokenStorage: Migrating legacy XOR-obfuscated tokens to secure storage...";

    auto migrateLegacyKey = [this](const QString& legacyKey, const QString& newKey) {
        QString encoded = m_settings.value(legacyKey).toString();
        if (encoded.isEmpty()) {
            return;
        }
        QString decoded = legacyDecode(encoded);
        if (!decoded.isEmpty()) {
            secureWrite(newKey, decoded);
        }
        m_settings.remove(legacyKey);
    };

    migrateLegacyKey(legacyAccessKey, ACCESS_TOKEN_KEY);
    migrateLegacyKey(legacyRefreshKey, REFRESH_TOKEN_KEY);
    migrateLegacyKey(legacyClientIdKey, CLIENT_ID_KEY);
    migrateLegacyKey(legacyClientSecretKey, CLIENT_SECRET_KEY);

    m_settings.sync();
    qInfo() << "TokenStorage: Legacy migration complete";
}

QString TokenStorage::legacyDecode(const QString& input) const {
    if (input.isEmpty()) {
        return QString();
    }

    const char key[] = "Via2024";
    const int keyLen = sizeof(key) - 1;

    QByteArray data = QByteArray::fromBase64(input.toLatin1());
    QByteArray result;
    result.reserve(data.size());

    for (int i = 0; i < data.size(); ++i) {
        result.append(data.at(i) ^ key[i % keyLen]);
    }

    return QString::fromUtf8(result);
}
