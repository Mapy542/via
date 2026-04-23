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
#include <QUrl>
#include <optional>

struct NativeDocShortcutInfo {
    QUrl url;
    QString remoteMimeType;
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

inline QStringList nativeDocShortcutExtensions() {
    return {
        QStringLiteral("gdoc"),  QStringLiteral("gsheet"), QStringLiteral("gslides"),
        QStringLiteral("gdraw"), QStringLiteral("gdrive"),
    };
}

inline QStringList nativeDocDesktopMimeTypes() {
    return {
        QStringLiteral("application/x-via-gdoc"),    QStringLiteral("application/x-via-gsheet"),
        QStringLiteral("application/x-via-gslides"), QStringLiteral("application/x-via-gdraw"),
        QStringLiteral("application/x-via-gdrive"),
    };
}

inline QString nativeDocDesktopMimeTypesField() {
    const QStringList mimeTypes = nativeDocDesktopMimeTypes();
    return mimeTypes.join(QLatin1Char(';')) + QLatin1Char(';');
}

inline QString nativeDocMimePackageXml() {
    return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-via-gdoc">
    <comment>Via Google Docs shortcut</comment>
    <glob pattern="*.gdoc"/>
  </mime-type>
  <mime-type type="application/x-via-gsheet">
    <comment>Via Google Sheets shortcut</comment>
    <glob pattern="*.gsheet"/>
  </mime-type>
  <mime-type type="application/x-via-gslides">
    <comment>Via Google Slides shortcut</comment>
    <glob pattern="*.gslides"/>
  </mime-type>
  <mime-type type="application/x-via-gdraw">
    <comment>Via Google Drawings shortcut</comment>
    <glob pattern="*.gdraw"/>
  </mime-type>
  <mime-type type="application/x-via-gdrive">
    <comment>Via Google Drive shortcut</comment>
    <glob pattern="*.gdrive"/>
  </mime-type>
</mime-info>
)");
}

inline bool isNativeDocShortcutPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return nativeDocShortcutExtensions().contains(suffix);
}

inline bool isNativeDocShortcutArgument(const QString& argument, QString* resolvedPathOut = nullptr) {
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
            (lowered.contains(QStringLiteral("limit")) || lowered.contains(QStringLiteral("maximum")) ||
             lowered.contains(QStringLiteral("size")) || lowered.contains(QStringLiteral("too large"))));
}

inline QString nativeDocExportFailureMessage(const QString& errorMsg, int httpStatus) {
    if (isNativeDocExportLimitError(errorMsg, httpStatus)) {
        return QStringLiteral("Google limits native doc exports to 10 MB. Open the document in your browser instead.");
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

inline std::optional<NativeDocShortcutInfo> parseNativeDocShortcutText(const QString& text,
                                                                       QString* errorOut = nullptr) {
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    if (rawLines.isEmpty() || rawLines.first().trimmed() != QStringLiteral("[Via Native Document]")) {
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

inline std::optional<NativeDocShortcutInfo> parseNativeDocShortcutFile(const QString& path,
                                                                       QString* errorOut = nullptr) {
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