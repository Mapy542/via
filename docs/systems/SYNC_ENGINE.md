# Synchronization Engine

## Overview

The synchronization engine (`src/sync/`) is the core component that keeps local and remote files in sync. It implements a tree-based diff algorithm with support for bidirectional sync, conflict resolution, and folder structure preservation.

## Architecture

### Live Sync System

The continuous live sync system follows a multi-stage architecture to prevent sync loops:

1. **Change Monitoring** - FileWatcher and GoogleDriveClient detect local/remote changes
2. **Change Queueing** - Changes are queued into `m_pendingChanges` for validation
3. **Change Validation** - Changes are compared against database stored modification times
4. **Sync Operations** - Only validated changes (2+ seconds newer than DB) are processed

```
┌─────────────────┐     ┌─────────────────┐
│  FileWatcher    │────▶│                 │
│  (local)        │     │ Pending Changes │
└─────────────────┘     │ Queue           │
                        │                 │
┌─────────────────┐     │                 │
│ GoogleDrive     │────▶│                 │
│ Client (remote) │     └────────┬────────┘
└─────────────────┘              │
                                 ▼
                        ┌─────────────────┐
                        │ Validate Change │
                        │ (2+ sec check)  │
                        └────────┬────────┘
                                 │
                                 ▼
                        ┌─────────────────┐
                        │  Sync Queue     │
                        │ (Upload/Dl/Del) │
                        └─────────────────┘
```

## Files

| File                   | Purpose                         |
| ---------------------- | ------------------------------- |
| `SyncEngine.h`         | Main orchestration class        |
| `SyncEngine.cpp`       | Sync logic implementation       |
| `InitialSync.cpp`      | Initial tree-based sync         |
| `LocalChanges.cpp`     | Local file change handlers      |
| `RemoteChanges.cpp`    | Remote change handlers          |
| `SyncDatabase.h`       | SQLite state tracking interface |
| `SyncDatabase.cpp`     | Database operations             |
| `SyncQueue.h`          | Thread-safe operation queue     |
| `SyncQueue.cpp`        | Queue implementation            |
| `FileWatcher.h`        | Local filesystem monitoring     |
| `FileWatcher.cpp`      | QFileSystemWatcher wrapper      |
| `ConflictResolver.h`   | Conflict resolution strategies  |
| `ConflictResolver.cpp` | Resolution implementation       |

## SyncEngine

### Responsibilities

-   Orchestrate initial and continuous sync
-   Build and compare file trees
-   Queue sync operations
-   Handle conflicts
-   Track sync state
-   Purge old deleted file records (daemon thread)

### Sync Modes

```cpp
enum SyncMode {
    KeepNewest,       // Bidirectional, keep newest version
    RemoteReadOnly,   // Only download, never upload
    RemoteNoDelete    // Bidirectional, but don't delete remote
};
```

### Sync Status

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
```

## Live Sync Change Validation

### The 2-Second Rule

All incoming changes (local or remote) must be at least **2 seconds newer** than the database stored modification time to be processed. This prevents sync loops where:

-   A file is downloaded
-   FileWatcher detects it as a local change
-   The change would trigger an upload
-   The upload would trigger another download

```cpp
// Key constants
static const int MIN_CHANGE_DIFF_SECS = 2;  // Change must be 2+ seconds newer
static const int DELETED_RECORD_MAX_AGE_DAYS = 31;  // Purge after 31 days

// Validation logic
bool validateChange(const PendingChange &change) const {
    QDateTime storedModTime = m_database->getLocalModifiedTime(change.relativePath);
    qint64 diffSecs = storedModTime.secsTo(change.detectedModTime);
    return diffSecs >= MIN_CHANGE_DIFF_SECS;
}
```

### Metadata Preservation

Downloaded files preserve their original modification time:

-   File modified in December, synced in January → file has December timestamp locally

```cpp
void onFileDownloaded(const QString &fileId, const QString &localPath) {
    QDateTime originalModTime = m_pendingDownloadModTimes.take(fileId);

    // Set file modification time using utime()
    struct utimbuf times;
    times.actime = times.modtime = originalModTime.toSecsSinceEpoch();
    utime(localPath.toLocal8Bit().constData(), &times);

    // Store in database
    m_database->setLocalModifiedTime(relativePath, originalModTime);
    m_database->setRemoteModifiedTime(relativePath, originalModTime);
}
```

## Tree-Based Sync Algorithm

### Phase 1: Build Remote Tree

```cpp
void performInitialSync() {
    // 1. Fetch all files with 'me' in owners filter
    m_driveClient->listFiles("root", QString());
}

void onInitialFilesPage(const QList<DriveFile> &files, const QString &nextPageToken) {
    // 2. Collect all files
    m_allRemoteItems.append(files);

    if (!nextPageToken.isEmpty()) {
        m_driveClient->listFiles("root", nextPageToken);
    } else {
        buildRemoteTree();
    }
}
```

### Phase 2: Discover Root Folder IDs

```cpp
void buildRemoteTree() {
    // Root IDs are parent IDs that don't match any folder ID
    QSet<QString> allFolderIds;
    for (const auto &item : m_allRemoteItems) {
        if (item.isFolder()) {
            allFolderIds.insert(item.id);
        }
    }

    for (const auto &item : m_allRemoteItems) {
        for (const auto &parentId : item.parents) {
            if (!allFolderIds.contains(parentId)) {
                m_rootFolderIds.insert(parentId);
            }
        }
    }

    // IMPORTANT: Only the primary root ID (My Drive root) is mapped to empty path
    // This prevents files from shared drives or external folders being placed in root
    // The primary root is identified as the one with the most direct children
    // In case of ties, the first root ID (sorted alphabetically) is used for determinism
    if (m_rootFolderIds.size() == 1) {
        m_folderIdToPath[*m_rootFolderIds.begin()] = "";
    } else if (m_rootFolderIds.size() > 1) {
        // Count children for each root
        QHash<QString, int> rootChildCounts;
        for (const QString &rootId : m_rootFolderIds) {
            rootChildCounts[rootId] = 0;
        }
        for (const auto &item : m_allRemoteItems) {
            QString parentId = item.parentId();
            if (m_rootFolderIds.contains(parentId)) {
                rootChildCounts[parentId]++;
            }
        }

        // Find root with most children (tie-breaking: alphabetical order)
        QString primaryRoot;
        int maxChildren = -1;
        for (auto it = rootChildCounts.begin(); it != rootChildCounts.end(); ++it) {
            if (it.value() > maxChildren) {
                maxChildren = it.value();
                primaryRoot = it.key();
            }
        }
        m_folderIdToPath[primaryRoot] = "";
    }
}
```

### Phase 3: Build Folder Hierarchy

```cpp
// Iteratively resolve folder paths
bool progress = true;
while (progress) {
    progress = false;
    for (const auto &item : m_allRemoteItems) {
        if (!item.isFolder()) continue;
        if (m_folderIdToPath.contains(item.id)) continue;

        for (const auto &parentId : item.parents) {
            if (m_rootFolderIds.contains(parentId)) {
                m_folderIdToPath[item.id] = item.name;
                progress = true;
            } else if (m_folderIdToPath.contains(parentId)) {
                m_folderIdToPath[item.id] = m_folderIdToPath[parentId] + "/" + item.name;
                progress = true;
            }
        }
    }
}
```

### Phase 4: Compare Trees

```cpp
void compareAndSyncTrees() {
    // For each remote file:
    for (const auto &item : m_allRemoteItems) {
        QString relativePath = resolveRelativePath(item);

        if (relativePath.isEmpty()) {
            // Not in My Drive hierarchy - skip
            continue;
        }

        QString localPath = m_syncFolder + "/" + relativePath;

        if (QFile::exists(localPath)) {
            // Compare modification times
            QFileInfo localInfo(localPath);
            if (localInfo.lastModified() == item.modifiedTime) {
                // In sync - skip
            } else {
                // Conflict resolution needed
            }
        } else {
            // Download from remote
            queueDownload(item.id, relativePath, item.modifiedTime);
        }
    }

    // For each local file not in remote:
    // Upload to remote
}
```

## Sync Loop Prevention

### Problem

When a file is downloaded, the filesystem watcher detects it as a local change and tries to upload it, creating an infinite loop.

### Solution

Store modification time at sync, require 2+ seconds newer change:

```cpp
QHash<QString, QDateTime> m_recentlySynced;

void markRecentlySynced(const QString &path, const QDateTime &modTime) {
    m_recentlySynced[path] = modTime;
}

bool isChangeNewEnough(const QString &path, const QDateTime &detectedModTime) const {
    if (!m_recentlySynced.contains(path)) return true;

    QDateTime cachedTime = m_recentlySynced[path];
    return detectedModTime > cachedTime.addSecs(2);
}
```

## SyncDatabase

### Schema

```sql
CREATE TABLE synced_files (
    id INTEGER PRIMARY KEY,
    relative_path TEXT UNIQUE NOT NULL,
    remote_id TEXT,
    local_modified TEXT,
    remote_modified TEXT,
    md5_checksum TEXT
);

CREATE TABLE deleted_files (
    id INTEGER PRIMARY KEY,
    relative_path TEXT UNIQUE NOT NULL,
    deleted_at TEXT NOT NULL
);
```

### Operations

```cpp
void addSyncedFile(const QString &relativePath, const QString &remoteId,
                   const QDateTime &localMod, const QDateTime &remoteMod);
void removeSyncedFile(const QString &relativePath);
bool isFileDeleted(const QString &relativePath);
void markFileDeleted(const QString &relativePath);
int purgeOldDeletedRecords(int maxAgeDays = 31);  // Daemon task
QDateTime getLocalModifiedTime(const QString &relativePath);
QDateTime getRemoteModifiedTime(const QString &relativePath);
void setLocalModifiedTime(const QString &relativePath, const QDateTime &time);
void setRemoteModifiedTime(const QString &relativePath, const QDateTime &time);
```

### Deleted File Purge Daemon

A background timer purges deleted file records older than 31 days:

```cpp
// Runs every 24 hours
static const int DELETION_PURGE_INTERVAL_MS = 86400000;  // 24 hours
static const int DELETED_RECORD_MAX_AGE_DAYS = 31;

void purgeOldDeletedRecords() {
    m_database->purgeOldDeletedRecords(DELETED_RECORD_MAX_AGE_DAYS);
}
```

This ensures:

1. Deleted files are tracked long enough to prevent re-sync from remote
2. Database doesn't grow unbounded over time

## SyncQueue

Thread-safe operation queue:

```cpp
struct SyncQueueItem {
    enum Type { Upload, Download, Delete };
    Type type;
    QString localPath;
    QString remotePath;
    QString fileId;
    QDateTime originalModifiedTime;  // For preserving timestamps
};

class SyncQueue {
    void enqueue(const SyncQueueItem &item);
    SyncQueueItem dequeue();
    int count() const;
    bool isEmpty() const;
};
```

## FileWatcher

Wraps QFileSystemWatcher:

```cpp
signals:
    void fileCreated(const QString &path);
    void fileModified(const QString &path);
    void fileDeleted(const QString &path);
```

## ConflictResolver

### Strategies

```cpp
enum Strategy {
    KeepBoth,      // Create local conflict copy
    KeepLocal,     // Overwrite remote
    KeepRemote,    // Overwrite local
    KeepNewest,    // Compare timestamps
    AskUser        // Show dialog
};
```

### Conflict Copy Naming

```
filename (local conflict 2026-01-03_12-30-45).ext
```

### KeepNewest Logic

```cpp
if (localModified > remoteModified) {
    return KeepLocal;
} else if (remoteModified > localModified) {
    return KeepRemote;
} else {
    // Both modified - keep both
    return KeepBoth;
}
```

## File Timestamp Preservation

Downloaded files get their original modification time:

```cpp
void onFileDownloaded(const QString &fileId, const QString &localPath) {
    if (m_pendingDownloadModTimes.contains(fileId)) {
        QDateTime origModTime = m_pendingDownloadModTimes.take(fileId);

        // Set file modification time using utime()
        struct utimbuf times;
        times.actime = times.modtime = origModTime.toSecsSinceEpoch();
        utime(localPath.toLocal8Bit().constData(), &times);

        // Store in database for future validation
        m_database->setLocalModifiedTime(relativePath, origModTime);
        m_database->setRemoteModifiedTime(relativePath, origModTime);
    }
}
```

## Settings Keys

The sync engine uses the following QSettings keys:

```ini
[Sync]
syncFolder=/home/user/GoogleDrive
syncMode=0        # 0=KeepNewest, 1=RemoteReadOnly, 2=RemoteNoDelete
conflictResolution=0  # 0=KeepBoth, 1=KeepLocal, 2=KeepRemote, 3=KeepNewest, 4=AskUser
changeToken=...   # Google Drive changes API token
selectiveFolders=folder1,folder2  # Folders to selectively sync
```
