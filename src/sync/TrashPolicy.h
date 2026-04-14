/**
 * @file TrashPolicy.h
 * @brief FreeDesktop trash path predicates and trash intent classification
 *
 * Provides reusable helpers shared by the mirror-sync pipeline and the
 * FUSE driver to recognise FreeDesktop-style trash directories and to
 * classify file operations as trash, restore, or ordinary.
 *
 * The path format recognised is:
 *     <syncRoot>/.Trash-<uid>/files/...
 *     <syncRoot>/.Trash-<uid>/info/...
 *
 * Only top-level .Trash-<uid> directories under the sync root are
 * treated as trash roots.  Nested occurrences are ignored.
 */

#ifndef TRASHPOLICY_H
#define TRASHPOLICY_H

#include <QString>

namespace TrashPolicy {

/**
 * @brief Check whether an absolute path is inside a FreeDesktop trash
 *        subtree directly under @p syncRoot.
 *
 * Returns true for paths like:
 *   <syncRoot>/.Trash-1000
 *   <syncRoot>/.Trash-1000/files/foo.txt
 *   <syncRoot>/.Trash-1000/info/foo.txt.trashinfo
 */
bool isTrashPath(const QString& absolutePath, const QString& syncRoot);

/**
 * @brief Check whether a relative path (relative to the sync root)
 *        falls inside a .Trash-<uid> subtree.
 */
bool isTrashRelativePath(const QString& relativePath);

/**
 * @brief Check whether a rename/move represents live → trash intent.
 *
 * Both arguments must be absolute paths.
 */
bool isMoveToTrash(const QString& fromAbsolute, const QString& toAbsolute, const QString& syncRoot);

/**
 * @brief Check whether a rename/move represents trash → live restore.
 *
 * Both arguments must be absolute paths.
 */
bool isRestoreFromTrash(const QString& fromAbsolute, const QString& toAbsolute, const QString& syncRoot);

}  // namespace TrashPolicy

#endif  // TRASHPOLICY_H
