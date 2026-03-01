# Authentication System

## Overview

The authentication system (`src/auth/`) handles OAuth 2.0 authentication with Google Drive API. It manages the complete authentication lifecycle including initial sign-in, token refresh, and secure storage.

## Files

| File                    | Purpose                           |
| ----------------------- | --------------------------------- |
| `GoogleAuthManager.h`   | Class declaration and signals     |
| `GoogleAuthManager.cpp` | OAuth flow implementation         |
| `TokenStorage.h`        | Token storage interface           |
| `TokenStorage.cpp`      | QSettings-based token persistence |

## GoogleAuthManager

### Responsibilities

- Initiate OAuth 2.0 authorization code flow
- Exchange authorization code for tokens
- Automatically refresh access tokens before expiry
- Store and retrieve tokens securely

### OAuth Configuration

```cpp
static const QString AUTH_URL = "https://accounts.google.com/o/oauth2/auth";
static const QString TOKEN_URL = "https://oauth2.googleapis.com/token";
static const QString SCOPE = "https://www.googleapis.com/auth/drive";
static const quint16 REDIRECT_PORT = 8080;
```

### Token Lifecycle

1. **Initial Authentication**
    - User clicks "Sign In"
    - Browser opens to Google consent screen
    - User grants permissions
    - Redirect to localhost:8080 with auth code
    - Exchange code for access + refresh tokens
    - Store tokens

2. **Token Refresh**
    - Access tokens expire in 60 minutes
    - Refresh scheduled at 55 minutes
    - Uses refresh token (never expires) to get new access token
    - No user interaction required

3. **App Restart**
    - Load stored refresh token
    - Check if access token expired
    - If expired, refresh automatically
    - If valid, use directly

### Key Methods

```cpp
// Start OAuth flow - opens browser
void authenticate();

// Refresh using stored refresh token
void refreshTokens();

// Check if tokens exist and are usable
bool isAuthenticated() const;

// Get current access token for API calls
QString accessToken() const;

// Set custom OAuth app credentials
void setCredentials(const QString &clientId, const QString &clientSecret);
```

### Signals

| Signal                         | When Emitted                        |
| ------------------------------ | ----------------------------------- |
| `authenticated()`              | OAuth flow completed successfully   |
| `loggedOut()`                  | User signed out                     |
| `authenticationError(QString)` | OAuth flow failed                   |
| `tokenRefreshed()`             | Access token refreshed successfully |
| `tokenRefreshError(QString)`   | Token refresh failed                |

## TokenStorage

### Responsibilities

- Persist tokens to disk securely
- Load tokens on app startup
- Clear tokens on logout

### Storage Location

`~/.config/Via/Via.conf`

### Stored Values

```ini
[Auth]
accessToken=<base64 encoded>
refreshToken=<base64 encoded>
tokenExpiry=2026-01-03T12:00:00Z
clientId=<user provided>
clientSecret=<user provided>
```

### Security Notes

- Tokens are base64 encoded (basic obfuscation)
- Stored in user's config directory
- Only readable by user (0600 permissions)

## Integration with Other Systems

### SyncEngine

```cpp
// In main.cpp
connect(&authManager, &GoogleAuthManager::tokenRefreshed,
        &syncEngine, &SyncEngine::start);
```

### GoogleDriveClient

```cpp
// GoogleDriveClient uses authManager for tokens
GoogleDriveClient client(&authManager, parent);
// Each API call includes: Authorization: Bearer <accessToken>
```

### MainWindow

```cpp
connect(&authManager, &GoogleAuthManager::authenticated,
        &mainWindow, &MainWindow::onAuthenticated);
```

## Error Handling

### Common Errors

1. **Network error during OAuth** - Check internet connection
2. **Invalid client credentials** - Verify client ID/secret in settings
3. **Token refresh failed** - User may need to re-authenticate
4. **Port 8080 in use** - Another app using redirect port

### Error Recovery

- On `tokenRefreshError`, prompt user to sign in again
- On `authenticationError`, show error and allow retry
