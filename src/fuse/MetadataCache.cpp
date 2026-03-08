/**
 * @file MetadataCache.cpp
 * @brief Implementation of in-memory metadata cache for FUSE filesystem
 */

#include "MetadataCache.h"

#include <QDebug>
#include <QReadLocker>
#include <QWriteLocker>

#include "api/GoogleDriveClient.h"
#include "sync/SyncDatabase.h"

const int MetadataCache::DEFAULT_MAX_CACHE_AGE_SECONDS;

MetadataCache::MetadataCache(SyncDatabase* database, GoogleDriveClient* driveClient,
                             QObject* parent)
    : QObject(parent),
      m_database(database),
      m_driveClient(driveClient),
      m_maxCacheAgeSeconds(DEFAULT_MAX_CACHE_AGE_SECONDS),
      m_cacheHits(0),
      m_cacheMisses(0) {
    // Connect to Google Drive client signals for async metadata fetch
    if (m_driveClient) {
        connect(m_driveClient, &GoogleDriveClient::fileReceived, this,
                [this](const DriveFile& file) {
                    // Convert DriveFile to FuseFileMetadata
                    FuseFileMetadata metadata;
                    metadata.fileId = file.id;
                    metadata.name = file.name;
                    metadata.parentId = file.parentId();
                    metadata.isFolder = file.isFolder;
                    metadata.size = file.size;
                    metadata.mimeType = file.mimeType;
                    metadata.createdTime = file.createdTime;
                    metadata.modifiedTime = file.modifiedTime;
                    metadata.cachedAt = QDateTime::currentDateTime();
                    metadata.lastAccessed = QDateTime::currentDateTime();

                    // Try to resolve path from parent chain
                    metadata.path = buildPathFromParents(file.id);

                    onApiMetadataReceived(file.id, metadata);
                });

        connect(m_driveClient, &GoogleDriveClient::filesListed, this,
                [this](const QList<DriveFile>& files, const QString& nextPageToken) {
                    Q_UNUSED(nextPageToken);

                    // Group files by their parent ID
                    QHash<QString, QList<FuseFileMetadata>> childrenByParent;

                    for (const DriveFile& file : files) {
                        FuseFileMetadata metadata;
                        metadata.fileId = file.id;
                        metadata.name = file.name;
                        metadata.parentId = file.parentId();
                        metadata.isFolder = file.isFolder;
                        metadata.size = file.size;
                        metadata.mimeType = file.mimeType;
                        metadata.createdTime = file.createdTime;
                        metadata.modifiedTime = file.modifiedTime;
                        metadata.cachedAt = QDateTime::currentDateTime();
                        metadata.lastAccessed = QDateTime::currentDateTime();

                        QString parentId = file.parentId();
                        if (!parentId.isEmpty()) {
                            childrenByParent[parentId].append(metadata);
                        }
                    }

                    // Process each parent's children
                    for (auto it = childrenByParent.constBegin(); it != childrenByParent.constEnd();
                         ++it) {
                        onApiChildrenReceived(it.key(), it.value());
                    }
                });
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

    // Load existing metadata from database
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
            FuseFileMetadata metadata;
            metadata.fileId = dbMeta.fileId;
            metadata.path = dbMeta.path;
            metadata.name = dbMeta.name;
            metadata.parentId = dbMeta.parentId;
            metadata.isFolder = dbMeta.isFolder;
            metadata.size = dbMeta.size;
            metadata.mimeType = dbMeta.mimeType;
            metadata.createdTime = dbMeta.createdTime;
            metadata.modifiedTime = dbMeta.modifiedTime;
            metadata.cachedAt = dbMeta.cachedAt;
            metadata.lastAccessed = dbMeta.lastAccessed;

            // Update in-memory cache
            {
                QWriteLocker locker(&m_lock);
                m_pathToMetadata[path] = metadata;
                m_fileIdToPath[metadata.fileId] = path;
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
    QReadLocker locker(&m_lock);

    auto pathIt = m_fileIdToPath.constFind(fileId);
    if (pathIt != m_fileIdToPath.constEnd()) {
        auto metaIt = m_pathToMetadata.constFind(pathIt.value());
        if (metaIt != m_pathToMetadata.constEnd()) {
            m_cacheHits++;
            return metaIt.value();
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
    QReadLocker locker(&m_lock);

    auto it = m_fileIdToPath.constFind(fileId);
    if (it != m_fileIdToPath.constEnd()) {
        return it.value();
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

    // Check if children are cached and fresh
    {
        QReadLocker locker(&m_lock);
        auto timeIt = m_childrenCacheTime.constFind(parentPath);
        if (timeIt != m_childrenCacheTime.constEnd()) {
            if (timeIt->secsTo(QDateTime::currentDateTime()) < m_maxCacheAgeSeconds) {
                return getChildren(parentPath);
            }
        }
    }

    // Try database via SyncDatabase (thread-safe)
    // Get parent file ID first
    QString parentId;
    if (parentPath.isEmpty() || parentPath == "/") {
        parentId = m_rootFolderId;
    } else {
        QReadLocker locker(&m_lock);
        auto it = m_pathToMetadata.constFind(parentPath);
        if (it != m_pathToMetadata.constEnd()) {
            parentId = it->fileId;
        }
    }

    if (!parentId.isEmpty() && m_database) {
        QList<FuseMetadata> dbChildren = m_database->getFuseChildren(parentId);

        if (!dbChildren.isEmpty()) {
            QList<FuseFileMetadata> children;
            for (const FuseMetadata& dbMeta : dbChildren) {
                FuseFileMetadata metadata;
                metadata.fileId = dbMeta.fileId;
                metadata.path = dbMeta.path;
                metadata.name = dbMeta.name;
                metadata.parentId = dbMeta.parentId;
                metadata.isFolder = dbMeta.isFolder;
                metadata.size = dbMeta.size;
                metadata.mimeType = dbMeta.mimeType;
                metadata.createdTime = dbMeta.createdTime;
                metadata.modifiedTime = dbMeta.modifiedTime;
                metadata.cachedAt = dbMeta.cachedAt;
                metadata.lastAccessed = dbMeta.lastAccessed;
                children.append(metadata);
            }

            // Update in-memory cache
            QWriteLocker locker(&m_lock);
            QList<QString> childPaths;
            for (const FuseFileMetadata& child : children) {
                m_pathToMetadata[child.path] = child;
                m_fileIdToPath[child.fileId] = child.path;
                childPaths.append(child.path);
            }
            m_parentToChildren[parentPath] = childPaths;
            m_childrenCacheTime[parentPath] = QDateTime::currentDateTime();

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

// ========================================
// Cache modification
// ========================================

void MetadataCache::setMetadata(const FuseFileMetadata& metadata) {
    if (!metadata.isValid()) {
        qWarning() << "MetadataCache: Cannot store invalid metadata";
        return;
    }

    // Update in-memory cache
    {
        QWriteLocker locker(&m_lock);

        // Remove old path mapping if file ID exists with different path
        auto oldPathIt = m_fileIdToPath.constFind(metadata.fileId);
        if (oldPathIt != m_fileIdToPath.constEnd() && oldPathIt.value() != metadata.path) {
            m_pathToMetadata.remove(oldPathIt.value());

            // Update parent's children list
            for (auto& children : m_parentToChildren) {
                children.removeAll(oldPathIt.value());
            }
        }

        m_pathToMetadata[metadata.path] = metadata;
        m_fileIdToPath[metadata.fileId] = metadata.path;

        // Update parent's children list
        QString parentPath = getParentPath(metadata.path);

        if (!m_parentToChildren[parentPath].contains(metadata.path)) {
            m_parentToChildren[parentPath].append(metadata.path);
        }
    }

    // Persist to database
    saveToDatabase(metadata);

    emit metadataUpdated(metadata.path);
}

void MetadataCache::setMetadataBatch(const QList<FuseFileMetadata>& metadataList) {
    if (metadataList.isEmpty()) {
        return;
    }

    QWriteLocker locker(&m_lock);

    for (const FuseFileMetadata& metadata : metadataList) {
        if (!metadata.isValid()) {
            continue;
        }

        // Update in-memory cache
        auto oldPathIt = m_fileIdToPath.constFind(metadata.fileId);
        if (oldPathIt != m_fileIdToPath.constEnd() && oldPathIt.value() != metadata.path) {
            m_pathToMetadata.remove(oldPathIt.value());

            // Also remove stale path from parent's children list
            for (auto& children : m_parentToChildren) {
                children.removeAll(oldPathIt.value());
            }
        }

        m_pathToMetadata[metadata.path] = metadata;
        m_fileIdToPath[metadata.fileId] = metadata.path;

        // Update parent's children list
        QString parentPath = getParentPath(metadata.path);

        if (!m_parentToChildren[parentPath].contains(metadata.path)) {
            m_parentToChildren[parentPath].append(metadata.path);
        }

        // Save to database via SyncDatabase (thread-safe)
        if (m_database) {
            FuseMetadata dbMeta;
            dbMeta.fileId = metadata.fileId;
            dbMeta.path = metadata.path;
            dbMeta.name = metadata.name;
            dbMeta.parentId = metadata.parentId;
            dbMeta.isFolder = metadata.isFolder;
            dbMeta.size = metadata.size;
            dbMeta.mimeType = metadata.mimeType;
            dbMeta.createdTime = metadata.createdTime;
            dbMeta.modifiedTime = metadata.modifiedTime;
            dbMeta.cachedAt = metadata.cachedAt;
            dbMeta.lastAccessed = metadata.lastAccessed;
            m_database->saveFuseMetadata(dbMeta);
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

void MetadataCache::removeByFileId(const QString& fileId) {
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

    // Remove from database
    removeFromDatabase(fileId);

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

void MetadataCache::setMaxCacheAge(int seconds) { m_maxCacheAgeSeconds = seconds; }

int MetadataCache::maxCacheAge() const { return m_maxCacheAgeSeconds; }

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
    if (metadata.isValid()) {
        setMetadata(metadata);
        emit metadataFetched(metadata.path, true);
    } else {
        qWarning() << "MetadataCache: Received invalid metadata for fileId:" << fileId;
        emit metadataFetched(QString(), false);
    }
}

void MetadataCache::onApiChildrenReceived(const QString& parentId,
                                          const QList<FuseFileMetadata>& children) {
    // Find parent path from parentId
    QString parentPath;
    {
        QReadLocker locker(&m_lock);
        if (parentId == m_rootFolderId) {
            parentPath = "/";
        } else {
            auto it = m_fileIdToPath.constFind(parentId);
            if (it != m_fileIdToPath.constEnd()) {
                parentPath = it.value();
            }
        }
    }

    if (parentPath.isEmpty()) {
        qWarning() << "MetadataCache: Cannot find parent path for parentId:" << parentId;
        return;
    }

    // Build full paths for children and store
    QList<FuseFileMetadata> childrenWithPaths;
    for (FuseFileMetadata child : children) {
        if (parentPath == "/") {
            // Use bare name (no leading slash) to match FuseDriver's
            // path convention and avoid duplicate cache entries
            child.path = child.name;
        } else {
            child.path = parentPath + "/" + child.name;
        }
        childrenWithPaths.append(child);
    }

    setMetadataBatch(childrenWithPaths);

    // Update children cache time
    {
        QWriteLocker locker(&m_lock);
        QList<QString> childPaths;
        for (const FuseFileMetadata& child : childrenWithPaths) {
            childPaths.append(child.path);
        }
        m_parentToChildren[parentPath] = childPaths;
        m_childrenCacheTime[parentPath] = QDateTime::currentDateTime();
    }
}

// ========================================
// Private methods
// ========================================

void MetadataCache::loadFromDatabase() {
    if (!m_database) {
        qWarning() << "MetadataCache: No database available, starting with empty cache";
        return;
    }

    QList<FuseMetadata> dbEntries = m_database->getAllFuseMetadata();

    QWriteLocker locker(&m_lock);

    for (const FuseMetadata& dbMeta : dbEntries) {
        FuseFileMetadata metadata;
        metadata.fileId = dbMeta.fileId;
        metadata.path = dbMeta.path;
        metadata.name = dbMeta.name;
        metadata.parentId = dbMeta.parentId;
        metadata.isFolder = dbMeta.isFolder;
        metadata.size = dbMeta.size;
        metadata.mimeType = dbMeta.mimeType;
        metadata.createdTime = dbMeta.createdTime;
        metadata.modifiedTime = dbMeta.modifiedTime;
        metadata.cachedAt = dbMeta.cachedAt;
        metadata.lastAccessed = dbMeta.lastAccessed;

        // Normalize legacy paths that may have a leading slash
        if (metadata.path.startsWith(QStringLiteral("/"))) {
            metadata.path = metadata.path.mid(1);
        }

        m_pathToMetadata[metadata.path] = metadata;
        m_fileIdToPath[metadata.fileId] = metadata.path;

        // Build parent-to-children mapping (deduplicate)
        QString parentPath = getParentPath(metadata.path);
        if (!m_parentToChildren[parentPath].contains(metadata.path)) {
            m_parentToChildren[parentPath].append(metadata.path);
        }
    }

    qDebug() << "MetadataCache: Loaded" << m_pathToMetadata.size() << "entries from database";
}

void MetadataCache::saveToDatabase(const FuseFileMetadata& metadata) {
    if (!m_database) {
        return;
    }

    FuseMetadata dbMeta;
    dbMeta.fileId = metadata.fileId;
    dbMeta.path = metadata.path;
    dbMeta.name = metadata.name;
    dbMeta.parentId = metadata.parentId;
    dbMeta.isFolder = metadata.isFolder;
    dbMeta.size = metadata.size;
    dbMeta.mimeType = metadata.mimeType;
    dbMeta.createdTime = metadata.createdTime;
    dbMeta.modifiedTime = metadata.modifiedTime;
    dbMeta.cachedAt = metadata.cachedAt;
    dbMeta.lastAccessed = metadata.lastAccessed;

    if (!m_database->saveFuseMetadata(dbMeta)) {
        qWarning() << "MetadataCache: Failed to save metadata to database";
        emit cacheError("Failed to save metadata");
    }
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
    if (path.isEmpty() || path == "/") {
        return "/";
    }

    int lastSlash = path.lastIndexOf('/');
    if (lastSlash <= 0) {
        // Path is at root level (e.g., "/file.txt" or "file.txt")
        return "/";
    }

    return path.left(lastSlash);
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
