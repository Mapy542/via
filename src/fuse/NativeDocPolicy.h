/**
 * @file NativeDocPolicy.h
 * @brief Policy helper for Google-native doc representation in FUSE
 *
 * Maps (remote MIME type, serving mode) to visibility, filename extension,
 * output MIME type, and content strategy. Used by FuseDriver and
 * MetadataRefreshWorker to decide how native docs appear in the FUSE mount.
 */

#ifndef NATIVEDOCPOLICY_H
#define NATIVEDOCPOLICY_H

#include <QString>

/**
 * @enum NativeDocMode
 * @brief Global serving mode for Google-native documents in FUSE
 */
enum class NativeDocMode {
    Hide,             ///< Current default — native docs are invisible
    BrowserShortcut,  ///< Read-only stub files that open webViewLink
    OpenDocument,     ///< Exported as ODT/ODS/ODP snapshots (read-only)
    Text              ///< Exported as Markdown/CSV/TXT snapshots (read-only)
};

/**
 * @struct NativeDocRepresentation
 * @brief Describes how a single native doc should appear in FUSE
 */
struct NativeDocRepresentation {
    bool visible = false;    ///< Whether the item appears in the FUSE tree
    QString extension;       ///< Pseudo-extension to append (e.g. ".gdoc", ".odt")
    QString outputMimeType;  ///< MIME type to report / export target
    bool synthetic = false;  ///< true = content generated in-process (stub); false = Drive export
    bool readOnly = true;    ///< Always true for native docs in this implementation
};

/**
 * @brief Convert a settings string to NativeDocMode enum
 * @param s String from QSettings (e.g. "hide", "browser-shortcut")
 * @return Corresponding enum value; defaults to Hide for unknown strings
 */
inline NativeDocMode nativeDocModeFromString(const QString& s) {
    if (s == QLatin1String("browser-shortcut")) return NativeDocMode::BrowserShortcut;
    if (s == QLatin1String("open-document")) return NativeDocMode::OpenDocument;
    if (s == QLatin1String("text")) return NativeDocMode::Text;
    return NativeDocMode::Hide;
}

/**
 * @brief Convert NativeDocMode enum to QSettings string
 */
inline QString nativeDocModeToString(NativeDocMode m) {
    switch (m) {
        case NativeDocMode::BrowserShortcut:
            return QStringLiteral("browser-shortcut");
        case NativeDocMode::OpenDocument:
            return QStringLiteral("open-document");
        case NativeDocMode::Text:
            return QStringLiteral("text");
        default:
            return QStringLiteral("hide");
    }
}

inline NativeDocMode effectiveNativeDocMode(const QString& overrideMode, NativeDocMode globalMode) {
    return overrideMode.isEmpty() ? globalMode : nativeDocModeFromString(overrideMode);
}

/**
 * @brief Determine how a native doc should be represented in FUSE
 * @param remoteMimeType The Google-native MIME type (e.g. "application/vnd.google-apps.document")
 * @param mode Current global serving mode
 * @return Representation descriptor; visible==false means the item should be hidden
 *
 * Known Google-native types handled:
 *   application/vnd.google-apps.document     — Docs
 *   application/vnd.google-apps.spreadsheet  — Sheets
 *   application/vnd.google-apps.presentation — Slides
 *   application/vnd.google-apps.drawing      — Drawings
 *   application/vnd.google-apps.jam          — Jamboard
 *   application/vnd.google-apps.script       — Apps Script
 *   application/vnd.google-apps.form         — Forms
 *   application/vnd.google-apps.site         — Sites
 *   application/vnd.google-apps.map          — My Maps
 *   application/vnd.google-apps.vid          — Google Vids
 *
 * Unsupported types (no safe export) are hidden even when mode != Hide.
 */
inline NativeDocRepresentation nativeDocRepresentation(const QString& remoteMimeType,
                                                       NativeDocMode mode) {
    NativeDocRepresentation r;

    if (mode == NativeDocMode::Hide) {
        return r;  // visible == false
    }

    // Identify type suffix: "document", "spreadsheet", etc.
    const QLatin1String prefix("application/vnd.google-apps.");
    if (!remoteMimeType.startsWith(prefix)) {
        return r;  // not a native type — caller shouldn't have asked
    }
    const QStringView typeKey = QStringView(remoteMimeType).mid(prefix.size());

    if (mode == NativeDocMode::BrowserShortcut) {
        // All known native types can be opened in a browser.
        r.synthetic = true;
        r.readOnly = true;

        if (typeKey == u"document") {
            r.visible = true;
            r.extension = QStringLiteral(".gdoc");
            r.outputMimeType = QStringLiteral("application/x-via-gdoc");
        } else if (typeKey == u"spreadsheet") {
            r.visible = true;
            r.extension = QStringLiteral(".gsheet");
            r.outputMimeType = QStringLiteral("application/x-via-gsheet");
        } else if (typeKey == u"presentation") {
            r.visible = true;
            r.extension = QStringLiteral(".gslides");
            r.outputMimeType = QStringLiteral("application/x-via-gslides");
        } else if (typeKey == u"drawing") {
            r.visible = true;
            r.extension = QStringLiteral(".gdraw");
            r.outputMimeType = QStringLiteral("application/x-via-gdraw");
        } else if (typeKey == u"form" || typeKey == u"jam" || typeKey == u"script" ||
                   typeKey == u"site" || typeKey == u"map" || typeKey == u"vid") {
            r.visible = true;
            r.extension = QStringLiteral(".gdrive");
            r.outputMimeType = QStringLiteral("application/x-via-gdrive");
        }
        // Unknown native types stay hidden (visible == false)
        return r;
    }

    if (mode == NativeDocMode::OpenDocument) {
        r.synthetic = false;
        r.readOnly = true;

        if (typeKey == u"document") {
            r.visible = true;
            r.extension = QStringLiteral(".odt");
            r.outputMimeType = QStringLiteral("application/vnd.oasis.opendocument.text");
        } else if (typeKey == u"spreadsheet") {
            r.visible = true;
            r.extension = QStringLiteral(".ods");
            r.outputMimeType = QStringLiteral("application/vnd.oasis.opendocument.spreadsheet");
        } else if (typeKey == u"presentation") {
            r.visible = true;
            r.extension = QStringLiteral(".odp");
            r.outputMimeType = QStringLiteral("application/vnd.oasis.opendocument.presentation");
        }
        // Drawings, Forms, Scripts, etc. have no OpenDocument equivalent → hidden
        return r;
    }

    if (mode == NativeDocMode::Text) {
        r.synthetic = false;
        r.readOnly = true;

        if (typeKey == u"document") {
            r.visible = true;
            r.extension = QStringLiteral(".md");
            r.outputMimeType = QStringLiteral("text/markdown");
        } else if (typeKey == u"spreadsheet") {
            r.visible = true;
            r.extension = QStringLiteral(".csv");
            r.outputMimeType = QStringLiteral("text/csv");
        } else if (typeKey == u"presentation") {
            r.visible = true;
            r.extension = QStringLiteral(".txt");
            r.outputMimeType = QStringLiteral("text/plain");
        }
        // Other types have no sensible text export → hidden
        return r;
    }

    return r;
}

inline NativeDocRepresentation effectiveNativeDocRepresentation(const QString& remoteMimeType,
                                                                const QString& overrideMode,
                                                                NativeDocMode globalMode) {
    return nativeDocRepresentation(remoteMimeType,
                                   effectiveNativeDocMode(overrideMode, globalMode));
}

#endif  // NATIVEDOCPOLICY_H
