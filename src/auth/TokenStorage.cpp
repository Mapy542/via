/**
 * @file TokenStorage.cpp
 * @brief Implementation of secure token storage
 */

#include "TokenStorage.h"

#include <QCryptographicHash>
#include <QDebug>

// Settings keys
const QString TokenStorage::ACCESS_TOKEN_KEY = "auth/accessToken";
const QString TokenStorage::REFRESH_TOKEN_KEY = "auth/refreshToken";
const QString TokenStorage::EXPIRY_KEY = "auth/tokenExpiry";
const QString TokenStorage::CLIENT_ID_KEY = "auth/clientId";
const QString TokenStorage::CLIENT_SECRET_KEY = "auth/clientSecret";

TokenStorage::TokenStorage(QObject* parent) : QObject(parent) {
    // Configure settings
    m_settings.setFallbacksEnabled(false);
}

TokenStorage::~TokenStorage() = default;

void TokenStorage::saveTokens(const QString& accessToken, const QString& refreshToken,
                              const QDateTime& expiry) {
    m_settings.setValue(ACCESS_TOKEN_KEY, encode(accessToken));
    m_settings.setValue(REFRESH_TOKEN_KEY, encode(refreshToken));
    m_settings.setValue(EXPIRY_KEY, expiry.toString(Qt::ISODate));
    m_settings.sync();

    qDebug() << "Tokens saved, expiry:" << expiry.toString(Qt::ISODate);
    emit tokensSaved();
}

QString TokenStorage::getAccessToken() const {
    QString encoded = m_settings.value(ACCESS_TOKEN_KEY).toString();
    return decode(encoded);
}

QString TokenStorage::getRefreshToken() const {
    QString encoded = m_settings.value(REFRESH_TOKEN_KEY).toString();
    return decode(encoded);
}

QDateTime TokenStorage::getTokenExpiry() const {
    QString expiryStr = m_settings.value(EXPIRY_KEY).toString();
    return QDateTime::fromString(expiryStr, Qt::ISODate);
}

bool TokenStorage::hasValidTokens() const {
    // We need at least a refresh token to be able to authenticate
    return !getRefreshToken().isEmpty();
}

bool TokenStorage::isTokenExpired() const {
    QDateTime expiry = getTokenExpiry();
    if (!expiry.isValid()) {
        return true;
    }

    // Consider token expired 1 minute before actual expiry
    return expiry.addSecs(-60) <= QDateTime::currentDateTime();
}

void TokenStorage::clearTokens() {
    m_settings.remove(ACCESS_TOKEN_KEY);
    m_settings.remove(REFRESH_TOKEN_KEY);
    m_settings.remove(EXPIRY_KEY);
    m_settings.sync();

    qDebug() << "Tokens cleared";
    emit tokensCleared();
}

void TokenStorage::saveCredentials(const QString& clientId, const QString& clientSecret) {
    m_settings.setValue(CLIENT_ID_KEY, encode(clientId));
    m_settings.setValue(CLIENT_SECRET_KEY, encode(clientSecret));
    m_settings.sync();

    qDebug() << "OAuth credentials saved";
}

QString TokenStorage::getClientId() const {
    QString encoded = m_settings.value(CLIENT_ID_KEY).toString();
    return decode(encoded);
}

QString TokenStorage::getClientSecret() const {
    QString encoded = m_settings.value(CLIENT_SECRET_KEY).toString();
    return decode(encoded);
}

QString TokenStorage::encode(const QString& input) const {
    if (input.isEmpty()) {
        return QString();
    }

    // Simple XOR-based obfuscation
    // Note: This is NOT cryptographically secure! For production,
    // use system keyring (libsecret on Linux) or proper encryption.

    const char key[] = "Via2024";
    const int keyLen = sizeof(key) - 1;

    QByteArray data = input.toUtf8();
    QByteArray result;
    result.reserve(data.size());

    for (int i = 0; i < data.size(); ++i) {
        result.append(data.at(i) ^ key[i % keyLen]);
    }

    return QString::fromLatin1(result.toBase64());
}

QString TokenStorage::decode(const QString& input) const {
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
