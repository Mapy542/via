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
#include <QMimeDatabase>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "NativeDocShortcutHandler.h"

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

        const bool desktopUpdated = installDesktopFile(execPath);
        const bool mimeUpdated = installMimePackage();
        installIcon();

        // Always refresh if files were written, then validate registration.
        // If validation fails (e.g. a previous refresh was silently lost),
        // retry the refresh once so Dolphin and other file managers resolve
        // our custom MIME types on this launch.
        if (desktopUpdated || mimeUpdated) {
            refreshDesktopDatabases();
        }

        if (!isMimeRegistrationValid()) {
            qInfo("AutostartManager: MIME registration invalid — retrying refresh");
            refreshDesktopDatabases();
            if (!isMimeRegistrationValid()) {
                qWarning("AutostartManager: MIME registration still invalid after retry");
            }
        }

        installDefaultMimeAssociations();

        syncAutostart();
    }

    /**
     * @brief Enable or disable autostart
     * @param enabled true to install the autostart entry, false to remove it
     * @return true on success
     */
    static bool setAutostart(bool enabled) {
        QString autostartDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";

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
        QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/via.desktop";
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
    static bool writeDesktopEntry(const QString& path, const QString& execPath, bool autostart,
                                  bool* changedOut = nullptr) {
        const QString content = desktopEntryContent(execPath, autostart);

        QFile existing(path);
        if (existing.exists() && existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString currentContent = QString::fromUtf8(existing.readAll());
            existing.close();
            if (currentContent == content) {
                if (changedOut) {
                    *changedOut = false;
                }
                return true;
            }
        }

        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return false;
        }

        QTextStream out(&file);
        out << content;

        file.close();
        if (changedOut) {
            *changedOut = true;
        }
        return true;
    }

    static QString desktopEntryContent(const QString& execPath, bool autostart) {
        QString execField = escapeDesktopExec(execPath);
        if (!autostart) {
            execField += QStringLiteral(" %F");
        }

        QString content;
        QTextStream out(&content);
        out << "[Desktop Entry]\n";
        out << "Name=Via\n";
        out << "GenericName=Cloud Storage Client\n";
        out << "Comment=Google Drive desktop client for Linux\n";
        out << "Exec=" << execField << "\n";
        out << "Icon=via\n";
        out << "Terminal=false\n";
        out << "Type=Application\n";
        out << "Categories=Network;FileTransfer;\n";
        out << "Keywords=google;drive;cloud;sync;storage;\n";
        out << "StartupWMClass=via\n";
        if (!autostart) {
            out << "MimeType=" << nativeDocDesktopMimeTypesField() << "\n";
        }
        if (autostart) {
            out << "X-GNOME-Autostart-enabled=true\n";
        }
        return content;
    }

    static QString escapeDesktopExec(const QString& execPath) {
        QString escaped = execPath;
        escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
        return QStringLiteral("\"") + escaped + QStringLiteral("\"");
    }

    /**
     * @brief Install the .desktop file to ~/.local/share/applications/
     *
     * Always writes the file so the Exec= path stays correct if the
     * user moves the AppImage.
     */
    static bool installDesktopFile(const QString& execPath) {
        QString appsDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications";

        QDir dir;
        if (!dir.mkpath(appsDir)) {
            qWarning("AutostartManager: cannot create %s", qPrintable(appsDir));
            return false;
        }

        QString desktopPath = appsDir + "/via.desktop";

        bool changed = false;
        if (writeDesktopEntry(desktopPath, execPath, false, &changed)) {
            if (changed) {
                qInfo("AutostartManager: installed desktop file to %s", qPrintable(desktopPath));
            }
            return changed;
        } else {
            qWarning("AutostartManager: failed to write %s", qPrintable(desktopPath));
            return false;
        }
    }

    static bool installMimePackage() {
        const QString mimePackagesDir =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/mime/packages";

        QDir dir;
        if (!dir.mkpath(mimePackagesDir)) {
            qWarning("AutostartManager: cannot create %s", qPrintable(mimePackagesDir));
            return false;
        }

        const QString mimePackagePath = mimePackagesDir + "/via-native-docs.xml";
        const QString content = nativeDocMimePackageXml();

        QFile existing(mimePackagePath);
        if (existing.exists() && existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString currentContent = QString::fromUtf8(existing.readAll());
            existing.close();
            if (currentContent == content) {
                return false;
            }
        }

        QFile file(mimePackagePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("AutostartManager: failed to write %s", qPrintable(mimePackagePath));
            return false;
        }

        QTextStream out(&file);
        out << content;
        file.close();

        qInfo("AutostartManager: installed MIME package to %s", qPrintable(mimePackagePath));
        return true;
    }

    static bool isMimeRegistrationValid() {
        QMimeDatabase mimeDb;
        const QStringList extensions = nativeDocShortcutExtensions();
        const QStringList expectedTypes = nativeDocDesktopMimeTypes();
        bool allValid = true;
        for (int i = 0; i < extensions.size(); ++i) {
            const QString fileName = QStringLiteral("test.") + extensions[i];
            const QMimeType resolved = mimeDb.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
            if (resolved.name() != expectedTypes[i]) {
                qInfo("AutostartManager: .%s resolved to %s, expected %s", qPrintable(extensions[i]),
                      qPrintable(resolved.name()), qPrintable(expectedTypes[i]));
                allValid = false;
            }
        }

        // Also verify that via.desktop is the default handler for our types.
        // Read the user's mimeapps.list and check the [Default Applications] section.
        const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        const QString mimeappsPath = configDir + "/mimeapps.list";
        QFile mimeappsFile(mimeappsPath);
        QMap<QString, QString> defaults;
        if (mimeappsFile.exists() && mimeappsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            bool inDefaultSection = false;
            while (!mimeappsFile.atEnd()) {
                const QString line = QString::fromUtf8(mimeappsFile.readLine()).trimmed();
                if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                    inDefaultSection = (line == QLatin1String("[Default Applications]"));
                    continue;
                }
                if (inDefaultSection) {
                    const int eq = line.indexOf(QLatin1Char('='));
                    if (eq > 0) {
                        defaults.insert(line.left(eq), line.mid(eq + 1));
                    }
                }
            }
            mimeappsFile.close();
        }

        for (const QString& mime : expectedTypes) {
            const QString handler = defaults.value(mime);
            if (!handler.contains(QLatin1String("via.desktop"))) {
                qInfo("AutostartManager: default handler for %s is '%s', expected via.desktop",
                      qPrintable(mime), qPrintable(handler));
                allValid = false;
            }
        }

        return allValid;
    }

    static void refreshDesktopDatabases() {
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const QString appsDir = dataDir + "/applications";
        const QString mimeDir = dataDir + "/mime";

        const QString updateDesktopDatabase = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
        if (!updateDesktopDatabase.isEmpty()) {
            const int rc = QProcess::execute(updateDesktopDatabase, {appsDir});
            if (rc != 0) {
                qWarning("AutostartManager: update-desktop-database failed (%d)", rc);
            }
        }

        const QString updateMimeDatabase = QStandardPaths::findExecutable(QStringLiteral("update-mime-database"));
        if (!updateMimeDatabase.isEmpty()) {
            const int rc = QProcess::execute(updateMimeDatabase, {mimeDir});
            if (rc != 0) {
                qWarning("AutostartManager: update-mime-database failed (%d)", rc);
            }
        }

        // KDE/Plasma maintains its own service cache (sycoca). Without
        // rebuilding it, Dolphin may not pick up new MIME type associations
        // until the cache expires (~5 minutes) or the session restarts.
        refreshKdeSycoca();
    }

    /**
     * @brief Rebuild the KDE service cache if running under Plasma
     *
     * Tries kbuildsycoca6 first (Plasma 6), then kbuildsycoca5 (Plasma 5).
     * No-op on non-KDE desktops.
     */
    static void refreshKdeSycoca() {
        const QString currentDesktop = QString::fromUtf8(qgetenv("XDG_CURRENT_DESKTOP")).toLower();
        if (!currentDesktop.contains(QLatin1String("kde")) &&
            !currentDesktop.contains(QLatin1String("plasma"))) {
            return;  // Not KDE — nothing to do
        }

        // Prefer kbuildsycoca6 (Plasma 6), fall back to kbuildsycoca5
        QString kbuildsycoca = QStandardPaths::findExecutable(QStringLiteral("kbuildsycoca6"));
        if (kbuildsycoca.isEmpty()) {
            kbuildsycoca = QStandardPaths::findExecutable(QStringLiteral("kbuildsycoca5"));
        }
        if (kbuildsycoca.isEmpty()) {
            qInfo("AutostartManager: KDE detected but kbuildsycoca not found — skipping sycoca refresh");
            return;
        }

        const int rc = QProcess::execute(kbuildsycoca, {QStringLiteral("--noincremental")});
        if (rc != 0) {
            qWarning("AutostartManager: %s failed (%d)", qPrintable(kbuildsycoca), rc);
        } else {
            qInfo("AutostartManager: KDE service cache rebuilt via %s", qPrintable(kbuildsycoca));
        }
    }

    /**
     * @brief Set Via as the default opener for its private MIME types
     *
     * Writes a [Default Applications] section to the user's mimeapps.list
     * so that file managers (Dolphin, Nautilus, etc.) open .gdoc/.gsheet/etc.
     * files with Via without prompting.
     *
     * Only adds defaults for types that do not already have one, so it
     * never overrides an explicit user choice.
     */
    static void installDefaultMimeAssociations() {
        const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        const QString mimeappsPath = configDir + "/mimeapps.list";

        // Read the current file (may not exist yet)
        QMap<QString, QString> defaults;
        QStringList otherLines;
        bool inDefaultSection = false;

        QFile existing(mimeappsPath);
        if (existing.exists() && existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!existing.atEnd()) {
                const QString line = QString::fromUtf8(existing.readLine()).trimmed();
                if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                    inDefaultSection = (line == QLatin1String("[Default Applications]"));
                    if (!inDefaultSection) {
                        otherLines.append(line);
                    }
                    continue;
                }
                if (inDefaultSection) {
                    const int eq = line.indexOf(QLatin1Char('='));
                    if (eq > 0) {
                        defaults.insert(line.left(eq), line.mid(eq + 1));
                    }
                } else {
                    otherLines.append(line);
                }
            }
            existing.close();
        }

        // Add our types only if not already set
        const QStringList mimeTypes = nativeDocDesktopMimeTypes();
        bool changed = false;
        for (const QString& mime : mimeTypes) {
            if (!defaults.contains(mime)) {
                defaults.insert(mime, QStringLiteral("via.desktop"));
                changed = true;
            }
        }

        if (!changed) {
            return;  // All associations already present
        }

        // Rewrite the file
        QDir().mkpath(configDir);
        QFile file(mimeappsPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("AutostartManager: failed to write %s", qPrintable(mimeappsPath));
            return;
        }

        QTextStream out(&file);
        // Write non-default sections first
        for (const QString& line : otherLines) {
            out << line << "\n";
        }
        // Write our [Default Applications] section
        out << "[Default Applications]\n";
        for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
            out << it.key() << "=" << it.value() << "\n";
        }
        file.close();

        qInfo("AutostartManager: installed default MIME associations to %s", qPrintable(mimeappsPath));
    }

    /**
     * @brief Install the app icon to the user's icon theme
     *
     * Copies via.svg from the Qt resource system (embedded in the binary)
     * or from alongside the executable to
     * ~/.local/share/icons/hicolor/scalable/apps/via.svg
     */
    static void installIcon() {
        QString iconsDir =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/icons/hicolor/scalable/apps";

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
