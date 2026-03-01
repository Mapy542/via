/**
 * @file AutostartManager.h
 * @brief Manages XDG autostart desktop entry for Via
 *
 * Writes or removes ~/.config/autostart/via.desktop to control
 * whether Via starts automatically on user login.
 */

#ifndef AUTOSTARTMANAGER_H
#define AUTOSTARTMANAGER_H

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>

/**
 * @class AutostartManager
 * @brief Static utility for managing XDG autostart entries
 *
 * Reads the running executable path (which is the AppImage path when
 * running from an AppImage, thanks to $APPIMAGE env var or
 * QCoreApplication::applicationFilePath()) and writes a .desktop entry
 * that will launch Via on login.
 */
class AutostartManager {
   public:
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

        // Determine the executable path
        // When running as an AppImage, $APPIMAGE holds the path to the .AppImage file.
        // Otherwise, fall back to the running binary path.
        QString execPath = resolveExecPath();

        // Write the desktop entry
        QFile file(desktopFilePath);
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
        out << "X-GNOME-Autostart-enabled=true\n";
        out << "StartupWMClass=via\n";

        file.close();
        return true;
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
