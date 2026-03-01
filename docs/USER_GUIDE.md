# User Guide

Welcome to Via, a Google Drive desktop client for Linux!

## Getting Started

### First Launch

1. **Launch Via** from your application menu or command line
2. The main window will appear, prompting you to set up API credentials

### Setting Up Google API Credentials

Before you can use Via, you need to create Google API credentials:

1. Go to the [Google Cloud Console](https://console.cloud.google.com/)
2. Create a new project or select an existing one
3. Navigate to **APIs & Services > Library**
4. Search for "Google Drive API" and enable it
5. Go to **APIs & Services > Credentials**
6. Click **Create Credentials > OAuth client ID**
7. Select **Desktop app** as the application type
8. Give it a name (e.g., "Via")
9. Click **Create**
10. Copy your **Client ID** and **Client Secret**

Now enter these credentials in Via:

1. Open **Settings** (or it will open automatically on first launch)
2. Go to the **Account** tab
3. Enter your **Client ID** in the first field
4. Enter your **Client Secret** in the second field
5. Click **Save API Credentials**

### Signing In

1. Click **"Sign In with Google"**
2. A browser window will open for Google authentication
3. Sign in with your Google account
4. Grant Via permission to access your Google Drive
5. Return to Via - you should see "Signed in to Google Drive"

### Your Sync Folder

By default, your Google Drive files sync to:

```
~/GoogleDrive
```

You can change this location in **Settings > Sync > Sync Folder**.

## Features

### Automatic Synchronization

Once signed in, Via automatically:

- **Downloads** new and modified files from Google Drive
- **Uploads** changes you make to local files
- **Syncs** new files you add to the sync folder
- **Removes** files you delete (after confirmation)

The sync status is shown in the main window and system tray.

### System Tray

Via runs in your system tray for quick access:

| Action           | Description      |
| ---------------- | ---------------- |
| **Single click** | Open main window |
| **Right-click**  | Show menu        |

#### Tray Menu Options

- **Open Google Drive Folder** - Opens your sync folder
- **Pause Sync** / **Resume Sync** - Temporarily stop/start syncing
- **Sync Now** - Trigger immediate sync
- **Recent Changes** - View recent file activity
- **Settings** - Open settings window
- **Quit** - Exit Via

### Selective Sync

Don't want to sync everything? Use selective sync:

1. Go to **Settings > Sync**
2. Click **Refresh** to load your folders
3. Uncheck folders you don't want to sync locally
4. Click **Apply**

Only checked folders will sync to your computer.

### Virtual File System (FUSE)

FUSE lets you browse all your Google Drive files without downloading them all:

#### Enabling FUSE

1. Go to **Settings > Advanced**
2. Check **"Enable FUSE virtual file system"**
3. Set the mount point (default: `~/GoogleDriveFuse`)
4. Click **Apply**

#### Using FUSE

Navigate to the mount point in your file manager. You'll see all your Google Drive files and folders.

- **Files appear instantly** but aren't downloaded yet
- **Opening a file** downloads it on-demand
- **Editing files** uploads changes automatically
- **Cached files** load faster on repeat access

### Offline Access

Make files available without internet:

#### Marking Files for Offline

1. In the Settings > Advanced, configure offline files
2. Or through the application's offline manager

#### Benefits

- Access important files without internet
- Files stay synced when you're back online
- Choose which files to pre-download

### Conflict Resolution

When the same file is modified in two places:

1. Via detects the conflict
2. A notification appears
3. Open the **Conflicts** window to resolve

#### Resolution Options

| Option          | Result                                      |
| --------------- | ------------------------------------------- |
| **Keep Local**  | Your local changes overwrite Google Drive   |
| **Keep Remote** | Google Drive version overwrites local       |
| **Keep Both**   | Local file renamed with "(conflict)" suffix |

### Bandwidth Management

Control how much bandwidth Via uses:

1. Go to **Settings > Bandwidth**
2. Enable limits for uploads and/or downloads
3. Set the speed in KB/s
4. Optionally set a schedule

#### Schedule Options

- **Always** - Limit applies all the time
- **9 AM - 5 PM** - Limit during work hours only
- **5 PM - 9 AM** - Limit during off-hours only
- **Weekends only** - Limit on Saturday and Sunday

### Notifications

Via keeps you informed with desktop notifications:

- File uploads completed
- File downloads completed
- Sync conflicts detected
- Errors encountered

To disable notifications:

1. Go to **Settings > Advanced**
2. Uncheck **"Show desktop notifications"**

## Settings Reference

### Account Tab

- **Google API Credentials** - Enter your OAuth Client ID and Secret from Google Cloud Console
- **Sign In / Sign Out** - Manage your Google account connection
- **Storage Usage** - View your Google Drive storage

### Sync Tab

- **Sync Folder** - Where Google Drive files are stored locally
- **Selective Sync** - Choose which folders to sync

### Bandwidth Tab

- **Upload Limit** - Maximum upload speed
- **Download Limit** - Maximum download speed
- **Schedule** - When limits apply

### Advanced Tab

- **Start on Login** - Launch Via when you log in
- **Notifications** - Enable/disable desktop notifications
- **FUSE** - Enable virtual file system
- **Cache Size** - Maximum local cache size
- **Debug Mode** - Enable detailed logging

## Troubleshooting

### "Not connected" Status

**Causes:**

- Not signed in
- No internet connection
- Authentication expired

**Solutions:**

1. Check your internet connection
2. Try signing out and back in
3. Check if Google services are accessible

### Files Not Syncing

**Causes:**

- Sync is paused
- File is in an ignored pattern
- Network issues

**Solutions:**

1. Check if sync is paused (resume if needed)
2. Click "Sync Now" to force sync
3. Check the activity log for errors

### FUSE Not Working

**Causes:**

- FUSE not installed
- Permission issues
- Mount point already in use

**Solutions:**

1. Install FUSE: `sudo apt-get install fuse3`
2. Add user to fuse group: `sudo usermod -a -G fuse $USER`
3. Ensure mount point directory is empty

### Slow Performance

**Causes:**

- Large number of files syncing
- Bandwidth limits too restrictive
- Cache full

**Solutions:**

1. Wait for initial sync to complete
2. Adjust bandwidth limits
3. Increase cache size in settings
4. Use selective sync to reduce file count

### Conflicts Keep Occurring

**Causes:**

- Editing same file on multiple devices
- Poor sync timing

**Solutions:**

1. Wait for sync to complete before editing
2. Use "Keep Both" to preserve all changes
3. Enable notifications to know when sync completes

## Keyboard Shortcuts

| Shortcut | Action           |
| -------- | ---------------- |
| `Ctrl+Q` | Quit application |
| `Ctrl+,` | Open settings    |

## Support

### Getting Help

1. Check this user guide
2. Review the [API documentation](API.md)
3. File an issue on [GitHub](https://github.com/Mapy542/Via/issues)

### Debug Logs

To help troubleshoot issues:

1. Enable debug mode in Settings > Advanced
2. Reproduce the issue
3. Find logs in `~/.local/share/Via/logs/`
4. Include relevant log excerpts in your issue report

## Uninstalling

### AppImage

Simply delete the AppImage file.

### Clean Up Data

To remove all Via data:

```bash
rm -rf ~/.local/share/Via
rm -rf ~/.config/Via
rm -rf ~/.cache/Via
```

**Note:** This does not delete your synced files in `~/GoogleDrive`.

## Data Storage Locations

Via stores data in standard XDG directories:

| Location              | Contents                                        |
| --------------------- | ----------------------------------------------- |
| `~/.config/Via/`      | Application settings, API credentials (encoded) |
| `~/.local/share/Via/` | Sync database, logs                             |
| `~/.cache/Via/`       | File cache, temporary data                      |
| `~/GoogleDrive/`      | Your synced files (configurable)                |
| `~/GoogleDriveFuse/`  | FUSE mount point (configurable)                 |

### Settings File

Settings are stored in `~/.config/Via/Via.conf` using the INI format.

### Security Note

OAuth credentials and tokens are stored with basic obfuscation. For maximum security, ensure proper file permissions on your home directory.
