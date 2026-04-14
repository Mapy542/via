/**
 * @file TrashPolicy.cpp
 * @brief FreeDesktop trash path predicates and trash intent classification
 */

#include "TrashPolicy.h"

#include <QDir>
#include <QRegularExpression>

namespace {

// Matches ".Trash-<digits>" as a single path component
static const QRegularExpression kTrashDirRe(QStringLiteral("^\\.Trash-\\d+$"));

/**
 * @brief Split @p relativePath into its first component and the remainder.
 *
 * For "a/b/c" returns ("a", "b/c").
 * For "a"     returns ("a", "").
 */
std::pair<QString, QString> splitFirst(const QString& relativePath) {
    int sep = relativePath.indexOf('/');
    if (sep < 0) {
        return {relativePath, QString()};
    }
    return {relativePath.left(sep), relativePath.mid(sep + 1)};
}

}  // namespace

bool TrashPolicy::isTrashRelativePath(const QString& relativePath) {
    if (relativePath.isEmpty()) {
        return false;
    }
    auto [first, rest] = splitFirst(relativePath);
    Q_UNUSED(rest);
    return kTrashDirRe.match(first).hasMatch();
}

bool TrashPolicy::isTrashPath(const QString& absolutePath, const QString& syncRoot) {
    QString canonical = QDir::cleanPath(absolutePath);
    QString root = QDir::cleanPath(syncRoot);

    if (!canonical.startsWith(root)) {
        return false;
    }

    // Strip the root prefix + separator
    QString relative = canonical.mid(root.length());
    if (relative.startsWith('/')) {
        relative = relative.mid(1);
    }

    return isTrashRelativePath(relative);
}

bool TrashPolicy::isMoveToTrash(const QString& fromAbsolute, const QString& toAbsolute, const QString& syncRoot) {
    return !isTrashPath(fromAbsolute, syncRoot) && isTrashPath(toAbsolute, syncRoot);
}

bool TrashPolicy::isRestoreFromTrash(const QString& fromAbsolute, const QString& toAbsolute, const QString& syncRoot) {
    return isTrashPath(fromAbsolute, syncRoot) && !isTrashPath(toAbsolute, syncRoot);
}
