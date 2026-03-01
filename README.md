# Via

A Google Drive desktop client for Linux, providing seamless synchronization and virtual file system access to your Google Drive files.
Not affiliated with Google or Google Drive.

![Build Status](https://github.com/Mapy542/Via/actions/workflows/build.yml/badge.svg)

## Features

- **Bidirectional Synchronization**: Keep your local files in sync with Google Drive automatically
- **Virtual File System (FUSE)**: Access Google Drive files as if they were on your local disk without allocating local storage
- **Mirror Mode**: Optionally keep a full local copy of your Google Drive for offline access and performant read/write operations
- **On-Demand Access**: Files are downloaded only when you need them
- **Offline Access**: Cached FUSE files are available offline and sync back when you're online, and mirror mode allows full local copies
- **Conflict Resolution**: Automatic detection and handling of file conflicts
- **Native Filesystem Integration**: Works with file system for access in all applications or cli
- **Interface**: Easy to use GUI with system tray integration for quick access and notifications

## Installation

### AppImage (Recommended)

1. Download the latest AppImage from the [Releases](https://github.com/Mapy542/Via/releases) page
2. Place it in your desired location (e.g., `~/Applications`)
3. Make it executable: `chmod +x Via-*.AppImage`
4. Run: `./Via-*.AppImage` (Will install .desktop file)

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

## Other Information

- [Detailed User Guide](docs/USER_GUIDE.md)
- [Developer Guide](docs/DEVELOPER_GUIDE.md)

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
