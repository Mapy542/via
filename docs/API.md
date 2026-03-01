# API Documentation

This document describes the internal APIs and components of Via.

## Table of Contents

1. [Authentication](#authentication)
2. [Google Drive API Client](#google-drive-api-client)
3. [Synchronization Engine](#synchronization-engine)
4. [FUSE Virtual File System](#fuse-virtual-file-system)
5. [Utilities](#utilities)

---

## Authentication

### GoogleAuthManager

Handles OAuth 2.0 authentication with Google.

#### Public Methods

```cpp
// Check if user is authenticated
bool isAuthenticated() const;

// Get current access token
QString accessToken() const;

// Start OAuth flow
void authenticate();

// Refresh access token
void refreshTokens();

// Log out user
void logout();

// Set custom credentials
void setCredentials(const QString &clientId, const QString &clientSecret);
```

#### Signals

```cpp
// Emitted when authentication succeeds
void authenticated();

// Emitted when user logs out
void loggedOut();

// Emitted on authentication error
void authenticationError(const QString &error);

// Emitted when token is refreshed
void tokenRefreshed();
```

### TokenStorage

Securely stores OAuth tokens.

```cpp
// Save tokens
void saveTokens(const QString &accessToken, const QString &refreshToken, const QDateTime &expiry);

// Retrieve tokens
QString getAccessToken() const;
QString getRefreshToken() const;

// Check token validity
bool hasValidTokens() const;
bool isTokenExpired() const;

// Clear tokens
void clearTokens();
```

---

## Google Drive API Client

### GoogleDriveClient

Wrapper for Google Drive REST API.

#### File Operations

```cpp
// List files in folder
void listFiles(const QString &folderId = "root", const QString &pageToken = QString());

// Get file metadata
void getFile(const QString &fileId);

// Download file
void downloadFile(const QString &fileId, const QString &localPath);

// Upload new file
void uploadFile(const QString &localPath, const QString &parentId = "root", const QString &fileName = QString());

// Update existing file
void updateFile(const QString &fileId, const QString &localPath);

// Move file
void moveFile(const QString &fileId, const QString &newParentId, const QString &oldParentId);

// Delete file
void deleteFile(const QString &fileId);

// Create folder
void createFolder(const QString &name, const QString &parentId = "root");
```

#### Change Tracking

```cpp
// Get changes since last check
void listChanges(const QString &startPageToken = QString());

// Get start page token
void getStartPageToken();
```

#### Signals

```cpp
void filesListed(const QList<DriveFile> &files, const QString &nextPageToken);
void fileDownloaded(const QString &fileId, const QString &localPath);
void fileUploaded(const DriveFile &file);
void changesReceived(const QList<DriveChange> &changes, const QString &newStartPageToken);
void error(const QString &operation, const QString &error);
```

### Data Structures

#### DriveFile

```cpp
struct DriveFile {
    QString id;              // Unique file ID
    QString name;            // File name
    QString mimeType;        // MIME type
    qint64 size;             // Size in bytes
    QDateTime createdTime;   // Creation time
    QDateTime modifiedTime;  // Modification time
    QString md5Checksum;     // MD5 checksum
    QStringList parents;     // Parent folder IDs
    bool trashed;            // In trash
    bool isFolder;           // Is folder
};
```

#### DriveChange

```cpp
struct DriveChange {
    QString changeId;        // Change ID
    QString fileId;          // File ID
    QDateTime time;          // Change time
    bool removed;            // File was removed
    DriveFile file;          // File metadata (if not removed)
};
```

---

## Synchronization Engine

### SyncEngine

Main synchronization controller.

#### Control Methods

```cpp
// Start sync
void start();

// Stop sync
void stop();

// Pause sync
void pause();

// Resume sync
void resume();

// Trigger immediate sync
void syncNow();

// Set selective sync folders
void setSelectiveSyncFolders(const QStringList &folders);
```

#### Status

```cpp
enum SyncStatus {
    Idle,
    Syncing,
    Uploading,
    Downloading,
    Error,
    Paused,
    NotAuthenticated
};

SyncStatus status() const;
QString currentStatus() const;
bool isRunning() const;
bool isPaused() const;
```

#### Signals

```cpp
void statusChanged(const QString &status);
void progressChanged(qint64 current, qint64 total);
void fileUploaded(const QString &fileName);
void fileDownloaded(const QString &fileName);
void syncError(const QString &error);
void conflictDetected(const QString &fileName);
```

### SyncDatabase

SQLite database for tracking sync state.

Note: All `localPath` parameters are gdrive-root-relative. Passing absolute or home-expanded paths
throws `std::invalid_argument` and is treated as a fatal caller error.

```cpp
// File state management
void saveFileState(const FileSyncState &state);
FileSyncState getFileState(const QString &localPath) const;

// File ID mapping
QString getFileId(const QString &localPath) const;
QString getLocalPath(const QString &fileId) const;
void setFileId(const QString &localPath, const QString &fileId);

// Sync tracking
QDateTime getLastSyncTime(const QString &localPath) const;
void setLastSyncTime(const QString &localPath, const QDateTime &time);

// Offline status
void setOfflineStatus(const QString &localPath, bool offline);
QList<FileSyncState> getOfflineFiles() const;
```

### FileWatcher

Monitors local file system for changes.

```cpp
// Watch management
bool watchDirectory(const QString &path, bool recursive = true);
void unwatchDirectory(const QString &path);
void unwatchAll();

// Ignore patterns
void setIgnorePatterns(const QStringList &patterns);

// Control
void pause();
void resume();
```

#### Signals

```cpp
void fileCreated(const QString &path);
void fileModified(const QString &path);
void fileDeleted(const QString &path);
void fileMoved(const QString &oldPath, const QString &newPath);
void directoryCreated(const QString &path);
void directoryDeleted(const QString &path);
```

### ConflictResolver

Handles file conflicts.

```cpp
enum Strategy {
    KeepLocal,      // Keep local, overwrite remote
    KeepRemote,     // Keep remote, overwrite local
    KeepBoth,       // Keep both versions
    AskUser         // Prompt user
};

// Check for conflicts
bool hasConflict(const QString &localPath) const;
QList<ConflictData> getConflicts() const;

// Resolve conflicts
void resolveConflict(const QString &localPath, Strategy strategy);
void resolveAllConflicts();
```

---

## FUSE Virtual File System

### FuseDriver

FUSE filesystem driver.

```cpp
// Mount management
bool mount();
void unmount();
bool isMounted() const;

// Configuration
QString mountPoint() const;
void setMountPoint(const QString &path);

// Static check
static bool isFuseAvailable();
```

#### Signals

```cpp
void mounted();
void unmounted();
void mountError(const QString &error);
void fileAccessed(const QString &path);
```

### FileCache

Local cache for FUSE files.

```cpp
// Cache management
QString getCachePath(const QString &fileId) const;
bool isCached(const QString &fileId) const;
QString addToCache(const QString &fileId, const QString &localPath);
void removeFromCache(const QString &fileId);

// Dirty tracking
void markDirty(const QString &fileId);
void markClean(const QString &fileId);
QStringList getDirtyFiles() const;

// Size management
qint64 maxCacheSize() const;
void setMaxCacheSize(qint64 bytes);
qint64 currentCacheSize() const;
void clearCache();
```

---

## Utilities

### NotificationManager

Desktop notifications.

```cpp
enum Urgency { Low, Normal, Critical };

void showNotification(const QString &title, const QString &message,
                     Urgency urgency = Normal, int timeoutMs = 5000);
void showInfo(const QString &title, const QString &message);
void showWarning(const QString &title, const QString &message);
void showError(const QString &title, const QString &message);

bool isEnabled() const;
void setEnabled(bool enabled);
```

### OfflineManager

Offline file access.

```cpp
// Status
bool isOffline() const;
bool isAvailableOffline(const QString &fileId) const;
QStringList getOfflineFiles() const;
qint64 offlineStorageUsed() const;

// Management
void markForOffline(const QString &fileId);
void removeFromOffline(const QString &fileId);
void markFolderForOffline(const QString &folderId);
void syncOfflineFiles();
```

### BandwidthManager

Bandwidth throttling.

```cpp
enum Schedule { Always, WorkHours, OffHours, WeekendsOnly };

// Upload limits
void setUploadLimitEnabled(bool enabled);
void setUploadLimit(qint64 bytesPerSecond);
void setUploadSchedule(Schedule schedule);
qint64 currentUploadLimit() const;

// Download limits
void setDownloadLimitEnabled(bool enabled);
void setDownloadLimit(qint64 bytesPerSecond);
void setDownloadSchedule(Schedule schedule);
qint64 currentDownloadLimit() const;

// Monitoring
qint64 currentUploadSpeed() const;
qint64 currentDownloadSpeed() const;
```

---

## Error Handling

All API operations should handle errors gracefully:

1. **Network Errors**: Retry with exponential backoff
2. **Authentication Errors**: Refresh token or re-authenticate
3. **Rate Limiting**: Respect API quotas and back off
4. **File Conflicts**: Use ConflictResolver
5. **FUSE Errors**: Return appropriate errno values

Example error handling:

```cpp
connect(driveClient, &GoogleDriveClient::error,
        this, [](const QString &operation, const QString &error) {
    qWarning() << "API error in" << operation << ":" << error;
    // Handle based on error type
});
```
