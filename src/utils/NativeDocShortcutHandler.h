/**
 * @file NativeDocShortcutHandler.h
 * @brief Shared helpers for Via native-doc shortcut files and MIME integration
 */

#ifndef NATIVEDOCSHORTCUTHANDLER_H
#define NATIVEDOCSHORTCUTHANDLER_H

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QUrl>
#include <array>
#include <optional>

struct NativeDocShortcutInfo {
    QUrl url;
    QString remoteMimeType;
};

struct NativeDocDesktopRegistration {
    const char* extension;
    const char* mimeType;
    const char* iconName;
    const char* comment;
};

inline QString nativeDocShortcutPathFromArgument(const QString& argument) {
    const QUrl url(argument);
    if (url.isValid() && url.isLocalFile()) {
        const QString localPath = url.toLocalFile();
        if (!localPath.isEmpty()) {
            return QFileInfo(localPath).absoluteFilePath();
        }
    }

    return QFileInfo(argument).absoluteFilePath();
}

inline const std::array<NativeDocDesktopRegistration, 5>& nativeDocDesktopRegistrations() {
    static const std::array<NativeDocDesktopRegistration, 5> registrations{{
        {"gdoc", "application/x-via-gdoc", "application-x-via-gdoc", "Via Google Docs shortcut"},
        {"gsheet", "application/x-via-gsheet", "application-x-via-gsheet",
         "Via Google Sheets shortcut"},
        {"gslides", "application/x-via-gslides", "application-x-via-gslides",
         "Via Google Slides shortcut"},
        {"gdraw", "application/x-via-gdraw", "application-x-via-gdraw",
         "Via Google Drawings shortcut"},
        {"gdrive", "application/x-via-gdrive", "application-x-via-gdrive",
         "Via Google Drive shortcut"},
    }};

    return registrations;
}

inline QStringList nativeDocShortcutExtensions() {
    QStringList extensions;
    const auto& registrations = nativeDocDesktopRegistrations();
    extensions.reserve(static_cast<qsizetype>(registrations.size()));
    for (const auto& registration : registrations) {
        extensions.append(QString::fromLatin1(registration.extension));
    }

    return extensions;
}

inline QStringList nativeDocDesktopMimeTypes() {
    QStringList mimeTypes;
    const auto& registrations = nativeDocDesktopRegistrations();
    mimeTypes.reserve(static_cast<qsizetype>(registrations.size()));
    for (const auto& registration : registrations) {
        mimeTypes.append(QString::fromLatin1(registration.mimeType));
    }

    return mimeTypes;
}

inline QStringList nativeDocDesktopIconNames() {
    QStringList iconNames;
    const auto& registrations = nativeDocDesktopRegistrations();
    iconNames.reserve(static_cast<qsizetype>(registrations.size()));
    for (const auto& registration : registrations) {
        iconNames.append(QString::fromLatin1(registration.iconName));
    }

    return iconNames;
}

inline QString nativeDocDesktopMimeTypesField() {
    const QStringList mimeTypes = nativeDocDesktopMimeTypes();
    return mimeTypes.join(QLatin1Char(';')) + QLatin1Char(';');
}

inline QString nativeDocMimePackageXml() {
    QString xml;
    QTextStream out(&xml);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";

    for (const auto& registration : nativeDocDesktopRegistrations()) {
        out << "  <mime-type type=\"" << registration.mimeType << "\">\n";
        out << "    <comment>" << registration.comment << "</comment>\n";
        out << "    <icon name=\"" << registration.iconName << "\"/>\n";
        out << "    <glob pattern=\"*." << registration.extension << "\"/>\n";
        out << "  </mime-type>\n";
    }

    out << "</mime-info>\n";
    return xml;
}

inline bool isNativeDocShortcutPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return nativeDocShortcutExtensions().contains(suffix);
}

inline bool isNativeDocShortcutArgument(const QString& argument,
                                        QString* resolvedPathOut = nullptr) {
    const QString resolvedPath = nativeDocShortcutPathFromArgument(argument);
    if (!isNativeDocShortcutPath(resolvedPath)) {
        return false;
    }

    if (resolvedPathOut) {
        *resolvedPathOut = resolvedPath;
    }
    return true;
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

inline std::optional<NativeDocShortcutInfo> parseNativeDocShortcutText(
    const QString& text, QString* errorOut = nullptr) {
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    if (rawLines.isEmpty() ||
        rawLines.first().trimmed() != QStringLiteral("[Via Native Document]")) {
        if (errorOut) {
            *errorOut = QStringLiteral("missing Via native-doc header");
        }
        return std::nullopt;
    }

    QString urlString;
    QString mimeType;
    for (const QString& rawLine : rawLines.mid(1)) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("URL="))) {
            urlString = line.mid(4).trimmed();
        } else if (line.startsWith(QStringLiteral("MimeType="))) {
            mimeType = line.mid(9).trimmed();
        }
    }

    if (urlString.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("missing URL field");
        }
        return std::nullopt;
    }

    const QUrl url(urlString);
    if (!url.isValid() || url.scheme().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("invalid URL field");
        }
        return std::nullopt;
    }

    NativeDocShortcutInfo info;
    info.url = url;
    info.remoteMimeType = mimeType;
    return info;
}

inline std::optional<NativeDocShortcutInfo> parseNativeDocShortcutFile(
    const QString& path, QString* errorOut = nullptr) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) {
            *errorOut = QStringLiteral("failed to open shortcut file");
        }
        return std::nullopt;
    }

    const QByteArray payload = file.readAll();
    if (payload.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("shortcut file is empty");
        }
        return std::nullopt;
    }

    return parseNativeDocShortcutText(QString::fromUtf8(payload), errorOut);
}

#endif  // NATIVEDOCSHORTCUTHANDLER_H