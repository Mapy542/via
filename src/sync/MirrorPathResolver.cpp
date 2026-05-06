/**
 * @file MirrorPathResolver.cpp
 * @brief Shared helpers for resolving Drive names to unique mirror paths
 */

#include "MirrorPathResolver.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include "SyncDatabase.h"
#include "SyncSettings.h"
#include "utils/NativeDocSupport.h"
#include "utils/PathUtils.h"

namespace MirrorPathResolver {
namespace {

enum class DuplicateNameMode {
    FileIdSuffix,
    NumericSuffix,
};

QString normalizeRelativePath(const QString& path) {
    if (path.isEmpty()) {
        return QString();
    }

    QString normalized = QDir::cleanPath(path);
    if (normalized == ".") {
        normalized.clear();
    }
    return normalized;
}

QString parentPathFor(const QString& relativePath) {
    QString normalized = normalizeRelativePath(relativePath);
    if (normalized.isEmpty()) {
        return QString();
    }

    QString parent = QFileInfo(normalized).path();
    if (parent == "." || parent == "/") {
        return QString();
    }
    return normalizeRelativePath(parent);
}

QString joinPath(const QString& parentPath, const QString& fileName) {
    QString normalizedParent = normalizeRelativePath(parentPath);
    if (normalizedParent.isEmpty()) {
        return fileName;
    }
    return QDir(normalizedParent).filePath(fileName);
}

DuplicateNameMode duplicateNameMode(const SyncSettings& settings) {
    if (settings.duplicateNameStrategy == "numeric-suffix") {
        return DuplicateNameMode::NumericSuffix;
    }
    return DuplicateNameMode::FileIdSuffix;
}

QString buildDisambiguatedFileName(const QString& desiredFileName, DuplicateNameMode mode,
                                   const QString& fileId, int collisionIndex) {
    QFileInfo info(desiredFileName);
    const QString baseName = info.completeBaseName();
    const QString extension = info.suffix();

    QString decoratedBaseName;
    if (mode == DuplicateNameMode::NumericSuffix || fileId.isEmpty()) {
        decoratedBaseName = QString("%1 (%2)").arg(baseName).arg(collisionIndex);
    } else {
        decoratedBaseName = QString("%1_%2").arg(baseName, fileId);
        if (collisionIndex > 1) {
            decoratedBaseName += QString("_%1").arg(collisionIndex - 1);
        }
    }

    if (extension.isEmpty()) {
        return decoratedBaseName;
    }
    return QString("%1.%2").arg(decoratedBaseName, extension);
}

QString buildDisambiguatedPath(const QString& desiredLocalPath, DuplicateNameMode mode,
                               const QString& fileId, int collisionIndex) {
    const QString normalized = normalizeRelativePath(desiredLocalPath);
    const QString parentPath = parentPathFor(normalized);
    const QString fileName = QFileInfo(normalized).fileName();
    return joinPath(parentPath, buildDisambiguatedFileName(fileName, mode, fileId, collisionIndex));
}

bool isPathClaimed(const QString& localPath, const QString& fileId, const SyncDatabase* database,
                   const QString& syncFolder, const QSet<QString>* additionalClaims,
                   const QString& currentLocalPath) {
    const QString normalized = normalizeRelativePath(localPath);
    const QString normalizedCurrent = normalizeRelativePath(currentLocalPath);

    if (normalized == normalizedCurrent) {
        return false;
    }

    if (additionalClaims && additionalClaims->contains(normalized)) {
        return true;
    }

    if (database && !normalized.isEmpty()) {
        const QString mappedId = database->getFileId(normalized);
        if (!mappedId.isEmpty()) {
            return mappedId != fileId;
        }
    }

    if (!syncFolder.isEmpty() && !normalized.isEmpty()) {
        const QString absolutePath = QDir(syncFolder).filePath(normalized);
        if (PathUtils::isPathWithinRoot(absolutePath, syncFolder) &&
            QFileInfo::exists(absolutePath)) {
            return true;
        }
    }

    return false;
}

bool fileNameMatchesAlias(const QString& existingFileName, const QString& desiredFileName,
                          DuplicateNameMode mode, const QString& fileId) {
    if (existingFileName == desiredFileName) {
        return true;
    }

    const QFileInfo desiredInfo(desiredFileName);
    const QFileInfo existingInfo(existingFileName);
    if (desiredInfo.suffix() != existingInfo.suffix()) {
        return false;
    }

    const QString desiredBase = desiredInfo.completeBaseName();
    const QString existingBase = existingInfo.completeBaseName();
    if (desiredBase.isEmpty() || existingBase.isEmpty()) {
        return false;
    }

    if (mode == DuplicateNameMode::NumericSuffix) {
        const QRegularExpression pattern(
            QString("^%1 \\(([1-9]\\d*)\\)$").arg(QRegularExpression::escape(desiredBase)));
        return pattern.match(existingBase).hasMatch();
    }

    if (fileId.isEmpty()) {
        return false;
    }

    const QRegularExpression pattern(
        QString("^%1_%2(?:_(\\d+))?$")
            .arg(QRegularExpression::escape(desiredBase), QRegularExpression::escape(fileId)));
    return pattern.match(existingBase).hasMatch();
}

bool canReuseExistingMapping(const QString& existingPath, const QString& desiredLocalPath,
                             DuplicateNameMode mode, const QString& fileId) {
    const QString normalizedExisting = normalizeRelativePath(existingPath);
    const QString normalizedDesired = normalizeRelativePath(desiredLocalPath);
    if (normalizedExisting.isEmpty() || normalizedDesired.isEmpty()) {
        return false;
    }

    if (parentPathFor(normalizedExisting) != parentPathFor(normalizedDesired)) {
        return false;
    }

    return fileNameMatchesAlias(QFileInfo(normalizedExisting).fileName(),
                                QFileInfo(normalizedDesired).fileName(), mode, fileId);
}

}  // namespace

QString resolveUniqueLocalPath(const QString& desiredLocalPath, const QString& fileId,
                               const SyncDatabase* database, const SyncSettings& settings,
                               const QString& syncFolder, const QSet<QString>* additionalClaims,
                               const QString& currentLocalPath, bool reuseExistingMapping) {
    const QString normalizedDesired = normalizeRelativePath(desiredLocalPath);
    if (normalizedDesired.isEmpty()) {
        return normalizedDesired;
    }

    const DuplicateNameMode mode = duplicateNameMode(settings);
    if (reuseExistingMapping && database && !fileId.isEmpty()) {
        const QString existingPath = database->getLocalPath(fileId);
        if (canReuseExistingMapping(existingPath, normalizedDesired, mode, fileId)) {
            return normalizeRelativePath(existingPath);
        }
    }

    if (!isPathClaimed(normalizedDesired, fileId, database, syncFolder, additionalClaims,
                       currentLocalPath)) {
        return normalizedDesired;
    }

    for (int collisionIndex = 1; collisionIndex <= 1000; ++collisionIndex) {
        const QString candidate =
            buildDisambiguatedPath(normalizedDesired, mode, fileId, collisionIndex);
        if (!isPathClaimed(candidate, fileId, database, syncFolder, additionalClaims,
                           currentLocalPath)) {
            return candidate;
        }
    }

    return normalizedDesired;
}

QString resolveRemoteLocalPath(const QString& parentLocalPath, const QString& remoteName,
                               const QString& fileId, const SyncDatabase* database,
                               const SyncSettings& settings, const QString& syncFolder,
                               const QSet<QString>* additionalClaims, bool reuseExistingMapping) {
    const QString safeName = PathUtils::sanitizeRemoteFileName(remoteName);
    const QString desiredLocalPath = joinPath(parentLocalPath, safeName);
    return resolveUniqueLocalPath(desiredLocalPath, fileId, database, settings, syncFolder,
                                  additionalClaims, QString(), reuseExistingMapping);
}

QString resolveRemoteLocalPath(const QString& parentLocalPath, const QString& remoteName,
                               const QString& remoteMimeType, const QString& nativeDocModeOverride,
                               const QString& fileId, const SyncDatabase* database,
                               const SyncSettings& settings, const QString& syncFolder,
                               const QSet<QString>* additionalClaims, bool reuseExistingMapping) {
    const QString visibleRemoteName =
        nativeDocVisibleName(remoteName, remoteMimeType, nativeDocModeOverride,
                             nativeDocModeFromString(settings.nativeDocMode));
    return resolveRemoteLocalPath(parentLocalPath, visibleRemoteName, fileId, database, settings,
                                  syncFolder, additionalClaims, reuseExistingMapping);
}

}  // namespace MirrorPathResolver