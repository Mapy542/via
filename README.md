# Via

A Google Drive desktop client for Linux, providing seamless synchronization and virtual file system access to your Google Drive files.
Not affiliated with Google or Google Drive.

![Build Status](https://github.com/Mapy542/Via/actions/workflows/build.yml/badge.svg)

## Features

- **Bidirectional Synchronization**: Keep your local files in sync with Google Drive automatically
- **Virtual File System (FUSE)**: Access Google Drive files as if they were on your local disk without allocating local storage
- **Mirror Mode**: Optionally keep a full local copy of your Google Drive for offline access and performant read/write operations
- **On-Demand Access**: Files are downloaded only when you need them in Fuse sync
- **Shared Native Document Controls**: Choose how Google-native docs appear across mirror sync and FUSE from one setting
- **Offline Access**: Mirror sync keeps a full local copy when enabled, and already-cached FUSE files remain available offline
- **Conflict Resolution**: Automatic detection and handling of file conflicts
- **Native Filesystem Integration**: Works with file system for access in all applications or cli
- **Interface**: Easy to use GUI with system tray integration for quick access and notifications

## Installation

### AppImage (Recommended)

1. Download the latest AppImage from the [Releases](https://github.com/Mapy542/Via/releases) page
2. Place it in your desired location (e.g., `~/Applications`)
3. Make it executable: `chmod +x Via-*.AppImage`
4. Run: `./Via-*.AppImage` (Will install .desktop file)

The appIamge will automatically prompt and then update itself to the latest release on this repo.

### Debian Package

When release packaging is enabled for your target version, Releases also include `.deb` assets for `amd64` and `arm64`.

1. Download the matching `via_<version>-1_<arch>.deb` asset
2. Install it with `sudo apt install ./via_<version>-1_<arch>.deb`

Note the debian package does not automatically update to the latest since it's not registered upstream with Debian repos.

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
    libfuse3-dev libsecret-1-dev libdbus-1-dev qtkeychain-qt6-dev
```

#### Building

```bash
git clone https://github.com/Mapy542/Via.git
cd Via
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

#### Building a Local Debian Package

There is a 1 click script to build the debian package locally:

```bash
sudo apt-get install build-essential cmake debhelper desktop-file-utils \
    dpkg-dev lintian pkgconf qt6-base-dev qt6-networkauth-dev \
    libqt6sql6-sqlite libfuse3-dev libgl1-mesa-dev qtkeychain-qt6-dev

./scripts/make-deb.sh
```

Artifacts are written to `build-deb/artifacts/` as `.deb`, `.changes`, and `.buildinfo` files.

## Configuration

### Google API Credentials (BYOK)

To use Via, you need to set up Google API credentials:

1. Go to the [Google Cloud Console](https://console.cloud.google.com/)
2. Create a new project or select an existing one
3. Enable the Google Drive API
4. Create OAuth 2.0 credentials (Desktop application)
5. Copy the Client ID and Client Secret from the created credential
6. In Via settings, enter that Client ID and Client Secret

Since the software runs on your client, there is no way to distribute a common key securely. You bring your own key for this program to work.
(For quick testing you may be able to borrow the API keys stored in Gnome G-Drive connector, or KIO-GDrive)

OAuth credentials and tokens are stored in the system keyring when one is available. If no supported keyring backend is available, Via falls back to a local secure store under `~/.local/share/Via/`.

### Settings

Access settings through the main window or system tray menu:

- **Login**: Supply client credentials, sign in or out, and view storage usage.
- **Mirror**: Configure the sync folder, sync mode, conflict resolution, and mirror performance.
- **Common**: Configure duplicate-name handling and shared native-document representation for mirror sync and FUSE.
- **Fuse**: Enable FUSE sync, set the mount point, and manage cache behavior.
- **Misc**: Configure startup, appearance, notifications, runtime pause behavior, and debug logging.

  Note you may run mirror sync and FUSE together, or enable only one of them. Only 1 account is supported at a time.

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

Enable debug logging in Settings > Misc > Debug > Enable debug logging

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
