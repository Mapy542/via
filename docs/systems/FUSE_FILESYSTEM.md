# FUSE Virtual File System

## Overview

The FUSE system (`src/fuse/`) provides an optional virtual filesystem that presents Google Drive as a mounted directory. Files are fetched on-demand rather than being fully synced locally.

See the detailed **FUSE Procedure Flow Chart** in `src/sync/dataflow.md` for the complete architecture and data flow diagrams.

## Files

| File             | Purpose                                          |
| ---------------- | ------------------------------------------------ |
| `FuseDriver.h`   | Main driver, FUSE callbacks, thread coordination |
| `FuseDriver.cpp` | FUSE callback implementation                     |
| `FileCache.h`    | LRU cache manager for file contents              |
| `FileCache.cpp`  | Cache implementation with dirty tracking         |

## Component Interfaces

### IMetadataProvider Interface

Defines the interface for metadata operations used by FuseDriver:

```cpp
class IMetadataProvider {
   public:
    // Get stat structure for a path
    virtual bool getStatByPath(const QString& path, struct stat* stbuf) const = 0;

    // Get file ID from path
    virtual QString getFileIdByPath(const QString& path) const = 0;

    // Get children of a directory
    virtual QStringList getChildNames(const QString& path) const = 0;

    // Check if path is a directory
    virtual bool isDirectory(const QString& path) const = 0;

    // Check if directory is empty (for rmdir)
    virtual bool isDirectoryEmpty(const QString& path) const = 0;

    // Refresh metadata from remote
    virtual bool refreshPath(const QString& path) = 0;
    virtual bool refreshRoot() = 0;

    // Metadata mutations
    virtual bool createEntry(const QString& path, const QString& fileId, bool isFolder) = 0;
    virtual bool removeEntry(const QString& path) = 0;
    virtual bool movePath(const QString& oldPath, const QString& newPath) = 0;
};
```

### IFileCacheProvider Interface

Defines the interface for file cache operations:

```cpp
class IFileCacheProvider {
   public:
    // Check if file is cached
    virtual bool isCached(const QString& fileId) const = 0;

    // Get cached path (may block for download)
    virtual QString getCachedPath(const QString& fileId, qint64 expectedSize = 0) = 0;

    // Get cache path without downloading
    virtual QString getCachePathForFile(const QString& fileId) const = 0;

    // Update LRU tracking
    virtual void updateAccessTime(const QString& fileId) = 0;

    // Dirty file tracking
    virtual void markDirty(const QString& fileId, const QString& path) = 0;

    // Cache management
    virtual void removeFromCache(const QString& fileId) = 0;
    virtual bool recordCacheEntry(const QString& fileId, const QString& localPath, qint64 size) = 0;
};
```

## FuseDriver

### Responsibilities

- Mount/unmount virtual filesystem
- Implement FUSE operation callbacks
- Coordinate between MetadataCache, FileCache, and GoogleDriveClient
- Manage background worker threads (DirtySyncWorker, MetadataRefreshWorker)
- Handle open file handles and dirty file tracking

### FUSE Operations

| Operation       | Purpose                          | Database Tables Used                     |
| --------------- | -------------------------------- | ---------------------------------------- |
| `fuse_getattr`  | Get file/folder attributes       | `fuse_metadata`                          |
| `fuse_readdir`  | List directory contents          | `fuse_metadata`                          |
| `fuse_open`     | Open file (may trigger download) | `fuse_metadata`, `fuse_cache_entries`    |
| `fuse_read`     | Read file contents               | `fuse_cache_entries`                     |
| `fuse_write`    | Write file contents              | `fuse_cache_entries`, `fuse_dirty_files` |
| `fuse_create`   | Create new file                  | `fuse_metadata`                          |
| `fuse_unlink`   | Delete file                      | `fuse_metadata`, `fuse_cache_entries`    |
| `fuse_mkdir`    | Create directory                 | `fuse_metadata`                          |
| `fuse_rmdir`    | Remove directory                 | `fuse_metadata`                          |
| `fuse_rename`   | Rename/move file                 | `fuse_metadata`                          |
| `fuse_truncate` | Truncate file                    | `fuse_dirty_files`                       |

### Mount Options

```cpp
struct fuse_operations ops = {};
ops.getattr = fuse_getattr;
ops.readdir = fuse_readdir;
ops.open = fuse_open;
ops.read = fuse_read;
ops.write = fuse_write;
ops.release = fuse_release;
ops.create = fuse_create;
ops.unlink = fuse_unlink;
ops.mkdir = fuse_mkdir;
ops.rmdir = fuse_rmdir;
ops.rename = fuse_rename;
ops.truncate = fuse_truncate;
ops.init = fuse_init;
ops.destroy = fuse_destroy;

// Mount with options
const char* argv[] = {"via", mountPoint, "-f"};
```

### Key Methods

```cpp
// Static utility
static bool isFuseAvailable();

// Configuration
QString mountPoint() const;
void setMountPoint(const QString &path);
bool isMounted() const;

// Component access
FileCache* fileCache() const;
SyncDatabase* database() const;
GoogleDriveClient* driveClient() const;

// Mount/unmount
bool mount();
void unmount();

// Operations
void refreshMetadata();
void flushDirtyFiles();
```

### Signals

```cpp
void mounted();
void unmounted();
void mountError(const QString& error);
void fileAccessed(const QString& path);
void fileModified(const QString& path);
void metadataRefreshed();
void dirtyFilesFlushed(int count);
```

### Open File Handle Structure

```cpp
struct FuseOpenFile {
    QString fileId;     // Google Drive file ID
    QString path;       // Path in FUSE filesystem
    QString cachePath;  // Local cache path
    qint64 size;        // File size
    bool writable;      // Whether opened for writing
    bool dirty;         // Whether file has been modified
};
```

### File Attribute Mapping

```cpp
static int fuse_getattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    // Look up in fuse_metadata table
    FuseMetadata meta = database->getFuseMetadataByPath(path);

    if (meta.isFolder) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = meta.size;
    }

    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = meta.modifiedTime.toSecsSinceEpoch();
    stbuf->st_atime = meta.lastAccessed.toSecsSinceEpoch();
    stbuf->st_ctime = meta.createdTime.toSecsSinceEpoch();

    return 0;
}
```

## FileCache

### Responsibilities

- Cache downloaded files locally in `~/.cache/Via/files/`
- Manage cache size limits (default: 10GB)
- Handle LRU cache eviction (dirty files are never evicted)
- Track file freshness via `fuse_cache_entries` table
- Track dirty (modified) files via `fuse_dirty_files` table
- Coordinate downloads with GoogleDriveClient

### Cache Location

`~/.cache/Via/files/`

### Cache Structure

```
~/.cache/Via/
├── files/
│   ├── <hash_subdir>/        # 2-char hash prefix subdirectory
│   │   ├── <sha256_hash>     # File cached by SHA256 hash of fileId
│   │   └── ...
│   └── ...
```

**Note**: Cache paths use SHA256 hash of fileId to avoid filesystem issues with Google Drive IDs.
The first 2 characters of the hash are used as a subdirectory for better filesystem distribution.

### Key Methods

```cpp
// Cache Configuration
QString cacheDirectory() const;
void setCacheDirectory(const QString& path);
qint64 maxCacheSize() const;
void setMaxCacheSize(qint64 bytes);
qint64 currentCacheSize() const;

// Cache Operations (used by FuseDriver)
bool isCached(const QString& fileId) const;
QString getCachedPath(const QString& fileId, qint64 expectedSize = 0);  // May block for download
QString getCachePathForFile(const QString& fileId) const;  // No download
void updateAccessTime(const QString& fileId);
void invalidate(const QString& fileId);
void removeFromCache(const QString& fileId);
void clearCache();

// Dirty File Tracking (for DirtySyncWorker)
void markDirty(const QString& fileId, const QString& path);
void clearDirty(const QString& fileId);
void markUploadFailed(const QString& fileId);
bool isDirty(const QString& fileId) const;
QList<DirtyFileEntry> getDirtyFiles() const;

// Cache Management
bool evictToFreeSpace(qint64 bytesNeeded);
bool recordCacheEntry(const QString& fileId, const QString& localPath, qint64 size);
```

### Signals

```cpp
void downloadStarted(const QString& fileId);
void downloadCompleted(const QString& fileId, const QString& cachePath);
void downloadFailed(const QString& fileId, const QString& error);
void fileEvicted(const QString& fileId);
void cacheSizeChanged(qint64 newSize);
void fileDirty(const QString& fileId, const QString& path);
```

### Cache Eviction

LRU (Least Recently Used) eviction. Dirty files are **never** evicted:

```cpp
void evictLRU() {
    while (m_currentSize > m_maxSize) {
        // Find oldest non-dirty file
        QString oldestFileId = findOldestNonDirtyFile();
        if (oldestFileId.isEmpty()) {
            break;  // All files are dirty, cannot evict
        }
        removeFromCache(oldestFileId);
        emit fileEvicted(oldestFileId);
    }
}
```

### Freshness Check

Freshness is tracked via the MetadataRefreshWorker which polls the Google Drive changes API:

```cpp
// MetadataRefreshWorker checks for changes every 30s (configurable)
void processChanges(const QList<DriveChange>& changes) {
    for (const DriveChange& change : changes) {
        if (change.removed) {
            database->deleteFuseMetadata(change.fileId);
        } else {
            // File modified remotely - invalidate cache
            fileCache->invalidate(change.fileId);
            // Update metadata
            database->saveFuseMetadata(change.file);
        }
    }
}
```

## Background Workers

### DirtySyncWorker

Background thread that periodically uploads modified files:

- Polls `fuse_dirty_files` table every 5 seconds (configurable)
- Uploads dirty files via GoogleDriveClient
- Clears dirty flag on successful upload
- Marks upload_failed flag on failure for retry

### MetadataRefreshWorker

Background thread that polls for remote changes:

- Polls Google Drive changes API every 30 seconds (configurable)
- Uses change page token from `fuse_sync_state` table
- Updates `fuse_metadata` table with changes
- Invalidates cached files when remote content changes

## Threading

FUSE runs in a separate thread with background workers:

### Threading Architecture

- **FUSE Thread**: Handles FUSE callback operations (runs in FUSE context)
- **Main Thread**: Qt event loop processes API requests queued from FUSE thread
- **Dirty Sync Thread**: Background thread periodically uploads modified files
- **Metadata Refresh Thread**: Background thread polls for remote changes

```cpp
// FuseDriver creates and manages these threads
m_fuseThread = QThread::create([this]() {
    // FUSE event loop - blocks until unmount
    fuse_loop(m_fuse);
});
m_fuseThread->start();

// Background workers in separate threads
m_dirtySyncThread = new QThread();
m_dirtySyncWorker->moveToThread(m_dirtySyncThread);

m_metadataRefreshThread = new QThread();
m_metadataRefreshWorker->moveToThread(m_metadataRefreshThread);
```

### Thread Safety

- GoogleDriveClient operations are queued to main thread via signals/slots
- FileCache uses QMutex for concurrent access to cache state
- SyncDatabase operations are thread-safe via Qt's SQL connection handling
- Open file handles protected by m_openFilesMutex in FuseDriver

## Database Tables

FUSE mode uses dedicated tables in SyncDatabase (isolated from Mirror Sync):

| Table                | Purpose                               |
| -------------------- | ------------------------------------- |
| `fuse_metadata`      | File/folder hierarchy and attributes  |
| `fuse_cache_entries` | Cached file tracking for LRU eviction |
| `fuse_dirty_files`   | Modified files pending upload         |
| `fuse_sync_state`    | Change tokens and other sync state    |

See `src/sync/dataflow.md` for complete schema definitions.

## Error Handling

FUSE errors are returned as negative errno values:

```cpp
if (!file.isValid()) {
    return -ENOENT;  // No such file
}

if (!hasPermission) {
    return -EACCES;  // Permission denied
}

if (isDirectory) {
    return -EISDIR;  // Is a directory
}

if (!isEmpty) {
    return -ENOTEMPTY;  // Directory not empty
}

if (badFileHandle) {
    return -EBADF;  // Bad file descriptor
}

if (ioError) {
    return -EIO;  // I/O error
}
```

### API Error Handling

From the procedure flow chart, API errors are handled as follows:

| Error Type        | Action                           |
| ----------------- | -------------------------------- |
| Network Error     | Retry with exponential backoff   |
| Auth Error        | Refresh OAuth token, retry       |
| Not Found         | Return -ENOENT                   |
| Rate Limit        | Wait for rate limit reset, retry |
| Permission Denied | Return -EACCES                   |

## Limitations

1. **No offline support** - Requires network connection for file access
2. **Write latency** - Writes are cached locally and uploaded asynchronously
3. **Cache coherency** - May show stale data until MetadataRefreshWorker polls
4. **Large files** - Downloads block file open until complete
5. **Cache size** - Limited to configured max size (default 10GB)

## Key Differences from Mirror Sync Mode

1. **On-Demand Access**: Files are only downloaded when opened, not synced in advance
2. **No Local Watcher**: FUSE intercepts all file operations directly
3. **Cache Management**: LRU eviction manages disk space (dirty files never evicted)
4. **Metadata Cache**: File/folder structure cached separately from file contents
5. **Dirty File Tracking**: Modified files tracked in `fuse_dirty_files` and synced asynchronously
6. **No Conflict Resolution**: FUSE operations are immediate; conflicts prevented by synchronous design

## Performance Considerations

1. **Metadata Cache**: Critical for performance; all `getattr` and `readdir` must be fast
2. **Parallel Downloads**: Multiple files can download simultaneously
3. **Write Buffering**: Writes are buffered locally and synced in batches by DirtySyncWorker
4. **Cache Size Limit**: Default 10GB, configurable in Settings > Advanced > FUSE Cache Settings
5. **Polling Intervals**: Dirty sync (default 5s) and metadata refresh (default 30s) are configurable

## Usage

```cpp
// Enable FUSE in settings
settings.setValue("Fuse/enabled", true);
settings.setValue("Fuse/mountPoint", "/home/user/GoogleDrive");

// Mount on startup
if (settings.value("Fuse/enabled", false).toBool()) {
    fuseDriver.mount(settings.value("Fuse/mountPoint").toString());
}
```
