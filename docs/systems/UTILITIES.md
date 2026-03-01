# Utility Systems

## Overview

The utilities (`src/utils/`) provide cross-cutting functionality used by multiple components including notifications, bandwidth management, offline access, and logging.

## Files

| File                      | Purpose                     |
| ------------------------- | --------------------------- |
| `NotificationManager.h`   | Desktop notifications       |
| `NotificationManager.cpp` | Notification implementation |
| `BandwidthManager.h`      | Upload/download throttling  |
| `BandwidthManager.cpp`    | Throttling implementation   |
| `OfflineManager.h`        | Offline file access         |
| `OfflineManager.cpp`      | Offline implementation      |
| `LogManager.h`            | File-based logging          |
| `LogManager.cpp`          | Logging implementation      |

---

## NotificationManager

### Responsibilities

- Display desktop notifications
- Support multiple notification backends
- Throttle notification frequency

### Notification Backends

1. **D-Bus** (Linux) - Primary, uses org.freedesktop.Notifications
2. **System Tray** - Fallback using QSystemTrayIcon

### Key Methods

```cpp
// Show a notification
void showNotification(const QString &title, const QString &message,
                      NotificationType type = Info);

// Check if notifications are available
bool isAvailable() const;

// Enable/disable notifications
void setEnabled(bool enabled);
```

### Notification Types

```cpp
enum NotificationType {
    Info,       // General information
    Success,    // Operation completed
    Warning,    // Warning message
    Error       // Error message
};
```

### Throttling

Prevents notification spam:

```cpp
void showNotification(...) {
    if (m_recentNotifications.contains(title + message)) {
        return;  // Skip duplicate
    }

    m_recentNotifications.insert(title + message);
    QTimer::singleShot(5000, [=]() {
        m_recentNotifications.remove(title + message);
    });

    // Show notification...
}
```

---

## BandwidthManager

### Responsibilities

- Limit upload/download speeds
- Track bandwidth usage
- Provide usage statistics

### Key Methods

```cpp
// Set limits (0 = unlimited)
void setUploadLimit(qint64 bytesPerSecond);
void setDownloadLimit(qint64 bytesPerSecond);

// Get current limits
qint64 uploadLimit() const;
qint64 downloadLimit() const;

// Get usage statistics
qint64 bytesUploaded() const;
qint64 bytesDownloaded() const;

// Throttle a transfer
void throttleUpload(qint64 bytes);
void throttleDownload(qint64 bytes);
```

### Throttling Implementation

Token bucket algorithm:

```cpp
void throttleUpload(qint64 bytes) {
    if (m_uploadLimit == 0) return;  // Unlimited

    qint64 tokensNeeded = bytes;
    while (tokensNeeded > m_uploadTokens) {
        // Wait for tokens to refill
        QThread::msleep(100);
        refillTokens();
    }
    m_uploadTokens -= tokensNeeded;
}

void refillTokens() {
    qint64 elapsed = m_lastRefill.msecsTo(QDateTime::currentDateTime());
    qint64 newTokens = (m_uploadLimit * elapsed) / 1000;
    m_uploadTokens = qMin(m_uploadTokens + newTokens, m_uploadLimit);
    m_lastRefill = QDateTime::currentDateTime();
}
```

---

## OfflineManager

### Responsibilities

- Track files marked for offline access
- Ensure offline files are always cached
- Handle offline mode

### Key Methods

```cpp
// Mark file for offline access
void setOfflineAvailable(const QString &relativePath, bool offline);

// Check if file is marked offline
bool isOfflineAvailable(const QString &relativePath) const;

// Get all offline files
QStringList offlineFiles() const;

// Check if we're in offline mode
bool isOfflineMode() const;

// Set offline mode
void setOfflineMode(bool offline);
```

### Storage

Offline file list stored in database:

```sql
CREATE TABLE offline_files (
    id INTEGER PRIMARY KEY,
    relative_path TEXT UNIQUE NOT NULL,
    last_sync TEXT
);
```

### Offline Mode Behavior

When offline:

- Local changes queued for later sync
- Remote changes cannot be fetched
- Cached files available normally
- UI shows offline indicator

---

## LogManager

### Responsibilities

- Capture all Qt log output
- Write to timestamped log files
- Rotate logs when size limit reached
- Clean up old log files

### Log Location

`~/.local/share/Via/logs/`

### Log File Naming

`via_YYYY-MM-DD_HH-mm-ss.log`

### Log Format

```
[2026-01-03 12:30:45.123] [INFO] [SyncEngine.cpp:456] Starting initial sync...
[2026-01-03 12:30:45.234] [DEBUG] [SyncEngine.cpp:478] Found 42 remote files
[2026-01-03 12:30:46.345] [WARN] [SyncEngine.cpp:512] File conflict detected
[2026-01-03 12:30:47.456] [ERROR] [GoogleDriveClient.cpp:234] API error: 429
```

### Key Methods

```cpp
// Initialize logging (call once at startup)
static void initialize();

// Shutdown logging (call before exit)
static void shutdown();

// Get current log file path
static QString currentLogFile();
```

### Qt Integration

Custom message handler:

```cpp
void customMessageHandler(QtMsgType type, const QMessageLogContext &context,
                          const QString &msg) {
    QString level;
    switch (type) {
        case QtDebugMsg:    level = "DEBUG"; break;
        case QtInfoMsg:     level = "INFO"; break;
        case QtWarningMsg:  level = "WARN"; break;
        case QtCriticalMsg: level = "ERROR"; break;
        case QtFatalMsg:    level = "FATAL"; break;
    }

    QString timestamp = QDateTime::currentDateTime()
        .toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString location = QString("%1:%2").arg(context.file).arg(context.line);

    QString logLine = QString("[%1] [%2] [%3] %4\n")
        .arg(timestamp, level, location, msg);

    // Write to file and console
    LogManager::instance()->write(logLine);
    fprintf(stderr, "%s", logLine.toLocal8Bit().constData());
}
```

### Log Rotation

```cpp
void checkRotation() {
    QFileInfo info(m_currentLogFile);
    if (info.size() > MAX_LOG_SIZE) {  // 10MB
        m_logFile.close();
        createNewLogFile();
    }
}

void cleanupOldLogs() {
    QDir logDir(m_logPath);
    QStringList logs = logDir.entryList({"via_*.log"},
                                         QDir::Files, QDir::Time);

    while (logs.size() > MAX_LOG_FILES) {  // Keep 5
        QFile::remove(logDir.filePath(logs.takeLast()));
    }
}
```

### Configuration

```cpp
// Log level can be controlled via environment
// QT_LOGGING_RULES="*.debug=false;Via.*=true"

// Or programmatically
QLoggingCategory::setFilterRules("*.debug=true");
```
