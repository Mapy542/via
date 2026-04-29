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
#include <QFileInfo>
#include <QString>

namespace PathUtils {

enum class RecursiveRootRemovalAction {
    Refuse,
    RemoveSymlinkOnly,
    RemoveRecursively,
};

struct RecursiveRootRemovalDecision {
    RecursiveRootRemovalAction action = RecursiveRootRemovalAction::Refuse;
    QString absolutePath;
    QString canonicalPath;
    int depth = 0;
};

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
 * @brief Check whether a path refers to a symbolic link without following it.
 *
 * @param path  Filesystem path to inspect
 * @return true if the path entry exists and is a symlink
 */
inline bool isSymlink(const QString& path) { return QFileInfo(path).isSymLink(); }

/**
 * @brief Check whether a QFileInfo refers to a symbolic link without following it.
 *
 * @param fileInfo  Metadata to inspect
 * @return true if the entry is a symlink
 */
inline bool isSymlink(const QFileInfo& fileInfo) { return fileInfo.isSymLink(); }

/**
 * @brief Check whether a candidate path is exactly the root or a child of it.
 *
 * Unlike a plain startsWith check, this requires a path-separator boundary so
 * sibling prefixes such as "/sync.Trash-1000" do not count as being inside
 * "/sync".
 *
 * @param candidatePath  Path being validated
 * @param rootDir        Root directory boundary
 * @return true if candidatePath is rootDir itself or nested beneath it
 */
inline bool isPathWithinRootBoundary(const QString& candidatePath, const QString& rootDir) {
    if (candidatePath.isEmpty() || rootDir.isEmpty()) {
        return false;
    }

    const QString cleanCandidate = QDir::cleanPath(candidatePath);
    const QString cleanRoot = QDir::cleanPath(rootDir);

    if (cleanCandidate == cleanRoot) {
        return true;
    }

    if (cleanRoot == QStringLiteral("/")) {
        return cleanCandidate.startsWith('/');
    }

    return cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'));
}

/**
 * @brief Convert an absolute path to a root-relative path when it stays inside the root.
 *
 * This combines the root-boundary containment check with relative path generation so
 * callers can share the same safety logic while keeping their own fallback behavior.
 *
 * @param candidatePath  Absolute path being converted
 * @param rootDir        Root directory boundary
 * @param relativePath   Output for the relative path; cleared on failure
 * @return true if candidatePath is rootDir itself or nested beneath it
 */
inline bool tryGetRelativePathWithinRoot(const QString& candidatePath, const QString& rootDir,
                                         QString* relativePath) {
    if (relativePath == nullptr) {
        return false;
    }

    relativePath->clear();
    if (!isPathWithinRootBoundary(candidatePath, rootDir)) {
        return false;
    }

    const QString cleanCandidate = QDir::cleanPath(candidatePath);
    const QString cleanRoot = QDir::cleanPath(rootDir);

    QString relative = QDir(cleanRoot).relativeFilePath(cleanCandidate);
    if (relative == QStringLiteral(".")) {
        relative.clear();
    }

    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../"))) {
        return false;
    }

    *relativePath = relative;
    return true;
}

/**
 * @brief Resolve a path to its canonical location if it exists.
 *
 * This resolves symlink targets and normalizes the resulting absolute path.
 *
 * @param path  Existing filesystem path
 * @return Canonical absolute path, or an empty string if unavailable
 */
inline QString canonicalPathIfExists(const QString& path) {
    QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        return QString();
    }

    return fileInfo.canonicalFilePath();
}

/**
 * @brief Check whether an existing path resolves within an existing root.
 *
 * Useful for symlink-aware callers that need to validate the canonical target
 * before recursing.
 *
 * @param path     Existing path that may be a symlink
 * @param rootDir  Existing allowed root directory
 * @return true if both canonical paths are available and the resolved path is inside the root
 */
inline bool isCanonicalPathWithinRoot(const QString& path, const QString& rootDir) {
    const QString canonicalPath = canonicalPathIfExists(path);
    const QString canonicalRoot = canonicalPathIfExists(rootDir);

    if (canonicalPath.isEmpty() || canonicalRoot.isEmpty()) {
        return false;
    }

    return isPathWithinRootBoundary(canonicalPath, canonicalRoot);
}

/**
 * @brief Decide whether a root path may be removed recursively.
 *
 * The decision preserves the existing dangerous-path guard (root, home, or
 * shallow paths are refused) and adds symlink-aware handling: a symlink root is
 * removed as a leaf only, and any path that resolves outside its lexical root
 * boundary is refused.
 *
 * @param rootPath  Existing root path being considered for recursive removal
 * @return Removal decision with normalized paths for logging
 */
inline RecursiveRootRemovalDecision classifyRecursiveRootRemoval(const QString& rootPath) {
    RecursiveRootRemovalDecision decision;

    if (rootPath.isEmpty()) {
        return decision;
    }

    const QFileInfo rootInfo(rootPath);
    decision.absolutePath = QDir::cleanPath(rootInfo.absoluteFilePath());
    decision.canonicalPath = canonicalPathIfExists(rootPath);
    decision.depth = decision.absolutePath.split('/', Qt::SkipEmptyParts).size();

    const QString homePath = QDir::cleanPath(QDir::homePath());
    if (decision.absolutePath.isEmpty() || decision.absolutePath == QStringLiteral("/") ||
        decision.absolutePath == homePath || decision.depth < 3) {
        return decision;
    }

    if (isSymlink(rootInfo)) {
        decision.action = RecursiveRootRemovalAction::RemoveSymlinkOnly;
        return decision;
    }

    if (decision.canonicalPath.isEmpty()) {
        return decision;
    }

    if (!isPathWithinRootBoundary(decision.canonicalPath, decision.absolutePath)) {
        return decision;
    }

    decision.action = RecursiveRootRemovalAction::RemoveRecursively;
    return decision;
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
    return isPathWithinRootBoundary(absolutePath, rootDir);
}

}  // namespace PathUtils

#endif  // PATHUTILS_H
