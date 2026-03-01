# Via

A Google Drive desktop client for Linux, providing seamless synchronization and virtual file system access to your Google Drive files.

![Build Status](https://github.com/Mapy542/Via/actions/workflows/build.yml/badge.svg)

## Features

- **Bidirectional Synchronization**: Keep your local files in sync with Google Drive automatically
- **Virtual File System (FUSE)**: Access Google Drive files as if they were on your local disk
- **On-Demand Access**: Files are downloaded only when you need them
- **Offline Access**: Mark files for offline access to use without internet
- **Conflict Resolution**: Automatic detection and handling of file conflicts
- **Bandwidth Management**: Control upload/download speeds with scheduling
- **System Tray Integration**: Quick access to sync status and common actions
- **Desktop Notifications**: Stay informed about sync activity

## Installation

### AppImage (Recommended)

1. Download the latest AppImage from the [Releases](https://github.com/Mapy542/Via/releases) page
2. Make it executable: `chmod +x Via-*.AppImage`
3. Run: `./Via-*.AppImage`

### Building from Source

#### Prerequisites

- CMake 3.20+
- C++20 compatible compiler (GCC 10+, Clang 10+)
- Qt 6 (Core, Gui, Widgets, Network, NetworkAuth, Sql, DBus, Concurrent)
- FUSE 3
- pkg-config

#### Ubuntu/Debian

```bash
sudo apt-get install build-essential cmake pkg-config \
    qt6-base-dev qt6-networkauth-dev libqt6sql6-sqlite \
    libfuse3-dev libsecret-1-dev libdbus-1-dev
```

#### Building

```bash
git clone https://github.com/Mapy542/Via.git
cd Via
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Configuration

### Google API Credentials

To use Via, you need to set up Google API credentials:

1. Go to the [Google Cloud Console](https://console.cloud.google.com/)
2. Create a new project or select an existing one
3. Enable the Google Drive API
4. Create OAuth 2.0 credentials (Desktop application)
5. Download the credentials JSON file
6. In Via settings, enter your Client ID and Client Secret

### Settings

Access settings through the main window or system tray menu:

- **Sync Folder**: Choose where to sync your Google Drive files
- **Selective Sync**: Choose which folders to sync
- **Bandwidth Limits**: Control upload/download speeds
- **FUSE Mount**: Enable virtual file system access
- **Notifications**: Configure desktop notifications
- **Startup**: Set to start on system login

## Usage

### Basic Usage

1. Launch Via
2. Sign in with your Google account
3. Your Google Drive files will sync to `~/GoogleDrive`

### System Tray

The system tray icon provides quick access to:

- Open Google Drive folder
- Pause/Resume sync
- Sync now
- Recent changes
- Settings
- Quit

### Virtual File System (FUSE)

When FUSE is enabled, your Google Drive is mounted at `~/GoogleDriveFuse`:

- Files appear instantly but are downloaded on-demand
- Changes are synced back to Google Drive
- Files are cached locally for faster access

### Offline Access

To make files available offline:

1. Right-click the file in your file manager
2. Select "Available Offline" (if integrated)
3. Or use the Via settings to manage offline files

## Architecture

```
Via/
├── src/
│   ├── main.cpp              # Application entry point
│   ├── ui/                   # User interface components
│   │   ├── MainWindow        # Main application window
│   │   ├── SettingsWindow    # Settings dialog
│   │   ├── SystemTrayManager # System tray icon and menu
│   │   └── ConflictDialog    # Conflict resolution UI
│   ├── auth/                 # Authentication
│   │   ├── GoogleAuthManager # OAuth 2.0 flow
│   │   └── TokenStorage      # Secure token storage
│   ├── api/                  # Google Drive API client
│   │   ├── GoogleDriveClient # REST API wrapper
│   │   ├── DriveFile         # File metadata structure
│   │   └── DriveChange       # Change notification structure
│   ├── sync/                 # Synchronization engine
│   │   ├── SyncEngine        # Main sync logic
│   │   ├── SyncDatabase      # SQLite state tracking
│   │   ├── FileWatcher       # Local file monitoring
│   │   ├── SyncQueue         # Operation queue
│   │   └── ConflictResolver  # Conflict handling
│   ├── fuse/                 # Virtual file system
│   │   ├── FuseDriver        # FUSE implementation
│   │   └── FileCache         # Local file cache
│   └── utils/                # Utilities
│       ├── NotificationManager
│       ├── OfflineManager
│       └── BandwidthManager
├── res/                      # Resources
├── docs/                     # Documentation
└── .github/workflows/        # CI/CD
```

## Troubleshooting

### Common Issues

**"FUSE not available"**

- Install FUSE 3: `sudo apt-get install fuse3`
- Add your user to the fuse group: `sudo usermod -a -G fuse $USER`
- Log out and back in

**"Authentication failed"**

- Check your internet connection
- Verify your Google API credentials
- Try signing out and back in

**"Sync conflicts"**

- Check the Conflicts window for details
- Choose which version to keep
- Consider using "Keep Both" for important files

### Debug Logging

Enable debug logging in Settings > Advanced > Enable debug logging

Logs are stored in: `~/.local/share/Via/logs/`

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Qt Framework
- FUSE (Filesystem in Userspace)
- Google Drive API
