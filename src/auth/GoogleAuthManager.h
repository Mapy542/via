/**
 * @file GoogleAuthManager.h
 * @brief OAuth 2.0 authentication manager for Google Drive API
 *
 * Handles Google OAuth 2.0 authentication flow including:
 * - Initial authentication
 * - Token refresh
 * - Token storage and retrieval
 */

#ifndef GOOGLEAUTHMANAGER_H
#define GOOGLEAUTHMANAGER_H

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QObject>
#include <QTimer>
#include <QUrl>

class TokenStorage;

/**
 * @class GoogleAuthManager
 * @brief Manages Google OAuth 2.0 authentication
 *
 * Implements OAuth 2.0 authorization code flow for Google Drive API access.
 * Handles token refresh and secure storage.
 */
class GoogleAuthManager : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the auth manager
     * @param tokenStorage Pointer to token storage
     * @param parent Parent object
     */
    explicit GoogleAuthManager(TokenStorage* tokenStorage, QObject* parent = nullptr);

    ~GoogleAuthManager() override;

    /**
     * @brief Check if the user is authenticated
     * @return true if authenticated with valid tokens
     */
    bool isAuthenticated() const;

    /**
     * @brief Get the current access token
     * @return Access token or empty string if not authenticated
     */
    QString accessToken() const;

    /**
     * @brief Get the refresh token
     * @return Refresh token or empty string if not authenticated
     */
    QString refreshToken() const;

   public slots:
    /**
     * @brief Start the OAuth authentication flow
     *
     * Opens a browser window for user to authenticate with Google.
     */
    void authenticate();

    /**
     * @brief Refresh the access token using the refresh token
     *
     * Called automatically when the access token expires.
     */
    void refreshTokens();

    /**
     * @brief Log out the user
     *
     * Clears stored tokens and resets authentication state.
     */
    void logout();

    /**
     * @brief Set custom OAuth credentials
     * @param clientId OAuth client ID
     * @param clientSecret OAuth client secret
     */
    void setCredentials(const QString& clientId, const QString& clientSecret);

   signals:
    /**
     * @brief Emitted when authentication is successful
     */
    void authenticated();

    /**
     * @brief Emitted when user logs out
     */
    void loggedOut();

    /**
     * @brief Emitted when authentication fails
     * @param error Error message
     */
    void authenticationError(const QString& error);

    /**
     * @brief Emitted when token refresh is successful
     */
    void tokenRefreshed();

    /**
     * @brief Emitted when token refresh fails
     * @param error Error message
     */
    void tokenRefreshError(const QString& error);

    /**
     * @brief Emitted when authentication can no longer continue without user re-auth
     * @param reason Failure reason
     */
    void authExpired(const QString& reason);

   private slots:
    void onAuthorizationGranted();
    void onTokenChanged(const QString& token);
    void onRefreshTokenChanged(const QString& token);
    void onError(const QString& error, const QString& errorDescription, const QUrl& uri);
    void onTokenRefreshTimerExpired();

   private:
    void setupOAuth();
    void setupRefreshTimer();
    void loadStoredTokens();
    void saveTokens();
    void scheduleTokenRefresh();

    TokenStorage* m_tokenStorage;
    QOAuth2AuthorizationCodeFlow* m_oauth;
    QOAuthHttpServerReplyHandler* m_replyHandler;
    QNetworkAccessManager* m_networkManager;
    QTimer* m_refreshTimer;

    QString m_clientId;
    QString m_clientSecret;
    QString m_accessToken;
    QString m_refreshTokenValue;
    QDateTime m_accessTokenExpiry;
    bool m_forceConsentPrompt;

    bool m_authenticated;

    // Google OAuth endpoints
    static const QString AUTH_URL;
    static const QString TOKEN_URL;
    static const QString SCOPE;
    static const quint16 REDIRECT_PORT;
};

#endif  // GOOGLEAUTHMANAGER_H
