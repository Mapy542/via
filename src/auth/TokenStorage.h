/**
 * @file TokenStorage.h
 * @brief Secure storage for OAuth tokens
 *
 * Handles secure storage of OAuth access and refresh tokens.
 * Uses QtKeychain (OS-backed secure storage) as the primary backend,
 * with a file-based fallback (0600-permissioned JSON) when no keyring
 * is available. Migrates legacy XOR-obfuscated QSettings values on
 * first use.
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
 * Provides secure storage for OAuth tokens using the OS keyring
 * (via QtKeychain / libsecret) with an encrypted-file fallback.
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
    // ── Keychain helpers ──────────────────────────────────────────────
    bool writeToKeychain(const QString& key, const QString& value) const;
    QString readFromKeychain(const QString& key) const;
    void deleteFromKeychain(const QString& key) const;

    // ── File-based fallback (0600-permissioned JSON) ──────────────────
    bool writeToFallbackFile(const QString& key, const QString& value) const;
    QString readFromFallbackFile(const QString& key) const;
    void deleteFromFallbackFile(const QString& key) const;
    QString fallbackFilePath() const;

    // ── Combined read/write using best available backend ─────────────
    void secureWrite(const QString& key, const QString& value) const;
    QString secureRead(const QString& key) const;
    void secureDelete(const QString& key) const;

    // ── Legacy migration ─────────────────────────────────────────────
    void migrateFromLegacySettings();
    QString legacyDecode(const QString& input) const;

    QSettings m_settings;      ///< Still used for non-secret settings (expiry)
    bool m_keychainAvailable;  ///< Cached result of QKeychain::isAvailable()

    // Service name for keychain entries
    static const QString KEYCHAIN_SERVICE;

    // Key names (shared between keychain and fallback)
    static const QString ACCESS_TOKEN_KEY;
    static const QString REFRESH_TOKEN_KEY;
    static const QString EXPIRY_KEY;
    static const QString CLIENT_ID_KEY;
    static const QString CLIENT_SECRET_KEY;
};

#endif  // TOKENSTORAGE_H
