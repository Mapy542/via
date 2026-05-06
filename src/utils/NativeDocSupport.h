/**
 * @file NativeDocSupport.h
 * @brief Shared helpers for Google-native document representation and state
 */

#ifndef NATIVEDOCSUPPORT_H
#define NATIVEDOCSUPPORT_H

#include <QByteArray>
#include <QString>
#include <QStringView>

enum class NativeDocMode {
    Hide,
    BrowserShortcut,
    OpenDocument,
    Text,
};

struct NativeDocRepresentation {
    bool visible = false;
    QString extension;
    QString outputMimeType;
    bool synthetic = false;
    bool readOnly = true;
};

struct NativeDocState {
    QString fileId;
    QString remoteName;
    QString remoteMimeType;
    QString webViewLink;
    QString nativeDocModeOverride;

    bool isValid() const { return !fileId.isEmpty(); }
};

inline bool isNativeDocMimeType(const QString& remoteMimeType) {
    return remoteMimeType.startsWith(QLatin1String("application/vnd.google-apps.")) &&
           !remoteMimeType.endsWith(QLatin1String("folder"));
}

inline NativeDocMode nativeDocModeFromString(const QString& s) {
    if (s == QLatin1String("browser-shortcut"))
        return NativeDocMode::BrowserShortcut;
    if (s == QLatin1String("open-document"))
        return NativeDocMode::OpenDocument;
    if (s == QLatin1String("text"))
        return NativeDocMode::Text;
    return NativeDocMode::Hide;
}

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

inline NativeDocRepresentation nativeDocRepresentation(const QString& remoteMimeType,
                                                       NativeDocMode mode) {
    NativeDocRepresentation r;

    if (mode == NativeDocMode::Hide || !isNativeDocMimeType(remoteMimeType)) {
        return r;
    }

    const QLatin1String prefix("application/vnd.google-apps.");
    const QStringView typeKey = QStringView(remoteMimeType).mid(prefix.size());

    if (mode == NativeDocMode::BrowserShortcut) {
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

inline QString nativeDocVisibleName(const QString& remoteName,
                                    const NativeDocRepresentation& representation) {
    return representation.extension.isEmpty() ? remoteName : remoteName + representation.extension;
}

inline QString nativeDocVisibleName(const QString& remoteName, const QString& remoteMimeType,
                                    const QString& overrideMode, NativeDocMode globalMode) {
    return nativeDocVisibleName(
        remoteName, effectiveNativeDocRepresentation(remoteMimeType, overrideMode, globalMode));
}

inline QString nativeDocRemoteNameFromVisibleName(const QString& visibleName,
                                                  const NativeDocRepresentation& representation) {
    if (!representation.extension.isEmpty() && visibleName.endsWith(representation.extension)) {
        return visibleName.left(visibleName.size() - representation.extension.size());
    }
    return visibleName;
}

inline QString nativeDocRemoteNameFromVisibleName(const QString& visibleName,
                                                  const QString& remoteMimeType,
                                                  const QString& overrideMode,
                                                  NativeDocMode globalMode) {
    return nativeDocRemoteNameFromVisibleName(
        visibleName, effectiveNativeDocRepresentation(remoteMimeType, overrideMode, globalMode));
}

inline QByteArray nativeDocShortcutPayload(const QString& webViewLink,
                                           const QString& remoteMimeType) {
    return QStringLiteral("[Via Native Document]\nURL=%1\nMimeType=%2\n")
        .arg(webViewLink, remoteMimeType)
        .toUtf8();
}

inline QByteArray nativeDocShortcutPayload(const NativeDocState& state) {
    return nativeDocShortcutPayload(state.webViewLink, state.remoteMimeType);
}

inline bool isNativeDocExportLimitError(const QString& errorMsg, int httpStatus) {
    if (httpStatus != 403) {
        return false;
    }

    const QString lowered = errorMsg.toLower();
    return lowered.contains(QStringLiteral("10 mb")) || lowered.contains(QStringLiteral("10mb")) ||
           lowered.contains(QStringLiteral("too large to be exported")) ||
           lowered.contains(QStringLiteral("too large to export")) ||
           (lowered.contains(QStringLiteral("export")) &&
            (lowered.contains(QStringLiteral("limit")) ||
             lowered.contains(QStringLiteral("maximum")) ||
             lowered.contains(QStringLiteral("size")) ||
             lowered.contains(QStringLiteral("too large"))));
}

inline QString nativeDocExportFailureMessage(const QString& errorMsg, int httpStatus) {
    if (isNativeDocExportLimitError(errorMsg, httpStatus)) {
        return QStringLiteral(
            "Google limits native doc exports to 10 MB. Open the document in your browser "
            "instead.");
    }

    const QString trimmed = errorMsg.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }

    if (httpStatus == 403) {
        return QStringLiteral("Google rejected the export for this document or account.");
    }

    return QStringLiteral("Native document export failed.");
}

inline bool shouldPersistNativeDocState(const NativeDocState& state) {
    return state.isValid() &&
           (isNativeDocMimeType(state.remoteMimeType) || !state.webViewLink.isEmpty() ||
            !state.nativeDocModeOverride.isEmpty());
}

#endif  // NATIVEDOCSUPPORT_H