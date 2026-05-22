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
2. Go to the **Login** tab
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

You can change this location in **Settings > Mirror > Sync Folder**.

## Features

### Automatic Synchronization

Once signed in and with mirror sync enabled, Via automatically:

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

- **Open Google Drive Folder** - Opens your mirror sync folder
- **Pause Sync** / **Resume Sync** - Temporarily stop/start syncing
- **Sync Now** - Trigger an immediate mirror sync pass
- **Recent Changes** - View recent file activity
- **Recent Notifications** - Review recent desktop notifications from the tray
- **Open Via** - Show the main window
- **Settings** - Open the settings window
- **Quit** - Exit Via

### Mirror Sync

Mirror sync maintains a local working folder that Via keeps aligned with Google Drive.

#### Configuring Mirror Sync

1. Go to **Settings > Mirror**
2. Check **"Enable mirror sync"**
3. Set the **Sync Folder**
4. Choose a **Sync Mode**
5. Choose a **Conflict Resolution** strategy
6. Optionally adjust **Performance** settings for dormant time and duty cycle

| Sync Mode              | Description                                                        |
| ---------------------- | ------------------------------------------------------------------ |
| **Keep Newest**        | Bidirectional sync using modification times to decide the winner   |
| **Remote Read-Only**   | Download changes from Drive, but never upload local changes        |
| **Remote No Delete**   | Bidirectional sync without propagating local deletions to Drive    |

### Virtual File System (FUSE)

FUSE lets you browse all your Google Drive files without downloading them all:

#### Enabling FUSE

1. Go to **Settings > Fuse**
2. Check **"Enable FUSE sync"**
3. Set the mount point in the **Mount Point** group (default: `~/GoogleDriveFuse`)
4. Optionally adjust the cache target in **Cache and Maintenance**
5. Click **Apply**

#### Using FUSE

Navigate to the mount point in your file manager. You'll see all your Google Drive files and folders.

- **Files appear instantly** but aren't downloaded yet
- **Opening a file** downloads it on-demand
- **Editing files** uploads changes automatically
- **Cached files** load faster on repeat access

#### Google-Native Documents

Google Docs, Sheets, Slides, and other native Google documents don't have downloadable file content. Via offers several options for how they appear locally in both the mirror sync folder and the FUSE view:

1. Go to **Settings > Common**
2. Open the **Native Documents** group
3. Select a mode:

| Mode                       | Description                                            |
| -------------------------- | ------------------------------------------------------ |
| **Hide** (default)         | Native docs are not materialized locally               |
| **Browser shortcuts**      | Appear as `.gdoc`, `.gsheet`, `.gslides` stub files    |
| **OpenDocument snapshots** | Exported as `.odt`, `.ods`, `.odp` read-only snapshots |
| **Text snapshots**         | Exported as `.md`, `.csv`, `.txt` read-only snapshots  |

**Important notes:**

- This shared setting applies to both mirror sync and FUSE
- Via registers custom MIME types for browser shortcuts on startup so file managers can open them through Via
- Changing this setting may require a restart of Via; enabled sync systems rebuild their local native-document artifacts on next launch
- If both mirror sync and FUSE are disabled, Via stores the choice and applies it the next time you enable one
- Native doc files are always **read-only** in all modes
- Export snapshots are limited to 10 MB by Google (a tray notification appears if export fails)
- Some native types (Forms, Scripts, Sites) are only available in browser-shortcut mode

### Offline Behavior

- Mirror sync keeps files in your local sync folder, so they remain available offline.
- FUSE can reopen files that are already cached locally, but Via does not provide a separate offline-manager workflow.
- Via can automatically pause network sync while offline, on metered networks, or in power saver mode. Configure this in **Settings > Misc > Runtime Pause**.

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

### Notifications

Via keeps you informed with desktop notifications:

- Sync conflicts detected
- Errors encountered

To disable notifications:

1. Go to **Settings > Misc**
2. Uncheck **"Show desktop notifications"**

## Settings Reference

### Login Tab

- **Google API Credentials** - Enter your OAuth Client ID and Secret from Google Cloud Console
- **Sign In / Sign Out** - Manage your Google account connection
- **Storage Usage** - View your Google Drive storage

### Mirror Tab

- **Sync Folder** - Where Google Drive files are stored locally
- **Sync Mode** - Choose between Keep Newest, Remote Read-Only, and Remote No Delete
- **Conflict Resolution** - Choose how Via resolves simultaneous local and remote edits
- **Performance** - Tune mirror dormant time and duty cycle

### Common Tab

- **Duplicate File Endings** - Choose how Via renames duplicate Drive files locally
- **Native Documents** - Choose how Google-native documents are materialized for mirror sync and FUSE

### Fuse Tab

- **Enable FUSE Sync** - Turn the virtual file system on or off
- **Mount Point** - Choose where the FUSE view is mounted
- **Cache and Maintenance** - Set the evictable cache target and clear cached FUSE files on restart

### Misc Tab

- **Start on Login** - Launch Via when you log in
- **Appearance** - Choose the tray icon theme override
- **Notifications** - Enable/disable desktop notifications
- **Runtime Pause** - Turn off automatic pausing for offline, metered-network, and power-saver conditions when you want Via to keep syncing anyway
- **Debug** - Enable detailed logging (`~/.local/share/Via/logs/`)

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
- File matches an ignored pattern or the current native-document representation mode hides it
- Network issues

**Solutions:**

1. Check if sync is paused (resume if needed)
2. Click "Sync Now" to force sync
3. Check the current native-document mode if the missing file is a Google Doc, Sheet, Slide, or other native type
4. Check the activity log for errors

### FUSE Not Working

**Causes:**

- FUSE not installed
- Permission issues
- Mount point already in use

**Solutions:**

1. Install FUSE: `sudo apt-get install fuse3`
2. Add user to fuse group: `sudo usermod -a -G fuse $USER`
3. Ensure mount point directory is empty

### Unable to Mount FUSE

**Causes:**

- FUSE not installed
- User not in fuse group
- Mount point already exists and is not empty

**Solutions:**

1. Install FUSE: `sudo apt-get install fuse3`
2. Add user to fuse group: `sudo usermod -a -G fuse $USER`
3. Ensure the mount point directory (default `~/GoogleDriveFuse`) is empty or does not exist before enabling FUSE in settings
4. `fusermount3 -u ~/GoogleDriveFuse` to unmount if it's stuck

### Conflicts Keep Occurring

**Causes:**

- Editing same file on multiple devices
- Poor sync timing
- Database issues

**Solutions:**

1. Wait for sync to complete before editing
2. Use "Keep Both" to preserve all changes
3. Enable notifications to know when sync completes

or

1. Stop Via
2. Backup and delete the sync database (`~/.local/share/Via/sync.db`)
3. Clear the sync folder (after backing up important files)
4. Restart Via and let it resync everything

## Keyboard Shortcuts

| Shortcut | Action           |
| -------- | ---------------- |
| `Ctrl+Q` | Quit application |
| `Ctrl+,` | Open settings    |

## Support

### Getting Help

1. Check this user guide for common questions
2. File an issue on [GitHub](https://github.com/Mapy542/Via/issues)

### Debug Logs

To help troubleshoot issues:

1. Enable debug mode in **Settings > Misc > Debug**
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
| `~/.config/Via/`      | Application settings                            |
| `~/.local/share/Via/` | Sync database, logs, secure token fallback file |
| `~/.cache/Via/`       | FUSE cache, temporary data                      |
| `~/GoogleDrive/`      | Your synced files (configurable)                |
| `~/GoogleDriveFuse/`  | FUSE mount point (configurable)                 |

### Settings File

Settings are stored in `~/.config/Via/Via.conf` using the INI format.

### Security Note

OAuth credentials and tokens are stored in the system keyring when available. If no supported keyring backend is available, Via falls back to a 0600-permissioned file at `~/.local/share/Via/secure_tokens.json`.
