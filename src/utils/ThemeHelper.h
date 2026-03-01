/**
 * @file ThemeHelper.h
 * @brief Theme detection and icon path utility
 *
 * Provides automatic detection of light/dark theme and returns
 * the appropriate icon paths for the current theme.
 */

#ifndef THEMEHELPER_H
#define THEMEHELPER_H

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStyleHints>

/**
 * @class ThemeHelper
 * @brief Utility class for theme-aware icon loading
 *
 * Provides two detection methods:
 * - System tray icons: Uses QGuiApplication::styleHints()->colorScheme() with user override
 * - GUI window icons: Uses QPalette luminance for reliable detection of app window theme
 *
 * - Dark theme (dark background): Uses light/white icons from icons/dark/
 * - Light theme (light background): Uses dark icons from icons/light/
 */
class ThemeHelper {
   public:
    /**
     * @brief Theme override values stored in settings (for tray icons)
     */
    enum class ThemeOverride { Auto = 0, Light = 1, Dark = 2 };

    /**
     * @brief Get the current theme override from settings
     * @return ThemeOverride value (Auto, Light, or Dark)
     */
    static ThemeOverride getThemeOverride() {
        QSettings settings;
        return static_cast<ThemeOverride>(settings.value("advanced/themeOverride", 0).toInt());
    }

    /**
     * @brief Check if the system tray theme is dark (with override support)
     * @return true if dark theme, respecting user override
     *
     * Used for system tray icons where we need override support.
     */
    static bool isTrayDarkTheme() {
        ThemeOverride override = getThemeOverride();
        if (override == ThemeOverride::Light) {
            return false;
        }
        if (override == ThemeOverride::Dark) {
            return true;
        }
        // Auto: use Qt's color scheme detection
        return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }

    /**
     * @brief Check if the application window theme is dark using QPalette
     * @return true if the window background is dark (luminance < 128)
     *
     * Used for in-GUI icons where we can reliably detect the actual window color.
     */
    static bool isGuiDarkTheme() {
        QPalette palette = QApplication::palette();
        QColor windowColor = palette.color(QPalette::Window);
        // Calculate perceived luminance (standard formula)
        int luminance = (windowColor.red() * 299 + windowColor.green() * 587 +
                         windowColor.blue() * 114) /
                        1000;
        return luminance < 128;
    }

    /**
     * @brief Get the icon folder for GUI/window icons based on palette
     * @return "dark" for dark themes, "light" for light themes
     */
    static QString guiIconThemeFolder() { return isGuiDarkTheme() ? "dark" : "light"; }

    /**
     * @brief Get the full resource path for a GUI icon
     * @param iconName Base name of the icon (e.g., "drive-idle.svg")
     * @return Full resource path based on window palette
     */
    static QString guiIconPath(const QString& iconName) {
        return QString(":/icons/%1/%2").arg(guiIconThemeFolder(), iconName);
    }

    /**
     * @brief Load a GUI icon with palette-based theme detection
     * @param iconName Base name of the icon (e.g., "drive-idle.svg")
     * @return QIcon loaded from the appropriate theme folder
     */
    static QIcon guiIcon(const QString& iconName) { return QIcon(guiIconPath(iconName)); }

    /**
     * @brief Get the icon folder for system tray icons
     * @return "dark" or "light" based on system theme with override
     */
    static QString trayIconThemeFolder() { return isTrayDarkTheme() ? "dark" : "light"; }

    /**
     * @brief Get the full resource path for a themed tray icon
     * @param iconName Base name of the icon (e.g., "drive-idle.svg")
     * @return Full resource path for system tray
     */
    static QString trayIconPath(const QString& iconName) {
        return QString(":/icons/%1/%2").arg(trayIconThemeFolder(), iconName);
    }

    /**
     * @brief Load a tray icon with colorScheme-based detection + override
     * @param iconName Base name of the icon (e.g., "drive-idle.svg")
     * @return QIcon loaded from the appropriate theme folder for tray
     */
    static QIcon trayIcon(const QString& iconName) { return QIcon(trayIconPath(iconName)); }

    // Legacy aliases for backward compatibility
    static bool isDarkTheme() { return isTrayDarkTheme(); }
    static QString iconThemeFolder() { return trayIconThemeFolder(); }
    static QString iconPath(const QString& iconName) { return trayIconPath(iconName); }
    static QIcon icon(const QString& iconName) { return trayIcon(iconName); }
};

#endif  // THEMEHELPER_H
