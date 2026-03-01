# API Client System

## Overview

The API client (`src/api/`) provides a Qt-based interface to the Google Drive REST API v3. It handles all communication with Google's servers including file operations, metadata queries, and change tracking.

## Files

| File                    | Purpose                  |
| ----------------------- | ------------------------ |
| `GoogleDriveClient.h`   | Client class declaration |
| `GoogleDriveClient.cpp` | REST API implementation  |
| `DriveFile.h`           | File metadata structure  |
| `DriveFile.cpp`         | File parsing and helpers |
| `DriveChange.h`         | Change event structure   |
| `DriveChange.cpp`       | Change parsing           |

## GoogleDriveClient

### Responsibilities

-   Execute REST API calls with proper authentication
-   Parse JSON responses into Qt objects
-   Handle errors and network issues
-   Emit signals for async operation completion

### API Base URLs

```cpp
static const QString API_BASE_URL = "https://www.googleapis.com/drive/v3";
static const QString UPLOAD_URL = "https://www.googleapis.com/upload/drive/v3";
```

### Supported Operations

#### File Listing

```cpp
void listFiles(const QString &folderId = "root", const QString &pageToken = QString());
```

-   Uses `'me' in owners` filter to exclude "Shared with me"
-   Supports pagination via `nextPageToken`
-   Fields requested: `id, name, mimeType, parents, modifiedTime, createdTime, size, md5Checksum, trashed, ownedByMe, shortcutDetails`

#### File Download

```cpp
void downloadFile(const QString &fileId, const QString &localPath);
```

-   Uses `alt=media` parameter
-   Emits progress signals
-   Supports bandwidth limiting

#### File Upload

```cpp
void uploadFile(const QString &localPath, const QString &parentId = "root",
               const QString &fileName = QString());
```

-   Uses multipart upload for files < 5MB
-   Uses resumable upload for larger files
-   Sets appropriate parent folder

#### File Update

```cpp
void updateFile(const QString &fileId, const QString &localPath);
```

-   Updates file content keeping same ID
-   Preserves file metadata

#### File Delete

```cpp
void deleteFile(const QString &fileId);
```

-   Moves file to trash (not permanent delete)

#### Folder Creation

```cpp
void createFolder(const QString &name, const QString &parentId = "root");
```

-   Creates folder with specified parent
-   Returns created folder metadata

#### Change Tracking

```cpp
void getStartPageToken();
void listChanges(const QString &startPageToken = QString());
```

-   Gets initial change token
-   Lists changes since token
-   Returns new token for next check

### Signals

| Signal             | Parameters                                      | Description            |
| ------------------ | ----------------------------------------------- | ---------------------- |
| `filesListed`      | `QList<DriveFile>, QString nextPageToken`       | File list received     |
| `fileReceived`     | `DriveFile`                                     | File metadata received |
| `fileDownloaded`   | `QString fileId, QString localPath`             | Download complete      |
| `fileUploaded`     | `DriveFile`                                     | Upload complete        |
| `fileDeleted`      | `QString fileId`                                | Delete complete        |
| `folderCreated`    | `DriveFile`                                     | Folder created         |
| `changesReceived`  | `QList<DriveChange>, QString newToken`          | Changes received       |
| `downloadProgress` | `QString fileId, qint64 received, qint64 total` | Download progress      |
| `uploadProgress`   | `QString path, qint64 sent, qint64 total`       | Upload progress        |
| `error`            | `QString operation, QString error`              | API error              |

## DriveFile Structure

```cpp
struct DriveFile {
    QString id;              // Unique file ID
    QString name;            // Display name
    QString mimeType;        // MIME type (application/vnd.google-apps.folder for folders)
    QStringList parents;     // Parent folder IDs
    QDateTime modifiedTime;  // Last modified time
    QDateTime createdTime;   // Creation time
    qint64 size;             // File size in bytes
    QString md5Checksum;     // MD5 hash (files only)
    bool trashed;            // In trash
    bool ownedByMe;          // User owns this file

    // Shortcut support
    QString shortcutTargetId;
    QString shortcutTargetMimeType;

    // Helpers
    bool isFolder() const;
    bool isShortcut() const;
};
```

### Folder Detection

```cpp
bool DriveFile::isFolder() const {
    return mimeType == "application/vnd.google-apps.folder";
}
```

### Shortcut Detection

```cpp
bool DriveFile::isShortcut() const {
    return mimeType == "application/vnd.google-apps.shortcut";
}
```

## DriveChange Structure

```cpp
struct DriveChange {
    QString kind;            // "drive#change"
    QString fileId;          // Changed file ID
    QDateTime time;          // Change time
    bool removed;            // File was removed
    DriveFile file;          // File metadata (if not removed)
};
```

## Request Format

All requests include:

```http
Authorization: Bearer <access_token>
Content-Type: application/json
```

## Error Handling

### HTTP Status Codes

| Code | Meaning      | Action              |
| ---- | ------------ | ------------------- |
| 401  | Unauthorized | Refresh token       |
| 403  | Forbidden    | Check permissions   |
| 404  | Not found    | File may be deleted |
| 429  | Rate limited | Backoff and retry   |
| 500+ | Server error | Retry with backoff  |

### Error Signal

```cpp
void onApiError(const QString &operation, const QString &error) {
    qWarning() << operation << "error:" << error;
    emit syncError(error);
}
```

## Bandwidth Management

```cpp
void setUploadLimit(qint64 bytesPerSecond);   // 0 = unlimited
void setDownloadLimit(qint64 bytesPerSecond); // 0 = unlimited
```

Throttling is applied per-operation, not globally.
