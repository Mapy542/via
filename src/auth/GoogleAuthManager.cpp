/**
 * @file GoogleAuthManager.cpp
 * @brief Implementation of Google OAuth 2.0 authentication
 */

#include "GoogleAuthManager.h"

#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSet>
#include <QUrlQuery>
#include <QtGlobal>
#include <climits>

#include "TokenStorage.h"

// Google OAuth endpoints
const QString GoogleAuthManager::AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
const QString GoogleAuthManager::TOKEN_URL = "https://oauth2.googleapis.com/token";
const QString GoogleAuthManager::SCOPE = "https://www.googleapis.com/auth/drive";
const quint16 GoogleAuthManager::REDIRECT_PORT = 8080;

GoogleAuthManager::GoogleAuthManager(TokenStorage* tokenStorage, QObject* parent)
    : QObject(parent),
      m_tokenStorage(tokenStorage),
      m_oauth(nullptr),
      m_replyHandler(nullptr),
      m_networkManager(new QNetworkAccessManager(this)),
      m_refreshTimer(new QTimer(this)),
      m_forceConsentPrompt(false),
      m_authenticated(false) {
    // Load stored credentials or use empty values
    // Users must configure credentials in Settings or via setCredentials()
    if (tokenStorage) {
        m_clientId = tokenStorage->getClientId();
        m_clientSecret = tokenStorage->getClientSecret();
    }

    // Log warning if credentials are not configured
    if (m_clientId.isEmpty() || m_clientSecret.isEmpty()) {
        qWarning() << "OAuth credentials not configured. "
                   << "Please set your Google OAuth client ID and secret in Settings "
                   << "or call setCredentials() before attempting authentication.";
    }

    setupOAuth();
    loadStoredTokens();

    connect(m_refreshTimer, &QTimer::timeout, this, &GoogleAuthManager::onTokenRefreshTimerExpired);
}

GoogleAuthManager::~GoogleAuthManager() {
    m_refreshTimer->stop();
    // Don't call logout() on destruction - this was clearing tokens on every app close
    // Tokens should persist so the user stays logged in between sessions
}

void GoogleAuthManager::setupOAuth() {
    // Create OAuth flow
    m_oauth = new QOAuth2AuthorizationCodeFlow(this);

    // Setup reply handler for local redirect
    m_replyHandler = new QOAuthHttpServerReplyHandler(REDIRECT_PORT, this);
    m_oauth->setReplyHandler(m_replyHandler);

    // Set OAuth parameters
    m_oauth->setAuthorizationUrl(QUrl(AUTH_URL));
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    m_oauth->setTokenUrl(QUrl(TOKEN_URL));
#else
    m_oauth->setAccessTokenUrl(QUrl(TOKEN_URL));
#endif

    // Set scope - use the scope setter which accepts a space-separated string
    m_oauth->setScope(SCOPE);

    // Set credentials if available
    if (!m_clientId.isEmpty()) {
        m_oauth->setClientIdentifier(m_clientId);
        m_oauth->setClientIdentifierSharedKey(m_clientSecret);
    }

    // Additional OAuth parameters
    m_oauth->setModifyParametersFunction([this](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant>* parameters) {
        if (stage == QAbstractOAuth::Stage::RequestingAuthorization) {
            parameters->insert("access_type", "offline");
            if (m_forceConsentPrompt) {
                parameters->insert("prompt", "consent");
            }
        }
    });

    // Connect signals
    connect(m_oauth, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, this,
            [](const QUrl& url) { QDesktopServices::openUrl(url); });

    connect(m_oauth, &QOAuth2AuthorizationCodeFlow::granted, this, &GoogleAuthManager::onAuthorizationGranted);

    connect(m_oauth, &QOAuth2AuthorizationCodeFlow::tokenChanged, this, &GoogleAuthManager::onTokenChanged);

    connect(m_oauth, &QOAuth2AuthorizationCodeFlow::refreshTokenChanged, this,
            &GoogleAuthManager::onRefreshTokenChanged);

#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    connect(m_oauth, &QAbstractOAuth2::serverReportedErrorOccurred, this,
            [this](const QString& error, const QString& errorDescription, const QUrl&) {
                onError(error, errorDescription, QUrl());
            });
#else
    connect(m_oauth, &QAbstractOAuth2::error, this,
            [this](const QString& error, const QString& errorDescription, const QUrl&) {
                onError(error, errorDescription, QUrl());
            });
#endif
}

void GoogleAuthManager::loadStoredTokens() {
    if (m_tokenStorage && m_tokenStorage->hasValidTokens()) {
        m_accessToken = m_tokenStorage->getAccessToken();
        m_refreshTokenValue = m_tokenStorage->getRefreshToken();
        m_accessTokenExpiry = m_tokenStorage->getTokenExpiry();

        if (!m_accessToken.isEmpty()) {
            m_oauth->setToken(m_accessToken);
        }
        if (!m_refreshTokenValue.isEmpty()) {
            m_oauth->setRefreshToken(m_refreshTokenValue);
        }

        // Check if token is expired
        if (m_tokenStorage->isTokenExpired()) {
            m_authenticated = false;
            // Don't refresh here - let main.cpp handle it after connections are made
            qInfo() << "Stored tokens are expired, will refresh when requested";
        } else {
            m_authenticated = true;
            scheduleTokenRefresh();
            qInfo() << "Loaded valid tokens from storage";
            // Don't emit authenticated() here - main.cpp will check isAuthenticated()
            // and emit or start sync engine appropriately after connections are established
        }
    }
}

void GoogleAuthManager::saveTokens() {
    if (m_tokenStorage) {
        QDateTime expiry = m_accessTokenExpiry;
        if (!expiry.isValid()) {
            expiry = QDateTime::currentDateTimeUtc().addSecs(3600);
        }
        m_tokenStorage->saveTokens(m_accessToken, m_refreshTokenValue, expiry);
    }
}

bool GoogleAuthManager::isAuthenticated() const { return m_authenticated && !m_accessToken.isEmpty(); }

QString GoogleAuthManager::accessToken() const { return m_accessToken; }

QString GoogleAuthManager::refreshToken() const { return m_refreshTokenValue; }

QDateTime GoogleAuthManager::tokenExpiry() const { return m_accessTokenExpiry; }

bool GoogleAuthManager::isTokenExpiringSoon(int bufferSecs) const {
    if (!m_authenticated || m_accessToken.isEmpty()) {
        return true;  // not authenticated at all
    }
    if (!m_accessTokenExpiry.isValid()) {
        return false;  // unknown expiry — assume valid
    }
    return QDateTime::currentDateTimeUtc().secsTo(m_accessTokenExpiry) <= bufferSecs;
}

bool GoogleAuthManager::ensureValidToken(int timeoutMs) {
    // Already valid and not expiring soon — nothing to do
    if (!isTokenExpiringSoon()) {
        return true;
    }

    // Cannot refresh without a refresh token
    if (m_refreshTokenValue.isEmpty()) {
        return false;
    }

    // If a refresh is already in flight (e.g. the timer fired on resume from
    // suspend at the same time an API call invoked us), just wait for the
    // result instead of issuing a second concurrent refresh.
    const bool needToInitiate = !m_refreshInFlight;

    qInfo() << "ensureValidToken: token expiring soon"
            << (needToInitiate ? "— triggering synchronous refresh" : "— waiting on in-flight refresh");

    QEventLoop loop;
    bool success = false;

    auto c1 = connect(this, &GoogleAuthManager::tokenRefreshed, &loop, [&]() {
        success = true;
        loop.quit();
    });
    auto c2 = connect(this, &GoogleAuthManager::tokenRefreshError, &loop, [&]() {
        success = false;
        loop.quit();
    });
    auto c3 = connect(this, &GoogleAuthManager::authExpired, &loop, [&]() {
        success = false;
        loop.quit();
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);

    if (needToInitiate) {
        refreshTokens();
    }
    loop.exec();

    disconnect(c1);
    disconnect(c2);
    disconnect(c3);

    if (success) {
        qInfo() << "ensureValidToken: refresh succeeded";
    } else {
        qWarning() << "ensureValidToken: refresh failed or timed out";
    }
    return success;
}

void GoogleAuthManager::authenticate() {
    if (m_clientId.isEmpty() || m_clientSecret.isEmpty()) {
        emit authenticationError(
            "OAuth credentials not configured. "
            "Please set your Google OAuth client ID and secret in Settings.");
        return;
    }

    m_forceConsentPrompt = m_refreshTokenValue.isEmpty();
    qInfo() << "Starting OAuth authentication flow";
    m_oauth->grant();
}

void GoogleAuthManager::refreshTokens() {
    if (m_refreshTokenValue.isEmpty()) {
        m_authenticated = false;
        emit tokenRefreshError("No refresh token available");
        emit authExpired("No refresh token available. Please sign in again.");
        return;
    }

    if (m_clientId.isEmpty() || m_clientSecret.isEmpty()) {
        emit tokenRefreshError("OAuth credentials not configured");
        return;
    }

    if (m_refreshInFlight) {
        qInfo() << "Token refresh already in flight, skipping duplicate request";
        return;
    }
    m_refreshInFlight = true;

    qInfo() << "Refreshing access token";

    // Manual token refresh using QNetworkAccessManager
    QUrl tokenUrl(TOKEN_URL);
    QNetworkRequest request(tokenUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QUrlQuery query;
    query.addQueryItem("client_id", m_clientId);
    query.addQueryItem("client_secret", m_clientSecret);
    query.addQueryItem("refresh_token", m_refreshTokenValue);
    query.addQueryItem("grant_type", "refresh_token");

    QNetworkReply* reply = m_networkManager->post(request, query.toString(QUrl::FullyEncoded).toUtf8());

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            m_refreshInFlight = false;
            qWarning() << "Token refresh failed:" << reply->errorString();
            emit tokenRefreshError(reply->errorString());
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        if (obj.contains("access_token")) {
            m_refreshInFlight = false;
            const bool wasAuthenticated = m_authenticated;
            m_accessToken = obj["access_token"].toString();
            m_oauth->setToken(m_accessToken);

            int expiresIn = obj["expires_in"].toInt(3600);
            if (expiresIn <= 0) {
                expiresIn = 3600;
            }
            m_accessTokenExpiry = QDateTime::currentDateTimeUtc().addSecs(expiresIn);

            // Refresh token might also be returned
            if (obj.contains("refresh_token")) {
                const QString refreshedToken = obj["refresh_token"].toString();
                if (!refreshedToken.isEmpty()) {
                    m_refreshTokenValue = refreshedToken;
                    m_oauth->setRefreshToken(m_refreshTokenValue);
                }
            }

            saveTokens();
            m_authenticated = true;
            scheduleTokenRefresh();

            qInfo() << "Access token refreshed successfully";
            emit tokenRefreshed();
            if (!wasAuthenticated) {
                emit authenticated();
            }
        } else {
            const QString errorCode = obj["error"].toString();
            QString error = obj["error_description"].toString();
            if (error.isEmpty()) {
                error = errorCode;
            }
            if (error.isEmpty()) {
                error = "Unknown error";
            }
            m_refreshInFlight = false;
            qWarning() << "Token refresh error:" << error;

            if (errorCode == "invalid_grant" || errorCode == "invalid_client" || errorCode == "unauthorized_client") {
                m_accessToken.clear();
                m_refreshTokenValue.clear();
                m_accessTokenExpiry = QDateTime();
                m_authenticated = false;
                m_forceConsentPrompt = true;

                if (m_oauth) {
                    m_oauth->setToken(QString());
                    m_oauth->setRefreshToken(QString());
                }

                if (m_tokenStorage) {
                    m_tokenStorage->clearTokens();
                }

                emit authExpired(error);
            }

            emit tokenRefreshError(error);
        }
    });
}

void GoogleAuthManager::logout() {
    qInfo() << "Logging out";

    m_refreshTimer->stop();
    m_accessToken.clear();
    m_refreshTokenValue.clear();
    m_accessTokenExpiry = QDateTime();
    m_authenticated = false;
    m_forceConsentPrompt = false;

    if (m_oauth) {
        m_oauth->setToken(QString());
        m_oauth->setRefreshToken(QString());
    }

    if (m_tokenStorage) {
        m_tokenStorage->clearTokens();
    }

    emit loggedOut();
}

void GoogleAuthManager::setCredentials(const QString& clientId, const QString& clientSecret) {
    m_clientId = clientId;
    m_clientSecret = clientSecret;

    if (m_oauth) {
        m_oauth->setClientIdentifier(clientId);
        m_oauth->setClientIdentifierSharedKey(clientSecret);
    }
}

void GoogleAuthManager::onAuthorizationGranted() {
    qInfo() << "Authorization granted";

    m_accessToken = m_oauth->token();
    m_refreshTokenValue = m_oauth->refreshToken();
    m_accessTokenExpiry = QDateTime::currentDateTimeUtc().addSecs(3600);
    m_authenticated = true;
    m_forceConsentPrompt = false;

    saveTokens();
    scheduleTokenRefresh();

    emit authenticated();
}

void GoogleAuthManager::onTokenChanged(const QString& token) {
    m_accessToken = token;
    qDebug() << "Access token updated";
}

void GoogleAuthManager::onRefreshTokenChanged(const QString& token) {
    m_refreshTokenValue = token;
    qDebug() << "Refresh token updated";
}

void GoogleAuthManager::onError(const QString& error, const QString& errorDescription, const QUrl& uri) {
    Q_UNUSED(uri)

    QString message = error;
    if (!errorDescription.isEmpty()) {
        message += ": " + errorDescription;
    }

    qWarning() << "OAuth error:" << message;
    emit authenticationError(message);
}

void GoogleAuthManager::onTokenRefreshTimerExpired() { refreshTokens(); }

void GoogleAuthManager::scheduleTokenRefresh() {
    int refreshInterval = 50 * 60 * 1000;
    if (m_accessTokenExpiry.isValid()) {
        const qint64 nowToExpiryMs = QDateTime::currentDateTimeUtc().msecsTo(m_accessTokenExpiry);
        const qint64 buffered = nowToExpiryMs - (2 * 60 * 1000);
        if (buffered <= 0) {
            refreshInterval = 30 * 1000;
        } else {
            refreshInterval = static_cast<int>(qMin<qint64>(buffered, INT_MAX));
        }
    }

    m_refreshTimer->stop();
    m_refreshTimer->start(refreshInterval);

    qDebug() << "Scheduled token refresh in" << (refreshInterval / 1000) << "seconds";
}
