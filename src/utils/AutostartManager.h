/**
 * @file AutostartManager.h
 * @brief Manages XDG desktop integration and autostart for Via
 *
 * Handles:
 * - Installing the .desktop file to ~/.local/share/applications/
 * - Installing the app icon to ~/.local/share/icons/hicolor/scalable/apps/
 * - Writing/removing ~/.config/autostart/via.desktop for login autostart
 * - Syncing autostart state with QSettings on startup
 */

#ifndef AUTOSTARTMANAGER_H
#define AUTOSTARTMANAGER_H

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>

/**
 * @class AutostartManager
 * @brief Static utility for managing XDG desktop integration and autostart
 */
class AutostartManager {
   public:
    /**
     * @brief Install desktop integration files on first run
     *
     * Installs the .desktop file to ~/.local/share/applications/ and
     * the app icon to ~/.local/share/icons/hicolor/scalable/apps/.
     * Also syncs the autostart entry with the current QSettings value.
     *
     * Safe to call every startup — only writes files if they are missing
     * or the Exec= path has changed (e.g. user moved the AppImage).
     */
    static void installDesktopIntegration() {
        QString execPath = resolveExecPath();

        installDesktopFile(execPath);
        installIcon();
        syncAutostart();
    }

    /**
     * @brief Enable or disable autostart
     * @param enabled true to install the autostart entry, false to remove it
     * @return true on success
     */
    static bool setAutostart(bool enabled) {
        QString autostartDir =
            QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";

        QString desktopFilePath = autostartDir + "/via.desktop";

        if (!enabled) {
            // Remove the file if it exists
            if (QFile::exists(desktopFilePath)) {
                return QFile::remove(desktopFilePath);
            }
            return true;  // Nothing to remove
        }

        // Ensure the autostart directory exists
        QDir dir;
        if (!dir.mkpath(autostartDir)) {
            return false;
        }

        QString execPath = resolveExecPath();

        return writeDesktopEntry(desktopFilePath, execPath, true);
    }

    /**
     * @brief Check if the autostart entry currently exists
     * @return true if ~/.config/autostart/via.desktop exists
     */
    static bool isAutostartEnabled() {
        QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
                       "/autostart/via.desktop";
        return QFile::exists(path);
    }

   private:
    /**
     * @brief Write a .desktop entry file
     * @param path Destination file path
     * @param execPath Exec= value (path to the binary/AppImage)
     * @param autostart If true, include X-GNOME-Autostart-enabled=true
     * @return true on success
     */
    static bool writeDesktopEntry(const QString& path, const QString& execPath, bool autostart) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return false;
        }

        QTextStream out(&file);
        out << "[Desktop Entry]\n";
        out << "Name=Via\n";
        out << "GenericName=Cloud Storage Client\n";
        out << "Comment=Google Drive desktop client for Linux\n";
        out << "Exec=" << execPath << "\n";
        out << "Icon=via\n";
        out << "Terminal=false\n";
        out << "Type=Application\n";
        out << "Categories=Network;FileTransfer;\n";
        out << "Keywords=google;drive;cloud;sync;storage;\n";
        out << "StartupWMClass=via\n";
        if (autostart) {
            out << "X-GNOME-Autostart-enabled=true\n";
        }

        file.close();
        return true;
    }

    /**
     * @brief Install the .desktop file to ~/.local/share/applications/
     *
     * Always writes the file so the Exec= path stays correct if the
     * user moves the AppImage.
     */
    static void installDesktopFile(const QString& execPath) {
        QString appsDir =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications";

        QDir dir;
        if (!dir.mkpath(appsDir)) {
            qWarning("AutostartManager: cannot create %s", qPrintable(appsDir));
            return;
        }

        QString desktopPath = appsDir + "/via.desktop";

        // Check if file already exists with correct Exec= path
        if (QFile::exists(desktopPath)) {
            QFile existing(desktopPath);
            if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString content = existing.readAll();
                existing.close();
                if (content.contains("Exec=" + execPath + "\n")) {
                    return;  // Already up-to-date
                }
            }
        }

        if (writeDesktopEntry(desktopPath, execPath, false)) {
            qInfo("AutostartManager: installed desktop file to %s", qPrintable(desktopPath));
        } else {
            qWarning("AutostartManager: failed to write %s", qPrintable(desktopPath));
        }
    }

    /**
     * @brief Install the app icon to the user's icon theme
     *
     * Copies via.svg from the Qt resource system (embedded in the binary)
     * or from alongside the executable to
     * ~/.local/share/icons/hicolor/scalable/apps/via.svg
     */
    static void installIcon() {
        QString iconsDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                           "/icons/hicolor/scalable/apps";

        QString destPath = iconsDir + "/via.svg";
        if (QFile::exists(destPath)) {
            return;  // Already installed
        }

        QDir dir;
        if (!dir.mkpath(iconsDir)) {
            qWarning("AutostartManager: cannot create %s", qPrintable(iconsDir));
            return;
        }

        // Try to find the icon from the AppDir or the source tree
        // When running as an AppImage, $APPDIR points to the mounted AppImage contents
        QStringList searchPaths;
        QByteArray appDirEnv = qgetenv("APPDIR");
        if (!appDirEnv.isEmpty()) {
            QString appDir = QString::fromUtf8(appDirEnv);
            searchPaths << appDir + "/usr/share/icons/hicolor/scalable/apps/via.svg";
            searchPaths << appDir + "/via.svg";
        }
        // Also try relative to the executable (for non-AppImage installs)
        QString exeDir = QCoreApplication::applicationDirPath();
        searchPaths << exeDir + "/../share/icons/hicolor/scalable/apps/via.svg";
        searchPaths << exeDir + "/../../res/icons/via.svg";

        for (const QString& src : searchPaths) {
            if (QFile::exists(src)) {
                if (QFile::copy(src, destPath)) {
                    qInfo("AutostartManager: installed icon to %s", qPrintable(destPath));
                    return;
                }
            }
        }

        qWarning("AutostartManager: could not find via.svg to install as icon");
    }

    /**
     * @brief Sync the autostart desktop entry with the QSettings value
     *
     * Ensures the autostart file exists/is-removed based on the
     * "advanced/startOnLogin" setting. Self-heals if the user
     * manually deleted the file or if it got out of sync.
     */
    static void syncAutostart() {
        QSettings settings;
        bool wantAutostart = settings.value("advanced/startOnLogin", false).toBool();
        bool hasAutostart = isAutostartEnabled();

        if (wantAutostart && !hasAutostart) {
            qInfo("AutostartManager: restoring autostart entry from settings");
            setAutostart(true);
        } else if (!wantAutostart && hasAutostart) {
            qInfo("AutostartManager: removing stale autostart entry");
            setAutostart(false);
        } else if (wantAutostart && hasAutostart) {
            // Update the Exec= path in case the AppImage was moved
            setAutostart(true);
        }
    }

    /**
     * @brief Resolve the path to use in the Exec= line
     *
     * Prefers $APPIMAGE (set by the AppImage runtime), then falls back
     * to QCoreApplication::applicationFilePath().
     */
    static QString resolveExecPath() {
        // $APPIMAGE is set by the AppImage runtime to the absolute path
        // of the .AppImage file that is running.
        QByteArray appImageEnv = qgetenv("APPIMAGE");
        if (!appImageEnv.isEmpty()) {
            return QString::fromUtf8(appImageEnv);
        }

        return QCoreApplication::applicationFilePath();
    }
};

#endif  // AUTOSTARTMANAGER_H
