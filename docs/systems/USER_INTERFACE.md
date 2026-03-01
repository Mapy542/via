# User Interface System

## Overview

The UI system (`src/ui/`) provides a Qt Widgets-based graphical interface with system tray integration. It includes the main window, settings dialog, system tray management, and conflict resolution dialog.

## Files

| File                    | Purpose                    |
| ----------------------- | -------------------------- |
| `MainWindow.h`          | Main application window    |
| `MainWindow.cpp`        | Main UI implementation     |
| `SettingsWindow.h`      | Settings dialog            |
| `SettingsWindow.cpp`    | Settings UI implementation |
| `SystemTrayManager.h`   | System tray icon and menu  |
| `SystemTrayManager.cpp` | Tray implementation        |
| `ConflictDialog.h`      | Conflict resolution dialog |
| `ConflictDialog.cpp`    | Conflict UI implementation |

## MainWindow

### Layout

```
+------------------------------------------+
|  Via                             [_][X]|
+------------------------------------------+
|  Status: Synced ✓                        |
|                                          |
|  [Sync Now]  [Pause]  [Settings]         |
|                                          |
|  Recent Activity:                        |
|  ├─ Uploaded: document.pdf               |
|  ├─ Downloaded: image.jpg                |
|  └─ Conflict: report.docx                |
+------------------------------------------+
```

### Key Methods

```cpp
// Update status display
void updateStatus(const QString &status);

// Add activity to log
void addActivity(const QString &message);

// Handle authentication state
void onAuthenticated();
void onLoggedOut();

// Handle sync state
void onSyncStarted();
void onSyncCompleted();
void onFileUploaded(const QString &fileName);
void onFileDownloaded(const QString &fileName);
```

### Signal Connections

```cpp
// To AuthManager
connect(signInButton, &QPushButton::clicked,
        authManager, &GoogleAuthManager::authenticate);
connect(signOutButton, &QPushButton::clicked,
        authManager, &GoogleAuthManager::logout);

// To SyncEngine
connect(syncButton, &QPushButton::clicked,
        syncEngine, &SyncEngine::syncNow);
connect(pauseButton, &QPushButton::clicked,
        syncEngine, &SyncEngine::pause);

// From SyncEngine
connect(syncEngine, &SyncEngine::statusChanged,
        this, &MainWindow::updateStatus);
connect(syncEngine, &SyncEngine::fileUploaded,
        this, &MainWindow::onFileUploaded);
```

## SettingsWindow

### Tabs

#### Account Tab

- OAuth client ID input
- OAuth client secret input
- "Save API Credentials" button
- Sign in/Sign out buttons
- Link to Google Cloud Console

#### Sync Tab

- Sync folder path with browse button
- Sync mode dropdown:
    - Keep Newest (bidirectional)
    - Remote Read-Only
    - Remote No Delete
- Conflict resolution dropdown:
    - Keep both versions
    - Always keep local
    - Always keep remote
    - Keep newest
    - Ask me each time

#### Notifications Tab

- Enable/disable notifications checkbox
- Notification types:
    - Upload complete
    - Download complete
    - Sync errors
    - Conflicts

#### Advanced Tab

- Upload bandwidth limit
- Download bandwidth limit
- Log file location (read-only)
- Clear cache button

### Settings Storage

Uses QSettings with organization/app name:

```cpp
QSettings settings("Via", "Via");
// Stored in: ~/.config/Via/Via.conf
```

### Key Methods

```cpp
void loadSettings();
void saveSettings();
void onSyncFolderBrowse();
void onSaveCredentials();
```

## SystemTrayManager

### Tray Menu

```
┌─────────────────────┐
│ Via                 │
├─────────────────────┤
│ Status: Synced      │
├─────────────────────┤
│ ○ Sync Now          │
│ ○ Pause Sync        │
│ ○ Open Folder       │
├─────────────────────┤
│ ○ Settings...       │
├─────────────────────┤
│ ○ Quit              │
└─────────────────────┘
```

### Icons

Status-based icons:

- `sync-idle.png` - Idle/synced
- `sync-syncing.png` - Sync in progress (animated)
- `sync-error.png` - Error state
- `sync-paused.png` - Paused

### Key Methods

```cpp
void setStatus(const QString &status);
void showNotification(const QString &title, const QString &message);
void updateMenu();
```

### Minimize to Tray

```cpp
void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_trayManager->isVisible()) {
        hide();
        event->ignore();
    } else {
        event->accept();
    }
}
```

## ConflictDialog

### Layout

```
+------------------------------------------+
|  Conflict Detected                       |
+------------------------------------------+
|  File: report.docx                       |
|                                          |
|  Local version:                          |
|    Modified: Jan 3, 2026 10:30 AM        |
|    Size: 45.2 KB                         |
|                                          |
|  Remote version:                         |
|    Modified: Jan 3, 2026 11:15 AM        |
|    Size: 46.8 KB                         |
|                                          |
|  [Keep Local] [Keep Remote] [Keep Both]  |
|                                          |
|  □ Remember my choice                    |
+------------------------------------------+
```

### Return Values

```cpp
enum Resolution {
    KeepLocal,
    KeepRemote,
    KeepBoth,
    Cancel
};

Resolution resolution() const;
bool rememberChoice() const;
```

## Resources

Located in `res/`:

- `icons/` - Application and status icons
- `via.desktop` - Desktop entry file

### Desktop Entry

```ini
[Desktop Entry]
Name=Via
Comment=Google Drive client for Linux
Exec=via
Icon=via
Type=Application
Categories=Network;FileTransfer;
```
