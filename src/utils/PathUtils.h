/**
 * @file PathUtils.h
 * @brief Utilities for sanitizing remote file/folder names
 *
 * Remote Drive file names may contain characters that are dangerous when used
 * to construct local filesystem paths — path separators, ".." traversal
 * components, null bytes, or device-reserved names. This header provides a
 * single helper to strip those hazards before a remote name is joined with a
 * local sync root.
 */

#ifndef PATHUTILS_H
#define PATHUTILS_H

#include <QDir>
#include <QString>

namespace PathUtils {

/**
 * @brief Sanitize a remote file/folder name for safe use in local paths.
 *
 * Strips or replaces:
 *  - Null bytes (\0)
 *  - Forward slashes (/)
 *  - Backslashes (\)
 *  - Leading/trailing whitespace and dots (prevents hidden/invalid names)
 *  - Names that resolve to "." or ".." after trimming
 *  - Control characters (U+0000–U+001F)
 *
 * If the sanitized result is empty, returns "_unnamed".
 *
 * @param name  The raw remote file name (e.g. from DriveFile::name)
 * @return A safe name suitable for path construction
 */
inline QString sanitizeRemoteFileName(const QString& name) {
    QString safe = name;

    // Remove null bytes and control characters (U+0000–U+001F)
    safe.remove(QChar(0));
    for (int c = 1; c <= 0x1F; ++c) {
        safe.remove(QChar(c));
    }

    // Replace path separators with underscores
    safe.replace('/', '_');
    safe.replace('\\', '_');

    // Strip leading/trailing whitespace and dots
    while (!safe.isEmpty() && (safe[0] == '.' || safe[0].isSpace())) {
        safe = safe.mid(1);
    }
    while (!safe.isEmpty() && (safe[safe.size() - 1] == '.' || safe[safe.size() - 1].isSpace())) {
        safe.chop(1);
    }

    // Reject remaining traversal names
    if (safe.isEmpty() || safe == "." || safe == "..") {
        return QStringLiteral("_unnamed");
    }

    return safe;
}

/**
 * @brief Verify that an absolute path stays within the given root directory.
 *
 * Cleans the path and checks that it starts with the cleaned root. Useful
 * after constructing a local path from a remote-supplied relative path.
 *
 * @param absolutePath  The constructed absolute path
 * @param rootDir       The sync root directory
 * @return true if the path is safely contained within rootDir
 */
inline bool isPathWithinRoot(const QString& absolutePath, const QString& rootDir) {
    QString cleanPath = QDir::cleanPath(absolutePath);
    QString cleanRoot = QDir::cleanPath(rootDir);

    // Ensure root ends with separator for prefix match
    if (!cleanRoot.endsWith('/')) {
        cleanRoot += '/';
    }

    return cleanPath.startsWith(cleanRoot) || cleanPath == QDir::cleanPath(rootDir);
}

}  // namespace PathUtils

#endif  // PATHUTILS_H
