# Via Architecture Documentation

> **Version**: 1.0  
> **Last Updated**: January 2026  
> **Target Audience**: AI Agents, Developers, Contributors

## Table of Contents

1. [Overview](#overview)
2. [System Components](#system-components)
    - [Authentication (`src/auth/`)](#authentication-srcauth)
    - [API Client (`src/api/`)](#api-client-srcapi)
    - [Synchronization Engine (`src/sync/`)](#synchronization-engine-srcsync)
    - [User Interface (`src/ui/`)](#user-interface-srcui)
    - [Virtual File System (`src/fuse/`)](#virtual-file-system-srcfuse)
    - [Utilities (`src/utils/`)](#utilities-srcutils)
3. [Data Flow](#data-flow)
4. [File Locations](#file-locations)
5. [Configuration](#configuration)
6. [Building](#building)
7. [Class Reference](#class-reference)

---

## Overview

Via is a Google Drive desktop client for Linux that provides:

- Bidirectional file synchronization with folder structure preservation
- OAuth 2.0 authentication with refresh token persistence
- Tree-based diff sync algorithm
- System tray integration
- FUSE virtual filesystem (optional)

### Technology Stack

- **Language**: C++20
- **Framework**: Qt 6
- **Build System**: CMake
- **Database**: SQLite (for sync state)
- **Virtual FS**: FUSE 3

---

## System Components

### Authentication (`src/auth/`)

**Purpose**: Manages OAuth 2.0 authentication with Google Drive API.

| File                      | Class               | Description                      |
| ------------------------- | ------------------- | -------------------------------- |
| `GoogleAuthManager.h/cpp` | `GoogleAuthManager` | OAuth 2.0 flow, token management |
| `TokenStorage.h/cpp`      | `TokenStorage`      | Secure token persistence         |

#### Key Features

- OAuth 2.0 Authorization Code Flow
- Refresh token persistence (never expire)
- Automatic access token refresh every 55 minutes
- Credentials stored in `~/.config/Via/Via.conf`

#### Public API

```cpp
// Check authentication status
bool isAuthenticated() const;

// Start OAuth flow (opens browser)
void authenticate();

// Refresh access token using stored refresh token
void refreshTokens();

// Set custom OAuth credentials
void setCredentials(const QString &clientId, const QString &clientSecret);
```

#### Signals

- `authenticated()` - OAuth flow completed successfully
- `tokenRefreshed()` - Access token refreshed
- `authenticationError(QString)` - Auth failed

---

### API Client (`src/api/`)

**Purpose**: Implements Google Drive REST API v3.

| File                      | Class               | Description             |
| ------------------------- | ------------------- | ----------------------- |
| `GoogleDriveClient.h/cpp` | `GoogleDriveClient` | REST API operations     |
| `DriveFile.h/cpp`         | `DriveFile`         | File metadata structure |
| `DriveChange.h/cpp`       | `DriveChange`       | Change event structure  |

#### Supported Operations

| Method                | API Endpoint                | Description                             |
| --------------------- | --------------------------- | --------------------------------------- |
| `listFiles()`         | `files.list`                | List files with `'me' in owners` filter |
| `getFile()`           | `files.get`                 | Get file metadata                       |
| `downloadFile()`      | `files.get?alt=media`       | Download file content                   |
| `uploadFile()`        | `files.create`              | Upload new file                         |
| `updateFile()`        | `files.update`              | Update existing file                    |
| `deleteFile()`        | `files.delete`              | Delete file                             |
| `createFolder()`      | `files.create`              | Create folder                           |
| `listChanges()`       | `changes.list`              | Get remote changes                      |
| `getStartPageToken()` | `changes.getStartPageToken` | Get initial change token                |

#### DriveFile Structure

```cpp
struct DriveFile {
    QString id;
    QString name;
    QString mimeType;
    QStringList parents;
    QDateTime modifiedTime;
    QDateTime createdTime;
    qint64 size;
    QString md5Checksum;
    bool trashed;
    bool ownedByMe;
    // For shortcuts:
    QString shortcutTargetId;
    QString shortcutTargetMimeType;
};
```

---

### Synchronization Engine (`src/sync/`)

**Purpose**: Core bidirectional sync logic with tree-based diff algorithm.

| File                     | Class              | Description                    |
| ------------------------ | ------------------ | ------------------------------ |
| `SyncEngine.h/cpp`       | `SyncEngine`       | Main sync orchestration        |
| `SyncDatabase.h/cpp`     | `SyncDatabase`     | SQLite state tracking          |
| `SyncQueue.h/cpp`        | `SyncQueue`        | Thread-safe operation queue    |
| `FileWatcher.h/cpp`      | `FileWatcher`      | Local filesystem monitoring    |
| `ConflictResolver.h/cpp` | `ConflictResolver` | Conflict resolution strategies |

#### Sync Modes

| Mode             | Behavior                                      |
| ---------------- | --------------------------------------------- |
| `KeepNewest`     | Bidirectional sync, keep newest version       |
| `RemoteReadOnly` | Only download from remote, never upload       |
| `RemoteNoDelete` | Bidirectional sync, don't delete remote files |

#### Conflict Resolution Strategies

| Strategy     | Behavior                                                     |
| ------------ | ------------------------------------------------------------ |
| `KeepBoth`   | Create local conflict copy: `file (local conflict DATE).ext` |
| `KeepLocal`  | Always keep local version                                    |
| `KeepRemote` | Always keep remote version                                   |
| `KeepNewest` | Compare timestamps, keep newest                              |
| `AskUser`    | Show dialog for user decision                                |

#### Tree-Based Sync Algorithm

1. **Build Remote Tree**: Fetch all files with `'me' in owners`, build hierarchy
2. **Build Local Tree**: Scan local sync folder recursively
3. **Discover Root IDs**: Identify My Drive root folder ID(s)
4. **Diff & Prune**: Compare trees:
    - Files in both with matching mod times → skip
    - Files only local → upload
    - Files only remote → download
    - Both modified → conflict resolution

#### Sync Loop Prevention

- Stores modification time at sync (not current time)
- Changes must be 2+ seconds newer than cached mod time
- `m_recentlySynced` hash tracks recent operations
- Deleted file records are automatically purged after 31 days

#### Database Schema

```sql
-- Tracked files
CREATE TABLE files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    local_path TEXT UNIQUE NOT NULL,
    file_id TEXT,
    md5_checksum TEXT,
    size INTEGER DEFAULT 0,
    local_modified TEXT,      -- Local file modification time
    remote_modified TEXT,     -- Remote file modification time
    last_synced TEXT,         -- Last sync operation time
    is_folder INTEGER DEFAULT 0,
    is_offline INTEGER DEFAULT 0
);

-- Deleted files (prevents re-download, purged after 31 days)
CREATE TABLE deleted_files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    local_path TEXT UNIQUE NOT NULL,
    file_id TEXT,
    deleted_at TEXT           -- Used for 31-day purge
);

-- Settings storage
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT
);

-- Conflict tracking
CREATE TABLE conflicts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    local_path TEXT NOT NULL,
    file_id TEXT,
    conflict_path TEXT,
    detected_at TEXT,
    resolved INTEGER DEFAULT 0
);
```

---

### User Interface (`src/ui/`)

**Purpose**: Qt-based GUI with system tray integration.

| File                      | Class               | Description                |
| ------------------------- | ------------------- | -------------------------- |
| `MainWindow.h/cpp`        | `MainWindow`        | Main application window    |
| `SettingsWindow.h/cpp`    | `SettingsWindow`    | Settings dialog            |
| `SystemTrayManager.h/cpp` | `SystemTrayManager` | System tray icon/menu      |
| `ConflictDialog.h/cpp`    | `ConflictDialog`    | Conflict resolution dialog |

#### MainWindow Features

- Sync status display
- Manual sync button
- Pause/Resume controls
- File activity log

#### Settings Tabs

1. **Account**: OAuth credentials, sign in/out
2. **Sync**: Sync folder path, sync mode, conflict resolution
3. **Notifications**: Enable/disable notifications
4. **Advanced**: Bandwidth limits, log settings

---

### Virtual File System (`src/fuse/`)

**Purpose**: FUSE-based virtual filesystem for on-demand file access.

| File               | Class        | Description                    |
| ------------------ | ------------ | ------------------------------ |
| `FuseDriver.h/cpp` | `FuseDriver` | FUSE operations implementation |
| `FileCache.h/cpp`  | `FileCache`  | Local file caching             |

#### FUSE Operations

- `getattr` - File metadata
- `readdir` - Directory listing
- `open` - Open file (triggers download)
- `read` - Read file content
- `write` - Write file content
- `create` - Create new file
- `unlink` - Delete file
- `mkdir` - Create directory
- `rmdir` - Remove directory

---

### Utilities (`src/utils/`)

**Purpose**: Cross-cutting utility classes.

| File                        | Class                 | Description                |
| --------------------------- | --------------------- | -------------------------- |
| `NotificationManager.h/cpp` | `NotificationManager` | Desktop notifications      |
| `BandwidthManager.h/cpp`    | `BandwidthManager`    | Upload/download throttling |
| `OfflineManager.h/cpp`      | `OfflineManager`      | Offline file access        |
| `LogManager.h/cpp`          | `LogManager`          | File-based logging         |

#### LogManager Features

- Log location: `~/.local/share/Via/logs/`
- File format: `via_YYYY-MM-DD_HH-mm-ss.log`
- Auto-rotation at 10MB
- Keeps 5 most recent files
- Log format: `[timestamp] [LEVEL] [file:line] message`

---

## Data Flow

### Initial Sync Flow

```
main.cpp
  └── GoogleAuthManager::hasValidTokens()
        └── If expired: refreshTokens()
              └── tokenRefreshed signal
  └── SyncEngine::start()
        └── performInitialSync()
              └── GoogleDriveClient::listFiles()
              └── Build remote tree
              └── Build local tree
              └── compareAndSyncTrees()
                    └── queueDownload() / queueUpload()
              └── processSyncQueue()
```

### Live Sync Flow

```
FileWatcher::fileChanged()
  └── SyncEngine::onLocalFileModified()
        └── isChangeNewEnough() check
        └── queueUpload()
        └── processSyncQueue()

GoogleDriveClient::changesReceived()
  └── SyncEngine::onRemoteChangesReceived()
        └── Filter: ownedByMe, not trashed
        └── Resolve parent path
        └── queueDownload()
        └── processSyncQueue()
```

---

## File Locations

| Data                | Location                         |
| ------------------- | -------------------------------- |
| Settings            | `~/.config/Via/Via.conf`         |
| Sync Database       | `~/.local/share/Via/via_sync.db` |
| Logs                | `~/.local/share/Via/logs/`       |
| Default Sync Folder | `~/GoogleDrive/`                 |

---

## Configuration

### QSettings Keys

```ini
[Auth]
clientId=<OAuth client ID>
clientSecret=<OAuth client secret>
accessToken=<encoded>
refreshToken=<encoded>
tokenExpiry=<ISO datetime>

[Sync]
syncFolder=/home/user/GoogleDrive
syncMode=0  # 0=KeepNewest, 1=RemoteReadOnly, 2=RemoteNoDelete
conflictResolution=0  # 0=KeepBoth, 1=KeepLocal, 2=KeepRemote, 3=KeepNewest, 4=AskUser
changeToken=<Google Drive changes API token>
selectiveFolders=<comma-separated folder names>

[Notifications]
enabled=true
```

---

## Building

### Dependencies

```bash
# Ubuntu/Debian
sudo apt install qt6-base-dev qt6-networkauth-dev libfuse3-dev cmake g++
```

### Build Commands

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### CMake Targets

- `via` - Main application
- `install` - Install to system

---

## Class Reference

### Signal Flow Diagram

```
GoogleAuthManager
  ├── authenticated() → MainWindow::onAuthenticated()
  │                    → SyncEngine::start()
  └── tokenRefreshed() → SyncEngine::start()

GoogleDriveClient
  ├── filesListed() → SyncEngine::onInitialFilesPage()
  ├── fileUploaded() → SyncEngine::onFileUploaded()
  ├── fileDownloaded() → SyncEngine::onFileDownloaded()
  └── changesReceived() → SyncEngine::onRemoteChangesReceived()

SyncEngine
  ├── fileUploaded() → NotificationManager::showNotification()
  ├── fileDownloaded() → NotificationManager::showNotification()
  └── statusChanged() → MainWindow::onStatusChanged()

FileWatcher
  ├── fileCreated() → SyncEngine::onLocalFileCreated()
  ├── fileModified() → SyncEngine::onLocalFileModified()
  └── fileDeleted() → SyncEngine::onLocalFileDeleted()
```

---

## For AI Agents

### Common Tasks

**To modify sync behavior:**

- Edit `src/sync/SyncEngine.cpp`
- Key methods: `performInitialSync()`, `processSyncQueue()`, `compareAndSyncTrees()`

**To add new conflict resolution:**

- Edit `src/sync/ConflictResolver.h/cpp`
- Add enum value in `ConflictResolver.h`
- Implement in `resolve()` method

**To modify API calls:**

- Edit `src/api/GoogleDriveClient.cpp`
- Check Google Drive API v3 documentation

**To add UI elements:**

- Edit `src/ui/MainWindow.cpp` or `src/ui/SettingsWindow.cpp`
- Qt Widgets-based UI

**To add new settings:**

1. Add QSettings key in relevant class
2. Add UI element in `SettingsWindow`
3. Connect signal/slot

### Important Invariants

1. Only files with `ownedByMe=true` are synced
2. Only files with parent chain to root folder ID are synced
3. Sync loop prevention requires 2+ second mod time difference
4. Downloaded files preserve original modification time
