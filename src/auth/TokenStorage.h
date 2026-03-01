/**
 * @file TokenStorage.h
 * @brief Secure storage for OAuth tokens
 *
 * Handles secure storage of OAuth access and refresh tokens.
 * Uses QSettings with optional encryption for security.
 */

#ifndef TOKENSTORAGE_H
#define TOKENSTORAGE_H

#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QString>

/**
 * @class TokenStorage
 * @brief Secure storage for OAuth tokens
 *
 * Provides secure storage for OAuth tokens using QSettings.
 * Tokens are stored with basic obfuscation to prevent casual
 * reading. For production use, system keyring integration
 * (e.g., libsecret on Linux) is recommended.
 */
class TokenStorage : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the token storage
     * @param parent Parent object
     */
    explicit TokenStorage(QObject* parent = nullptr);

    ~TokenStorage() override;

    /**
     * @brief Save OAuth tokens
     * @param accessToken Access token
     * @param refreshToken Refresh token
     * @param expiry Token expiry time
     */
    void saveTokens(const QString& accessToken, const QString& refreshToken,
                    const QDateTime& expiry);

    /**
     * @brief Get the stored access token
     * @return Access token or empty string if not stored
     */
    QString getAccessToken() const;

    /**
     * @brief Get the stored refresh token
     * @return Refresh token or empty string if not stored
     */
    QString getRefreshToken() const;

    /**
     * @brief Get the token expiry time
     * @return Token expiry time or invalid QDateTime if not stored
     */
    QDateTime getTokenExpiry() const;

    /**
     * @brief Check if valid tokens are stored
     * @return true if tokens are stored and refresh token is available
     */
    bool hasValidTokens() const;

    /**
     * @brief Check if the access token is expired
     * @return true if the access token has expired
     */
    bool isTokenExpired() const;

    /**
     * @brief Clear all stored tokens
     */
    void clearTokens();

    /**
     * @brief Save OAuth client credentials
     * @param clientId OAuth client ID
     * @param clientSecret OAuth client secret
     */
    void saveCredentials(const QString& clientId, const QString& clientSecret);

    /**
     * @brief Get stored client ID
     * @return Client ID or empty string
     */
    QString getClientId() const;

    /**
     * @brief Get stored client secret
     * @return Client secret or empty string
     */
    QString getClientSecret() const;

   signals:
    /**
     * @brief Emitted when tokens are saved
     */
    void tokensSaved();

    /**
     * @brief Emitted when tokens are cleared
     */
    void tokensCleared();

   private:
    /**
     * @brief Encode a string for storage (basic obfuscation)
     * @param input Plain text
     * @return Encoded string
     */
    QString encode(const QString& input) const;

    /**
     * @brief Decode a stored string
     * @param input Encoded string
     * @return Plain text
     */
    QString decode(const QString& input) const;

    QSettings m_settings;

    // Settings keys
    static const QString ACCESS_TOKEN_KEY;
    static const QString REFRESH_TOKEN_KEY;
    static const QString EXPIRY_KEY;
    static const QString CLIENT_ID_KEY;
    static const QString CLIENT_SECRET_KEY;
};

#endif  // TOKENSTORAGE_H
