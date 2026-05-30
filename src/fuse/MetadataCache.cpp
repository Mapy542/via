/**
 * @file MetadataCache.cpp
 * @brief Implementation of in-memory metadata cache for FUSE filesystem
 */

#include "MetadataCache.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QReadLocker>
#include <QSet>
#include <QSettings>
#include <QWriteLocker>
#include <algorithm>

#include "api/GoogleDriveClient.h"
#include "sync/SyncDatabase.h"
#include "sync/SyncSettings.h"
#include "sync/TrashPolicy.h"
#include "utils/NativeDocSupport.h"

const int MetadataCache::DEFAULT_MAX_CACHE_AGE_SECONDS;

namespace {

QString normalizeMetadataPath(const QString& path) {
    if (path.isEmpty()) {
        return QString();
    }

    QString normalized = QDir::cleanPath(path);
    if (normalized == ".") {
        normalized.clear();
    }
    if (normalized.startsWith(QStringLiteral("/")) && normalized != QStringLiteral("/")) {
        normalized.remove(0, 1);
    }
    return normalized;
}

QString parentPathFor(const QString& path) {
    const QString normalized = normalizeMetadataPath(path);
    if (normalized.isEmpty() || normalized == QStringLiteral("/")) {
        return QStringLiteral("/");
    }

    const int lastSlash = normalized.lastIndexOf('/');
    if (lastSlash <= 0) {
        return QStringLiteral("/");
    }

    return normalized.left(lastSlash);
}

QString joinMetadataPath(const QString& parentPath, const QString& fileName) {
    const QString normalizedParent = normalizeMetadataPath(parentPath);
    if (normalizedParent.isEmpty() || normalizedParent == QStringLiteral("/")) {
        return fileName;
    }
    return QDir(normalizedParent).filePath(fileName);
}

QString fileNameForPath(const QString& path) {
    const QString normalized = normalizeMetadataPath(path);
    if (normalized.isEmpty() || normalized == QStringLiteral("/")) {
        return QString();
    }

    const int lastSlash = normalized.lastIndexOf('/');
    if (lastSlash < 0) {
        return normalized;
    }
    return normalized.mid(lastSlash + 1);
}

QString remoteNameForMetadata(const FuseFileMetadata& metadata) {
    if (!metadata.remoteName.isEmpty()) {
        return metadata.remoteName;
    }
    if (!metadata.name.isEmpty()) {
        return metadata.name;
    }
    return fileNameForPath(metadata.path);
}

QString visibleNameForMetadata(const FuseFileMetadata& metadata) {
    if (!metadata.name.isEmpty()) {
        return metadata.name;
    }
    return remoteNameForMetadata(metadata);
}

NativeDocMode globalNativeDocModeSetting() {
    QSettings settings;
    return nativeDocModeFromString(settings.value("advanced/nativeDocMode", "hide").toString());
}

bool isDigitsOnly(QStringView value) {
    if (value.isEmpty()) {
        return false;
    }

    for (const QChar ch : value) {
        if (!ch.isDigit()) {
            return false;
        }
    }

    return true;
}

bool existingPathMatchesVisibleName(const QString& existingPath, const QString& visibleName,
                                    const QString& duplicateNameStrategy, const QString& fileId) {
    const QString existingName = fileNameForPath(existingPath);
    if (existingName == visibleName) {
        return true;
    }

    const QFileInfo desiredInfo(visibleName);
    const QFileInfo existingInfo(existingName);
    if (desiredInfo.suffix() != existingInfo.suffix()) {
        return false;
    }

    const QString desiredBase = desiredInfo.completeBaseName();
    const QString existingBase = existingInfo.completeBaseName();

    if (duplicateNameStrategy == QStringLiteral("numeric-suffix") || fileId.isEmpty()) {
        const QString prefix = desiredBase + QStringLiteral(" (");
        if (!existingBase.startsWith(prefix) || !existingBase.endsWith(QLatin1Char(')'))) {
            return false;
        }

        const QStringView suffix =
            QStringView(existingBase).mid(prefix.size(), existingBase.size() - prefix.size() - 1);
        return isDigitsOnly(suffix);
    }

    const QString prefix = desiredBase + QLatin1Char('_') + fileId;
    if (existingBase == prefix) {
        return true;
    }
    if (!existingBase.startsWith(prefix + QLatin1Char('_'))) {
        return false;
    }

    const QStringView suffix = QStringView(existingBase).mid(prefix.size() + 1);
    return isDigitsOnly(suffix);
}

bool shouldExposeRemoteFile(const DriveFile& file,
                            const QString& nativeDocModeOverride = QString()) {
    if (!file.isValid()) {
        return false;
    }
    if (file.isShortcut) {
        return false;
    }
    if (file.isGoogleDoc() && !file.isFolder) {
        const NativeDocRepresentation rep = effectiveNativeDocRepresentation(
            file.mimeType, nativeDocModeOverride, globalNativeDocModeSetting());
        return rep.visible;
    }
    return true;
}

FuseFileMetadata fromDbMetadata(const FuseMetadata& dbMeta) {
    FuseFileMetadata metadata;
    metadata.fileId = dbMeta.fileId;
    metadata.path = normalizeMetadataPath(dbMeta.path);
    metadata.name = dbMeta.name;
    metadata.remoteName = dbMeta.remoteName.isEmpty() ? dbMeta.name : dbMeta.remoteName;
    metadata.nativeDocModeOverride = dbMeta.nativeDocModeOverride;
    metadata.parentId = dbMeta.parentId;
    metadata.isFolder = dbMeta.isFolder;
    metadata.size = dbMeta.size;
    metadata.mimeType = dbMeta.mimeType;
    metadata.remoteMimeType = dbMeta.remoteMimeType;
    metadata.webViewLink = dbMeta.webViewLink;
    metadata.createdTime = dbMeta.createdTime;
    metadata.modifiedTime = dbMeta.modifiedTime;
    metadata.cachedAt = dbMeta.cachedAt;
    metadata.lastAccessed = dbMeta.lastAccessed;
    return metadata;
}

FuseMetadata toDbMetadata(const FuseFileMetadata& metadata) {
    FuseMetadata dbMeta;
    dbMeta.fileId = metadata.fileId;
    dbMeta.path = normalizeMetadataPath(metadata.path);
    dbMeta.name = metadata.name;
    dbMeta.remoteName = remoteNameForMetadata(metadata);
    dbMeta.nativeDocModeOverride = metadata.nativeDocModeOverride;
    dbMeta.parentId = metadata.parentId;
    dbMeta.isFolder = metadata.isFolder;
    dbMeta.size = metadata.size;
    dbMeta.mimeType = metadata.mimeType;
    dbMeta.remoteMimeType = metadata.remoteMimeType;
    dbMeta.webViewLink = metadata.webViewLink;
    dbMeta.createdTime = metadata.createdTime;
    dbMeta.modifiedTime = metadata.modifiedTime;
    dbMeta.cachedAt = metadata.cachedAt;
    dbMeta.lastAccessed = metadata.lastAccessed;
    return dbMeta;
}

FuseFileMetadata fromDriveFile(const DriveFile& file,
                               const QString& nativeDocModeOverride = QString()) {
    FuseFileMetadata metadata;
    metadata.fileId = file.id;
    metadata.name = file.name;
    metadata.remoteName = file.name;
    metadata.nativeDocModeOverride = nativeDocModeOverride;
    metadata.parentId = file.parentId();
    metadata.isFolder = file.isFolder;
    metadata.size = file.size;
    metadata.mimeType = file.mimeType;
    metadata.webViewLink = file.webViewLink;
    // Preserve the original Google-native MIME type for native docs so the
    // representation policy can make decisions later.
    if (file.isGoogleDoc() && !file.isFolder) {
        metadata.remoteMimeType = file.mimeType;

        // Append pseudo-extension based on current serving mode so the
        // FUSE-visible name reflects the representation format.
        const NativeDocRepresentation rep = effectiveNativeDocRepresentation(
            file.mimeType, nativeDocModeOverride, globalNativeDocModeSetting());
        if (rep.visible && !rep.extension.isEmpty()) {
            metadata.name = nativeDocVisibleName(file.name, rep);
        }
    }
    metadata.createdTime = file.createdTime;
    metadata.modifiedTime = file.modifiedTime;
    metadata.cachedAt = QDateTime::currentDateTime();
    metadata.lastAccessed = QDateTime::currentDateTime();
    return metadata;
}

QString buildDisambiguatedFileName(const QString& remoteName, const QString& duplicateNameStrategy,
                                   const QString& fileId, int collisionIndex) {
    const QFileInfo info(remoteName);
    const QString baseName = info.completeBaseName();
    const QString extension = info.suffix();

    QString decoratedBaseName;
    if (duplicateNameStrategy == QStringLiteral("numeric-suffix") || fileId.isEmpty()) {
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

FuseFileMetadata resolveRemoteMetadata(const FuseFileMetadata& metadata, const QString& parentPath,
                                       const QHash<QString, FuseFileMetadata>& existingByFileId,
                                       QSet<QString>* claimedPaths,
                                       const QString& duplicateNameStrategy) {
    FuseFileMetadata resolved = metadata;
    resolved.path.clear();

    const QString remoteName = remoteNameForMetadata(metadata);
    const QString visibleName = visibleNameForMetadata(metadata);
    if (metadata.fileId.isEmpty() || remoteName.isEmpty() || visibleName.isEmpty() ||
        parentPath.isEmpty()) {
        return FuseFileMetadata();
    }

    resolved.remoteName = remoteName;

    const auto existingIt = existingByFileId.constFind(metadata.fileId);
    if (existingIt != existingByFileId.constEnd()) {
        const FuseFileMetadata& existing = existingIt.value();
        const QString existingPath = normalizeMetadataPath(existing.path);
        if (!existingPath.isEmpty() && parentPathFor(existingPath) == parentPath &&
            remoteNameForMetadata(existing) == remoteName &&
            existingPathMatchesVisibleName(existingPath, visibleName, duplicateNameStrategy,
                                           metadata.fileId) &&
            (!claimedPaths || !claimedPaths->contains(existingPath))) {
            resolved.path = existingPath;
            resolved.name = fileNameForPath(existingPath);
            if (claimedPaths) {
                claimedPaths->insert(existingPath);
            }
            return resolved;
        }
    }

    QString candidatePath = joinMetadataPath(parentPath, visibleName);
    if (!claimedPaths || !claimedPaths->contains(candidatePath)) {
        resolved.path = candidatePath;
        resolved.name = fileNameForPath(candidatePath);
        if (claimedPaths) {
            claimedPaths->insert(candidatePath);
        }
        return resolved;
    }

    for (int collisionIndex = 1;; ++collisionIndex) {
        const QString candidateName = buildDisambiguatedFileName(visibleName, duplicateNameStrategy,
                                                                 metadata.fileId, collisionIndex);
        candidatePath = joinMetadataPath(parentPath, candidateName);
        if (!claimedPaths || !claimedPaths->contains(candidatePath)) {
            resolved.path = candidatePath;
            resolved.name = candidateName;
            if (claimedPaths) {
                claimedPaths->insert(candidatePath);
            }
            return resolved;
        }
    }
}

bool pathIsWithinSubtree(const QString& candidatePath, const QString& rootPath) {
    const QString normalizedCandidate = normalizeMetadataPath(candidatePath);
    const QString normalizedRoot = normalizeMetadataPath(rootPath);
    if (normalizedCandidate.isEmpty() || normalizedRoot.isEmpty()) {
        return false;
    }
    if (normalizedCandidate == normalizedRoot) {
        return true;
    }
    return normalizedCandidate.startsWith(normalizedRoot + QStringLiteral("/"));
}

QString existingNativeDocModeOverride(const MetadataCache* cache, SyncDatabase* database,
                                      const QString& fileId) {
    if (!cache || fileId.isEmpty()) {
        return QString();
    }

    const FuseFileMetadata cached = cache->getMetadataByFileId(fileId);
    if (cached.isValid()) {
        return cached.nativeDocModeOverride;
    }

    if (!database) {
        return QString();
    }

    return database->getNativeDocState(fileId).nativeDocModeOverride;
}

}  // namespace

MetadataCache::MetadataCache(SyncDatabase* database, GoogleDriveClient* driveClient,
                             QObject* parent)
    : QObject(parent),
      m_database(database),
      m_driveClient(driveClient),
      m_duplicateNameStrategy(SyncSettings::load().duplicateNameStrategy),
      m_maxCacheAgeSeconds(DEFAULT_MAX_CACHE_AGE_SECONDS),
      m_cacheHits(0),
      m_cacheMisses(0) {
    // Connect to Google Drive client signals for async metadata fetch
    if (m_driveClient) {
        connect(
            m_driveClient, &GoogleDriveClient::fileReceived, this,
            [this](const DriveFile& file) { onApiMetadataReceived(file.id, fromDriveFile(file)); });
    }
}

MetadataCache::~MetadataCache() = default;

bool MetadataCache::initialize() {
    if (!m_database || !m_database->isOpen()) {
        qWarning() << "MetadataCache: Database not available or not open";
        emit cacheError("Database not available");
        return false;
    }

    // L2 fix: Table creation is handled by SyncDatabase::createFuseTables().
    // We no longer duplicate the DDL here.

    // Perform lightweight startup maintenance without mirroring the full metadata table in RAM.
    loadFromDatabase();

    qInfo() << "MetadataCache initialized with" << m_pathToMetadata.size() << "entries";
    return true;
}

// ========================================
// Path-based lookups
// ========================================

FuseFileMetadata MetadataCache::getMetadataByPath(const QString& path) const {
    QReadLocker locker(&m_lock);

    auto it = m_pathToMetadata.constFind(path);
    if (it != m_pathToMetadata.constEnd()) {
        m_cacheHits++;
        return it.value();
    }

    m_cacheMisses++;
    return FuseFileMetadata();
}

FuseFileMetadata MetadataCache::getOrFetchMetadataByPath(const QString& path, bool* fetched) {
    if (fetched) {
        *fetched = false;
    }

    // First, try in-memory cache
    {
        QReadLocker locker(&m_lock);
        auto it = m_pathToMetadata.constFind(path);
        if (it != m_pathToMetadata.constEnd() && !it->isStale(m_maxCacheAgeSeconds)) {
            m_cacheHits++;
            return it.value();
        }
    }

    // Try database via SyncDatabase (thread-safe)
    if (m_database) {
        FuseMetadata dbMeta = m_database->getFuseMetadataByPath(path);
        if (!dbMeta.fileId.isEmpty()) {
            FuseFileMetadata metadata = fromDbMetadata(dbMeta);

            // Update in-memory cache
            {
                QWriteLocker locker(&m_lock);
                m_pathToMetadata[metadata.path] = metadata;
                m_fileIdToPath[metadata.fileId] = metadata.path;
            }

            if (fetched) {
                *fetched = true;
            }

            m_cacheHits++;
            return metadata;
        }
    }

    // Not found in cache or database - would need API fetch
    // For now, return invalid metadata; caller can decide to fetch async
    m_cacheMisses++;
    return FuseFileMetadata();
}

bool MetadataCache::hasPath(const QString& path) const {
    QReadLocker locker(&m_lock);
    return m_pathToMetadata.contains(path);
}

// ========================================
// FileId-based lookups
// ========================================

FuseFileMetadata MetadataCache::getMetadataByFileId(const QString& fileId) const {
    {
        QReadLocker locker(&m_lock);

        auto pathIt = m_fileIdToPath.constFind(fileId);
        if (pathIt != m_fileIdToPath.constEnd()) {
            auto metaIt = m_pathToMetadata.constFind(pathIt.value());
            if (metaIt != m_pathToMetadata.constEnd()) {
                m_cacheHits++;
                return metaIt.value();
            }
        }
    }

    if (m_database) {
        const FuseMetadata dbMeta = m_database->getFuseMetadata(fileId);
        if (!dbMeta.fileId.isEmpty()) {
            m_cacheHits++;
            return fromDbMetadata(dbMeta);
        }
    }

    m_cacheMisses++;
    return FuseFileMetadata();
}

QString MetadataCache::getFileIdByPath(const QString& path) const {
    QReadLocker locker(&m_lock);

    auto it = m_pathToMetadata.constFind(path);
    if (it != m_pathToMetadata.constEnd()) {
        return it->fileId;
    }

    return QString();
}

QString MetadataCache::getPathByFileId(const QString& fileId) const {
    {
        QReadLocker locker(&m_lock);

        auto it = m_fileIdToPath.constFind(fileId);
        if (it != m_fileIdToPath.constEnd()) {
            return it.value();
        }
    }

    if (m_database) {
        const FuseMetadata dbMeta = m_database->getFuseMetadata(fileId);
        if (!dbMeta.fileId.isEmpty()) {
            return dbMeta.path;
        }
    }

    return QString();
}

// ========================================
// Directory operations
// ========================================

QList<FuseFileMetadata> MetadataCache::getChildren(const QString& parentPath) const {
    QReadLocker locker(&m_lock);

    QList<FuseFileMetadata> result;
    auto childrenIt = m_parentToChildren.constFind(parentPath);
    if (childrenIt != m_parentToChildren.constEnd()) {
        for (const QString& childPath : childrenIt.value()) {
            auto metaIt = m_pathToMetadata.constFind(childPath);
            if (metaIt != m_pathToMetadata.constEnd()) {
                result.append(metaIt.value());
            }
        }
    }

    return result;
}

QList<FuseFileMetadata> MetadataCache::getOrFetchChildren(const QString& parentPath,
                                                          bool* fetched) {
    if (fetched) {
        *fetched = false;
    }

    // Fresh warm listings can be served directly. If we already know a directory listing is stale,
    // let the caller decide whether it wants to refresh from the API instead of re-serving SQLite.
    bool hadDirectoryState = false;
    {
        QReadLocker locker(&m_lock);
        auto timeIt = m_childrenCacheTime.constFind(parentPath);
        if (timeIt != m_childrenCacheTime.constEnd()) {
            hadDirectoryState = true;
            if (timeIt->secsTo(QDateTime::currentDateTime()) < m_maxCacheAgeSeconds) {
                return getChildren(parentPath);
            }
        }
    }

    if (hadDirectoryState) {
        return QList<FuseFileMetadata>();
    }

    // Cold directory lookup: try SQLite before asking Drive for a fresh listing.
    QString parentId;
    bool parentKnown = false;
    if (parentPath.isEmpty() || parentPath == "/") {
        parentId = m_rootFolderId;
    } else {
        {
            QReadLocker locker(&m_lock);
            auto it = m_pathToMetadata.constFind(parentPath);
            if (it != m_pathToMetadata.constEnd()) {
                parentId = it->fileId;
                parentKnown = true;
            }
        }

        if (!parentKnown && m_database) {
            const FuseMetadata dbParentMeta = m_database->getFuseMetadataByPath(parentPath);
            if (!dbParentMeta.fileId.isEmpty()) {
                const FuseFileMetadata parentMetadata = fromDbMetadata(dbParentMeta);
                parentId = parentMetadata.fileId;
                parentKnown = true;

                QWriteLocker locker(&m_lock);
                m_pathToMetadata[parentMetadata.path] = parentMetadata;
                m_fileIdToPath[parentMetadata.fileId] = parentMetadata.path;
            }
        }
    }

    if (!parentId.isEmpty() && m_database) {
        const QList<FuseMetadata> dbChildren = m_database->getFuseChildren(parentId);

        if (!dbChildren.isEmpty() || (parentKnown && parentPath != "/")) {
            QList<FuseFileMetadata> children;
            children.reserve(dbChildren.size());
            for (const FuseMetadata& dbMeta : dbChildren) {
                children.append(fromDbMetadata(dbMeta));
            }

            // Update in-memory cache
            QWriteLocker locker(&m_lock);
            QList<QString> childPaths;
            childPaths.reserve(children.size());
            for (const FuseFileMetadata& child : children) {
                m_pathToMetadata[child.path] = child;
                m_fileIdToPath[child.fileId] = child.path;
                childPaths.append(child.path);
            }
            m_parentToChildren[parentPath] = childPaths;
            m_childrenCacheTime[parentPath] = QDateTime::currentDateTime();

            if (fetched) {
                *fetched = true;
            }

            return children;
        }
    }

    // Not found - return empty list; caller can trigger async API fetch
    return QList<FuseFileMetadata>();
}

bool MetadataCache::hasChildrenCached(const QString& parentPath) const {
    QReadLocker locker(&m_lock);

    auto timeIt = m_childrenCacheTime.constFind(parentPath);
    if (timeIt != m_childrenCacheTime.constEnd()) {
        return timeIt->secsTo(QDateTime::currentDateTime()) < m_maxCacheAgeSeconds;
    }

    return false;
}

FuseFileMetadata MetadataCache::upsertRemoteMetadata(const DriveFile& file) {
    const QString nativeDocModeOverride = existingNativeDocModeOverride(this, m_database, file.id);
    if (!shouldExposeRemoteFile(file, nativeDocModeOverride)) {
        return FuseFileMetadata();
    }

    return upsertRemoteMetadataInternal(fromDriveFile(file, nativeDocModeOverride));
}

QList<FuseFileMetadata> MetadataCache::replaceRemoteChildren(const QString& parentId,
                                                             const QList<DriveFile>& files) {
    QList<FuseFileMetadata> children;
    children.reserve(files.size());

    for (const DriveFile& file : files) {
        const QString nativeDocModeOverride =
            existingNativeDocModeOverride(this, m_database, file.id);
        if (!shouldExposeRemoteFile(file, nativeDocModeOverride)) {
            continue;
        }
        children.append(fromDriveFile(file, nativeDocModeOverride));
    }

    return replaceRemoteChildrenInternal(parentId, children);
}

FuseFileMetadata MetadataCache::applyNativeDocModeOverride(const QString& fileId,
                                                           const QString& modeOverride,
                                                           QString* previousPathOut) {
    FuseFileMetadata metadata = getMetadataByFileId(fileId);
    if (!metadata.isValid() && m_database) {
        const FuseMetadata dbMeta = m_database->getFuseMetadata(fileId);
        if (!dbMeta.fileId.isEmpty()) {
            metadata = fromDbMetadata(dbMeta);
        }
    }

    if (!metadata.isValid() || metadata.isFolder || metadata.remoteMimeType.isEmpty()) {
        return FuseFileMetadata();
    }

    if (previousPathOut) {
        *previousPathOut = normalizeMetadataPath(metadata.path);
    }

    metadata.nativeDocModeOverride = modeOverride;

    const NativeDocRepresentation rep = effectiveNativeDocRepresentation(
        metadata.remoteMimeType, modeOverride, globalNativeDocModeSetting());
    if (!rep.visible) {
        return FuseFileMetadata();
    }

    const QString remoteName = remoteNameForMetadata(metadata);
    metadata.name = nativeDocVisibleName(remoteName, rep);
    metadata.cachedAt = QDateTime::currentDateTime();
    metadata.lastAccessed = QDateTime::currentDateTime();

    return upsertRemoteMetadataInternal(metadata);
}

// ========================================
// Cache modification
// ========================================

void MetadataCache::setMetadata(const FuseFileMetadata& metadata, bool persistToDatabase) {
    FuseFileMetadata normalized = metadata;
    normalized.path = normalizeMetadataPath(metadata.path);
    if (normalized.remoteName.isEmpty()) {
        normalized.remoteName = normalized.name;
    }
    if (normalized.name.isEmpty()) {
        normalized.name = fileNameForPath(normalized.path);
    }

    if (!normalized.isValid()) {
        qWarning() << "MetadataCache: Cannot store invalid metadata";
        return;
    }

    // Update in-memory cache
    {
        QWriteLocker locker(&m_lock);

        // Remove old path mapping if file ID exists with different path
        auto oldPathIt = m_fileIdToPath.constFind(normalized.fileId);
        if (oldPathIt != m_fileIdToPath.constEnd() && oldPathIt.value() != normalized.path) {
            m_pathToMetadata.remove(oldPathIt.value());

            // Update parent's children list
            for (auto& children : m_parentToChildren) {
                children.removeAll(oldPathIt.value());
            }
        }

        // Clean up orphaned fileId mapping when a different fileId already
        // occupies this path (Google Drive allows duplicate names).
        auto existingIt = m_pathToMetadata.constFind(normalized.path);
        if (existingIt != m_pathToMetadata.constEnd() && existingIt->fileId != normalized.fileId) {
            m_fileIdToPath.remove(existingIt->fileId);
        }

        m_pathToMetadata[normalized.path] = normalized;
        m_fileIdToPath[normalized.fileId] = normalized.path;

        // Update parent's children list
        const QString parentPath = getParentPath(normalized.path);

        if (!m_parentToChildren[parentPath].contains(normalized.path)) {
            m_parentToChildren[parentPath].append(normalized.path);
        }
    }

    if (persistToDatabase) {
        saveToDatabase(normalized);
    }

    emit metadataUpdated(normalized.path);
}

void MetadataCache::setMetadataBatch(const QList<FuseFileMetadata>& metadataList) {
    if (metadataList.isEmpty()) {
        return;
    }

    QWriteLocker locker(&m_lock);

    for (const FuseFileMetadata& metadata : metadataList) {
        FuseFileMetadata normalized = metadata;
        normalized.path = normalizeMetadataPath(metadata.path);
        if (normalized.remoteName.isEmpty()) {
            normalized.remoteName = normalized.name;
        }
        if (normalized.name.isEmpty()) {
            normalized.name = fileNameForPath(normalized.path);
        }

        if (!normalized.isValid()) {
            continue;
        }

        // Update in-memory cache
        auto oldPathIt = m_fileIdToPath.constFind(normalized.fileId);
        if (oldPathIt != m_fileIdToPath.constEnd() && oldPathIt.value() != normalized.path) {
            m_pathToMetadata.remove(oldPathIt.value());

            // Also remove stale path from parent's children list
            for (auto& children : m_parentToChildren) {
                children.removeAll(oldPathIt.value());
            }
        }

        // Clean up orphaned fileId mapping when a different fileId already
        // occupies this path (Google Drive allows duplicate names).
        auto existingIt = m_pathToMetadata.constFind(normalized.path);
        if (existingIt != m_pathToMetadata.constEnd() && existingIt->fileId != normalized.fileId) {
            m_fileIdToPath.remove(existingIt->fileId);
        }

        m_pathToMetadata[normalized.path] = normalized;
        m_fileIdToPath[normalized.fileId] = normalized.path;

        // Update parent's children list
        const QString parentPath = getParentPath(normalized.path);

        if (!m_parentToChildren[parentPath].contains(normalized.path)) {
            m_parentToChildren[parentPath].append(normalized.path);
        }

        // Save to database via SyncDatabase (thread-safe)
        if (m_database) {
            m_database->saveFuseMetadata(toDbMetadata(normalized));
        }
    }

    qDebug() << "MetadataCache: Batch stored" << metadataList.size() << "entries";
}

void MetadataCache::removeByPath(const QString& path) {
    QString fileId;

    {
        QWriteLocker locker(&m_lock);

        auto it = m_pathToMetadata.constFind(path);
        if (it == m_pathToMetadata.constEnd()) {
            return;
        }

        fileId = it->fileId;

        m_pathToMetadata.remove(path);
        m_fileIdToPath.remove(fileId);

        // Remove from parent's children list
        for (auto& children : m_parentToChildren) {
            children.removeAll(path);
        }

        // Remove children list if this was a directory
        m_parentToChildren.remove(path);
        m_childrenCacheTime.remove(path);
    }

    // Remove from database
    removeFromDatabase(fileId);

    emit metadataRemoved(path);
}

void MetadataCache::removeByFileId(const QString& fileId, bool removeFromDatabaseFlag) {
    QWriteLocker locker(&m_lock);

    auto pathIt = m_fileIdToPath.constFind(fileId);
    if (pathIt == m_fileIdToPath.constEnd()) {
        return;
    }

    QString path = pathIt.value();
    auto metaIt = m_pathToMetadata.constFind(path);
    if (metaIt == m_pathToMetadata.constEnd()) {
        // Just clean up the orphaned fileIdToPath entry
        m_fileIdToPath.remove(fileId);
        return;
    }

    m_pathToMetadata.remove(path);
    m_fileIdToPath.remove(fileId);

    // Remove from parent's children list
    for (auto& children : m_parentToChildren) {
        children.removeAll(path);
    }

    // Remove children list if this was a directory
    m_parentToChildren.remove(path);
    m_childrenCacheTime.remove(path);

    // Unlock before database and signal operations
    locker.unlock();

    if (removeFromDatabaseFlag) {
        removeFromDatabase(fileId);
    }

    emit metadataRemoved(path);
}

bool MetadataCache::updatePath(const QString& oldPath, const QString& newPath) {
    QWriteLocker locker(&m_lock);

    auto it = m_pathToMetadata.find(oldPath);
    if (it == m_pathToMetadata.end()) {
        return false;
    }

    FuseFileMetadata metadata = it.value();
    metadata.path = newPath;
    metadata.name = newPath.contains('/') ? newPath.mid(newPath.lastIndexOf('/') + 1) : newPath;
    metadata.remoteName = metadata.name;
    metadata.cachedAt = QDateTime::currentDateTime();

    // Remove old entries
    m_pathToMetadata.remove(oldPath);
    for (auto& children : m_parentToChildren) {
        children.removeAll(oldPath);
    }

    // Add new entries
    m_pathToMetadata[newPath] = metadata;
    m_fileIdToPath[metadata.fileId] = newPath;

    QString newParentPath = getParentPath(newPath);
    m_parentToChildren[newParentPath].append(newPath);

    // If this was a directory, update children's parent cache
    if (metadata.isFolder) {
        auto childrenIt = m_parentToChildren.find(oldPath);
        if (childrenIt != m_parentToChildren.end()) {
            m_parentToChildren[newPath] = childrenIt.value();
            m_parentToChildren.remove(oldPath);
            m_childrenCacheTime[newPath] = m_childrenCacheTime.take(oldPath);
        }
    }

    // Make a copy of metadata before unlocking for thread safety
    FuseFileMetadata metadataCopy = metadata;

    // Update database (unlock first for database operation)
    locker.unlock();
    saveToDatabase(metadataCopy);

    emit metadataUpdated(newPath);
    return true;
}

bool MetadataCache::updateParentId(const QString& fileId, const QString& newParentId) {
    QWriteLocker locker(&m_lock);

    auto pathIt = m_fileIdToPath.constFind(fileId);
    if (pathIt == m_fileIdToPath.constEnd()) {
        return false;
    }

    QString path = pathIt.value();
    auto metaIt = m_pathToMetadata.find(path);
    if (metaIt == m_pathToMetadata.end()) {
        return false;
    }

    metaIt->parentId = newParentId;
    metaIt->cachedAt = QDateTime::currentDateTime();

    // Make a copy of metadata before unlocking for thread safety
    FuseFileMetadata metadata = metaIt.value();

    // Update database
    locker.unlock();
    saveToDatabase(metadata);

    emit metadataUpdated(path);
    return true;
}

void MetadataCache::markAccessed(const QString& path) {
    QWriteLocker locker(&m_lock);

    auto it = m_pathToMetadata.find(path);
    if (it != m_pathToMetadata.end()) {
        it->lastAccessed = QDateTime::currentDateTime();
    }
}

// ========================================
// Cache invalidation
// ========================================

void MetadataCache::invalidate(const QString& path) {
    QWriteLocker locker(&m_lock);

    auto it = m_pathToMetadata.find(path);
    if (it != m_pathToMetadata.end()) {
        // Set cachedAt to epoch to force refresh on next access
        it->cachedAt = QDateTime();
    }
}

void MetadataCache::invalidateByFileId(const QString& fileId) {
    QWriteLocker locker(&m_lock);

    auto it = m_fileIdToPath.constFind(fileId);
    if (it != m_fileIdToPath.constEnd()) {
        QString path = it.value();
        auto metaIt = m_pathToMetadata.find(path);
        if (metaIt != m_pathToMetadata.end()) {
            metaIt->cachedAt = QDateTime();
        }
    }
}

void MetadataCache::invalidateChildren(const QString& parentPath) {
    QWriteLocker locker(&m_lock);

    m_childrenCacheTime.remove(parentPath);

    auto childrenIt = m_parentToChildren.constFind(parentPath);
    if (childrenIt != m_parentToChildren.constEnd()) {
        for (const QString& childPath : childrenIt.value()) {
            auto metaIt = m_pathToMetadata.find(childPath);
            if (metaIt != m_pathToMetadata.end()) {
                metaIt->cachedAt = QDateTime();
            }
        }
    }
}

void MetadataCache::dropSubtreeFromCache(const QString& rootPath) {
    const QString normalizedRoot = normalizeMetadataPath(rootPath);
    if (normalizedRoot.isEmpty() || normalizedRoot == QStringLiteral("/")) {
        return;
    }

    QWriteLocker locker(&m_lock);

    QSet<QString> pathsToRemove;
    const QString subtreePrefix = normalizedRoot + QStringLiteral("/");
    for (auto it = m_pathToMetadata.constBegin(); it != m_pathToMetadata.constEnd(); ++it) {
        const QString candidatePath = it.key();
        if (candidatePath == normalizedRoot || candidatePath.startsWith(subtreePrefix)) {
            pathsToRemove.insert(candidatePath);
        }
    }

    if (pathsToRemove.isEmpty()) {
        return;
    }

    for (auto childrenIt = m_parentToChildren.begin(); childrenIt != m_parentToChildren.end();
         ++childrenIt) {
        QList<QString>& children = childrenIt.value();
        for (const QString& path : pathsToRemove) {
            children.removeAll(path);
        }
    }

    for (const QString& path : pathsToRemove) {
        auto metaIt = m_pathToMetadata.constFind(path);
        if (metaIt != m_pathToMetadata.constEnd()) {
            m_fileIdToPath.remove(metaIt->fileId);
        }
        m_pathToMetadata.remove(path);
        m_parentToChildren.remove(path);
        m_childrenCacheTime.remove(path);
    }
}

void MetadataCache::clearCache() {
    QWriteLocker locker(&m_lock);

    m_pathToMetadata.clear();
    m_fileIdToPath.clear();
    m_parentToChildren.clear();
    m_childrenCacheTime.clear();

    qInfo() << "MetadataCache: In-memory cache cleared";
    emit cacheCleared();
}

void MetadataCache::clearAll() {
    clearCache();

    // Clear database table via SyncDatabase
    if (m_database) {
        // Get all entries and delete them
        QList<FuseMetadata> allEntries = m_database->getAllFuseMetadata();
        for (const FuseMetadata& entry : allEntries) {
            m_database->deleteFuseMetadata(entry.fileId);
        }
    }

    qInfo() << "MetadataCache: All metadata cleared including database";
}

// ========================================
// Configuration
// ========================================

void MetadataCache::setMaxCacheAge(int seconds) {
    m_maxCacheAgeSeconds = seconds;
}

int MetadataCache::maxCacheAge() const {
    return m_maxCacheAgeSeconds;
}

QString MetadataCache::rootFolderId() const {
    QReadLocker locker(&m_lock);
    return m_rootFolderId;
}

void MetadataCache::setRootFolderId(const QString& fileId) {
    QWriteLocker locker(&m_lock);
    m_rootFolderId = fileId;

    // Add root entry to cache with path "/"
    FuseFileMetadata rootMeta;
    rootMeta.fileId = fileId;
    rootMeta.path = "/";
    rootMeta.name = "";
    rootMeta.parentId = "";
    rootMeta.isFolder = true;
    rootMeta.cachedAt = QDateTime::currentDateTime();
    rootMeta.lastAccessed = QDateTime::currentDateTime();

    m_pathToMetadata["/"] = rootMeta;
    m_fileIdToPath[fileId] = "/";
}

// ========================================
// Statistics
// ========================================

int MetadataCache::cacheSize() const {
    QReadLocker locker(&m_lock);
    return m_pathToMetadata.size();
}

void MetadataCache::getStatistics(qint64* hits, qint64* misses) const {
    if (hits) {
        *hits = m_cacheHits;
    }
    if (misses) {
        *misses = m_cacheMisses;
    }
}

void MetadataCache::resetStatistics() {
    m_cacheHits = 0;
    m_cacheMisses = 0;
}

// ========================================
// Private slots
// ========================================

void MetadataCache::onApiMetadataReceived(const QString& fileId, const FuseFileMetadata& metadata) {
    Q_UNUSED(fileId);

    FuseFileMetadata resolved = upsertRemoteMetadataInternal(metadata);
    if (resolved.isValid()) {
        emit metadataFetched(resolved.path, true);
    } else {
        qWarning() << "MetadataCache: Received invalid metadata for fileId:" << fileId;
        emit metadataFetched(QString(), false);
    }
}

void MetadataCache::onApiChildrenReceived(const QString& parentId,
                                          const QList<FuseFileMetadata>& children) {
    replaceRemoteChildrenInternal(parentId, children);
}

// ========================================
// Private methods
// ========================================

void MetadataCache::loadFromDatabase() {
    // Avoid eagerly mirroring fuse_metadata into RAM. Cold paths are hydrated from SQLite on demand
    // by getOrFetchMetadataByPath() / getOrFetchChildren().
    {
        QWriteLocker locker(&m_lock);
        m_pathToMetadata.clear();
        m_fileIdToPath.clear();
        m_parentToChildren.clear();
        m_childrenCacheTime.clear();
    }

    if (!m_database) {
        qWarning() << "MetadataCache: No database available, starting with empty cache";
        return;
    }

    const QList<FuseMetadata> dbEntries = m_database->getAllFuseMetadata();
    QList<FuseMetadata> staleTrashEntries;
    staleTrashEntries.reserve(dbEntries.size());

    for (const FuseMetadata& dbMeta : dbEntries) {
        if (TrashPolicy::isTrashRelativePath(dbMeta.path)) {
            staleTrashEntries.append(dbMeta);
        }
    }

    for (const FuseMetadata& staleEntry : staleTrashEntries) {
        m_database->deleteNativeDocState(staleEntry.fileId);
        m_database->deleteFuseMetadata(staleEntry.fileId);
    }

    qDebug() << "MetadataCache: Deferred startup preload; pruned"
             << staleTrashEntries.size() << "stale trash entries";
}

void MetadataCache::saveToDatabase(const FuseFileMetadata& metadata) {
    if (!m_database) {
        return;
    }

    if (!m_database->saveFuseMetadata(toDbMetadata(metadata))) {
        qWarning() << "MetadataCache: Failed to save metadata to database";
        emit cacheError("Failed to save metadata");
    }
}

QString MetadataCache::resolveParentPath(const QString& parentId) const {
    if (parentId.isEmpty()) {
        return QString();
    }

    {
        QReadLocker locker(&m_lock);
        if (parentId == QStringLiteral("root") ||
            (!m_rootFolderId.isEmpty() && parentId == m_rootFolderId)) {
            return QStringLiteral("/");
        }

        auto it = m_fileIdToPath.constFind(parentId);
        if (it != m_fileIdToPath.constEnd()) {
            return normalizeMetadataPath(it.value());
        }
    }

    if (!m_database) {
        return QString();
    }

    const FuseMetadata parentMeta = m_database->getFuseMetadata(parentId);
    if (!parentMeta.fileId.isEmpty()) {
        return normalizeMetadataPath(parentMeta.path);
    }

    return QString();
}

FuseFileMetadata MetadataCache::upsertRemoteMetadataInternal(const FuseFileMetadata& metadata) {
    const QString parentPath = resolveParentPath(metadata.parentId);
    if (parentPath.isEmpty()) {
        return FuseFileMetadata();
    }

    QHash<QString, FuseFileMetadata> existingByFileId;
    QSet<QString> claimedPaths;

    if (m_database && !metadata.parentId.isEmpty()) {
        const QList<FuseMetadata> siblings = m_database->getFuseChildren(metadata.parentId);
        for (const FuseMetadata& sibling : siblings) {
            const FuseFileMetadata existing = fromDbMetadata(sibling);
            existingByFileId.insert(existing.fileId, existing);
            if (existing.fileId != metadata.fileId) {
                claimedPaths.insert(existing.path);
            }
        }

        const FuseMetadata selfMeta = m_database->getFuseMetadata(metadata.fileId);
        if (!selfMeta.fileId.isEmpty()) {
            existingByFileId.insert(selfMeta.fileId, fromDbMetadata(selfMeta));
        }
    }

    FuseFileMetadata resolved = resolveRemoteMetadata(metadata, parentPath, existingByFileId,
                                                      &claimedPaths, m_duplicateNameStrategy);
    if (!resolved.isValid()) {
        return FuseFileMetadata();
    }

    if (TrashPolicy::isTrashRelativePath(resolved.path)) {
        return FuseFileMetadata();
    }

    setMetadata(resolved);
    return resolved;
}

QList<FuseFileMetadata> MetadataCache::replaceRemoteChildrenInternal(
    const QString& parentId, const QList<FuseFileMetadata>& children) {
    const QString parentPath = resolveParentPath(parentId);
    if (parentPath.isEmpty()) {
        qWarning() << "MetadataCache: Cannot find parent path for parentId:" << parentId;
        return {};
    }

    QList<FuseMetadata> existingChildren;
    if (m_database && !parentId.isEmpty()) {
        existingChildren = m_database->getFuseChildren(parentId);
    }

    QHash<QString, FuseFileMetadata> existingByFileId;
    QSet<QString> incomingIds;
    for (const FuseMetadata& existingChild : existingChildren) {
        const FuseFileMetadata existing = fromDbMetadata(existingChild);
        existingByFileId.insert(existing.fileId, existing);
    }

    QList<FuseFileMetadata> resolvedChildren;
    resolvedChildren.reserve(children.size());
    QSet<QString> claimedPaths;
    for (const FuseFileMetadata& child : children) {
        if (child.fileId.isEmpty()) {
            continue;
        }

        incomingIds.insert(child.fileId);
        FuseFileMetadata resolved = resolveRemoteMetadata(child, parentPath, existingByFileId,
                                                          &claimedPaths, m_duplicateNameStrategy);
        if (!resolved.isValid()) {
            continue;
        }

        if (TrashPolicy::isTrashRelativePath(resolved.path)) {
            continue;
        }

        resolvedChildren.append(resolved);
    }

    if (m_database) {
        QList<FuseMetadata> allEntries;
        QSet<QString> staleRoots;
        for (const FuseMetadata& existingChild : existingChildren) {
            if (!incomingIds.contains(existingChild.fileId)) {
                staleRoots.insert(normalizeMetadataPath(existingChild.path));
            }
        }

        if (!staleRoots.isEmpty()) {
            allEntries = m_database->getAllFuseMetadata();
        }

        QSet<QString> staleFileIds;
        for (const FuseMetadata& entry : allEntries) {
            const QString entryPath = normalizeMetadataPath(entry.path);
            for (const QString& staleRoot : staleRoots) {
                if (pathIsWithinSubtree(entryPath, staleRoot)) {
                    staleFileIds.insert(entry.fileId);
                    break;
                }
            }
        }

        for (const QString& staleRoot : staleRoots) {
            const auto staleSelf =
                std::find_if(existingChildren.cbegin(), existingChildren.cend(),
                             [&staleRoot](const FuseMetadata& existingChild) {
                                 return normalizeMetadataPath(existingChild.path) == staleRoot;
                             });
            if (staleSelf != existingChildren.cend()) {
                staleFileIds.insert(staleSelf->fileId);
            }
        }

        for (const QString& staleFileId : staleFileIds) {
            removeByFileId(staleFileId);
        }
    }

    setMetadataBatch(resolvedChildren);

    {
        QWriteLocker locker(&m_lock);
        QList<QString> childPaths;
        childPaths.reserve(resolvedChildren.size());
        for (const FuseFileMetadata& child : resolvedChildren) {
            childPaths.append(child.path);
        }
        m_parentToChildren[parentPath] = childPaths;
        m_childrenCacheTime[parentPath] = QDateTime::currentDateTime();
    }

    return resolvedChildren;
}

void MetadataCache::removeFromDatabase(const QString& fileId) {
    if (!m_database) {
        return;
    }

    if (!m_database->deleteFuseMetadata(fileId)) {
        qWarning() << "MetadataCache: Failed to remove from database";
        emit cacheError("Failed to remove metadata");
    }
}

QString MetadataCache::buildPathFromParents(const QString& fileId) const {
    QReadLocker locker(&m_lock);

    // If already in cache, return known path
    auto it = m_fileIdToPath.constFind(fileId);
    if (it != m_fileIdToPath.constEnd()) {
        return it.value();
    }

    // Try to build from parent chain (would need API calls for unknown parents)
    // For now, return empty - caller should use getOrFetchMetadataByPath after
    // setting up the path mapping elsewhere
    return QString();
}

QString MetadataCache::getParentPath(const QString& path) {
    const QString normalized = normalizeMetadataPath(path);
    if (normalized.isEmpty() || normalized == "/") {
        return "/";
    }

    const int lastSlash = normalized.lastIndexOf('/');
    if (lastSlash <= 0) {
        // Path is at root level (e.g., "/file.txt" or "file.txt")
        return "/";
    }

    return normalized.left(lastSlash);
}

void MetadataCache::requestMetadataFromApi(const QString& fileId) {
    if (m_driveClient) {
        QMetaObject::invokeMethod(
            m_driveClient,
            [driveClient = m_driveClient, fileId]() { driveClient->getFile(fileId); },
            Qt::QueuedConnection);
    }
}

void MetadataCache::requestChildrenFromApi(const QString& parentId) {
    if (m_driveClient) {
        QMetaObject::invokeMethod(
            m_driveClient,
            [driveClient = m_driveClient, parentId]() { driveClient->listFiles(parentId); },
            Qt::QueuedConnection);
    }
}
