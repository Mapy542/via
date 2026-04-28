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
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

bool isFallbackForcedByEnvironment() {
    const QByteArray value = qgetenv("VIA_FORCE_TOKEN_FALLBACK").trimmed().toLower();
    return !value.isEmpty() && value != "0" && value != "false";
}

}  // namespace

// ── Static constants ────────────────────────────────────────────────────────

const QString TokenStorage::KEYCHAIN_SERVICE = QStringLiteral("Via");
const QString TokenStorage::ACCESS_TOKEN_KEY = QStringLiteral("accessToken");
const QString TokenStorage::REFRESH_TOKEN_KEY = QStringLiteral("refreshToken");
const QString TokenStorage::EXPIRY_KEY = QStringLiteral("auth/tokenExpiry");
const QString TokenStorage::CLIENT_ID_KEY = QStringLiteral("clientId");
const QString TokenStorage::CLIENT_SECRET_KEY = QStringLiteral("clientSecret");

// ── Construction / destruction ──────────────────────────────────────────────

TokenStorage::TokenStorage(QObject* parent)
    : QObject(parent),
      m_keychainAvailable(QKeychain::isAvailable() && !isFallbackForcedByEnvironment()) {
    m_settings.setFallbacksEnabled(false);

    if (isFallbackForcedByEnvironment()) {
        qInfo() << "TokenStorage: forcing file-based fallback backend via VIA_FORCE_TOKEN_FALLBACK";
    }

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

QString TokenStorage::fallbackLockFilePath() const {
    return fallbackFilePath() + QStringLiteral(".lock");
}

bool TokenStorage::loadFallbackObject(const QString& path, QJsonObject& root) const {
    QFile file(path);
    if (!file.exists()) {
        root = QJsonObject();
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open fallback token file for reading:" << path;
        root = QJsonObject();
        return false;
    }

    const QByteArray payload = file.readAll();
    file.close();

    if (payload.trimmed().isEmpty()) {
        root = QJsonObject();
        return true;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "Fallback token file is malformed, rebuilding from an empty object:" << path
                   << parseError.errorString();
        root = QJsonObject();
        return true;
    }

    root = document.object();
    return true;
}

bool TokenStorage::writeFallbackObjectAtomically(const QString& path,
                                                 const QJsonObject& root) const {
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const QFileInfo fileInfo(path);
    QDir directory(fileInfo.path());

    if (!directory.exists() && !QDir().mkpath(directory.path())) {
        qWarning() << "Failed to create fallback token directory:" << directory.path();
        return false;
    }

#ifdef Q_OS_UNIX
    QByteArray tempTemplate =
        QFile::encodeName(directory.filePath(fileInfo.fileName() + QStringLiteral(".XXXXXX")));
    tempTemplate.append('\0');

    const int fileDescriptor = ::mkstemp(tempTemplate.data());
    if (fileDescriptor == -1) {
        qWarning() << "Failed to create fallback token temp file:" << path << strerror(errno);
        return false;
    }

    const QString tempPath = QFile::decodeName(tempTemplate.constData());
    bool ok = true;
    qsizetype offset = 0;

    if (::fchmod(fileDescriptor, S_IRUSR | S_IWUSR) != 0) {
        qWarning() << "Failed to secure fallback token temp file permissions:" << tempPath
                   << strerror(errno);
        ok = false;
    }

    while (ok && offset < payload.size()) {
        const ssize_t written =
            ::write(fileDescriptor, payload.constData() + offset, payload.size() - offset);
        if (written == -1) {
            qWarning() << "Failed to write fallback token temp file:" << tempPath
                       << strerror(errno);
            ok = false;
            break;
        }
        offset += written;
    }

    if (ok && ::fsync(fileDescriptor) != 0) {
        qWarning() << "Failed to sync fallback token temp file:" << tempPath << strerror(errno);
        ok = false;
    }

    if (::close(fileDescriptor) != 0) {
        qWarning() << "Failed to close fallback token temp file:" << tempPath << strerror(errno);
        ok = false;
    }

    if (!ok) {
        QFile::remove(tempPath);
        return false;
    }

    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(path).constData()) !=
        0) {
        qWarning() << "Failed to replace fallback token file atomically:" << path
                   << strerror(errno);
        QFile::remove(tempPath);
        return false;
    }

    return true;
#else
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open fallback token file for atomic write:" << path;
        return false;
    }

    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(payload) != payload.size()) {
        qWarning() << "Failed to write fallback token file:" << path;
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        qWarning() << "Failed to commit fallback token file:" << path;
        return false;
    }

    return true;
#endif
}

bool TokenStorage::mutateFallbackFile(const QString& key, const QString* value) const {
    const QString path = fallbackFilePath();
    const QFileInfo fileInfo(path);

    if (!QDir().mkpath(fileInfo.path())) {
        qWarning() << "Failed to create fallback token directory:" << fileInfo.path();
        return false;
    }

    // Serialize fallback mutations across instances and processes.
    QLockFile lock(fallbackLockFilePath());
    if (!lock.tryLock(5000)) {
        qWarning() << "Failed to acquire fallback token file lock:" << fallbackLockFilePath();
        return false;
    }

    QJsonObject root;
    if (!loadFallbackObject(path, root)) {
        return false;
    }

    if (value != nullptr) {
        root.insert(key, *value);
    } else {
        if (!root.contains(key)) {
            return true;
        }
        root.remove(key);
    }

    if (root.isEmpty()) {
        if (!QFile::exists(path)) {
            return true;
        }
        if (!QFile::remove(path)) {
            qWarning() << "Failed to remove empty fallback token file:" << path;
            return false;
        }
        return true;
    }

    return writeFallbackObjectAtomically(path, root);
}

bool TokenStorage::writeToFallbackFile(const QString& key, const QString& value) const {
    return mutateFallbackFile(key, &value);
}

QString TokenStorage::readFromFallbackFile(const QString& key) const {
    QJsonObject root;
    if (!loadFallbackObject(fallbackFilePath(), root)) {
        return QString();
    }
    return root.value(key).toString();
}

void TokenStorage::deleteFromFallbackFile(const QString& key) const {
    mutateFallbackFile(key, nullptr);
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
