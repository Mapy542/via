/**
 * @file SyncDatabase.cpp
 * @brief Implementation of SQLite sync database
 */

#include "SyncDatabase.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QThread>
#include <atomic>
#include <stdexcept>

const QString SyncDatabase::DB_NAME = "via_sync.db";
const QString SyncDatabase::SCHEMA_EPOCH_KEY = "sync_schema_epoch";
const int SyncDatabase::DB_VERSION = 6;
const int SyncDatabase::CURRENT_SCHEMA_EPOCH = 1;

namespace {

static std::atomic<int> s_connectionCounter{0};

bool tableExists(const QSqlDatabase& db, const QString& tableName) {
    return db.tables().contains(tableName, Qt::CaseInsensitive);
}

bool tableHasColumn(QSqlDatabase db, const QString& tableName, const QString& columnName) {
    QSqlQuery query(db);
    if (!query.exec(QString("PRAGMA table_info(%1)").arg(tableName))) {
        return false;
    }
    while (query.next()) {
        if (query.value("name").toString() == columnName) {
            return true;
        }
    }
    return false;
}

bool addColumnIfMissing(QSqlDatabase db, const QString& tableName, const QString& columnDef) {
    const QString columnName = columnDef.section(' ', 0, 0);
    if (tableHasColumn(db, tableName, columnName)) {
        return true;
    }

    QSqlQuery query(db);
    return query.exec(QString("ALTER TABLE %1 ADD COLUMN %2").arg(tableName, columnDef));
}

FuseMetadata readFuseMetadataRow(const QSqlQuery& query) {
    FuseMetadata metadata;
    metadata.fileId = query.value("file_id").toString();
    metadata.path = query.value("path").toString();
    metadata.name = query.value("name").toString();

    const int remoteNameIndex = query.record().indexOf("remote_name");
    if (remoteNameIndex >= 0) {
        metadata.remoteName = query.value(remoteNameIndex).toString();
    }
    if (metadata.remoteName.isEmpty()) {
        metadata.remoteName = metadata.name;
    }

    const int nativeDocModeOverrideIndex = query.record().indexOf("native_doc_mode_override");
    if (nativeDocModeOverrideIndex >= 0) {
        metadata.nativeDocModeOverride = query.value(nativeDocModeOverrideIndex).toString();
    }

    metadata.parentId = query.value("parent_id").toString();
    metadata.isFolder = query.value("is_folder").toInt() == 1;
    metadata.size = query.value("size").toLongLong();
    metadata.mimeType = query.value("mime_type").toString();

    const int remoteMimeIndex = query.record().indexOf("remote_mime_type");
    if (remoteMimeIndex >= 0) {
        metadata.remoteMimeType = query.value(remoteMimeIndex).toString();
    }

    const int webViewIndex = query.record().indexOf("web_view_link");
    if (webViewIndex >= 0) {
        metadata.webViewLink = query.value(webViewIndex).toString();
    }

    metadata.createdTime =
        QDateTime::fromString(query.value("created_time").toString(), Qt::ISODate);
    metadata.modifiedTime =
        QDateTime::fromString(query.value("modified_time").toString(), Qt::ISODate);
    metadata.cachedAt = QDateTime::fromString(query.value("cached_at").toString(), Qt::ISODate);
    metadata.lastAccessed =
        QDateTime::fromString(query.value("last_accessed").toString(), Qt::ISODate);
    return metadata;
}

NativeDocState readNativeDocStateRow(const QSqlQuery& query) {
    NativeDocState state;
    state.fileId = query.value("file_id").toString();
    state.remoteName = query.value("remote_name").toString();
    state.remoteMimeType = query.value("remote_mime_type").toString();
    state.webViewLink = query.value("web_view_link").toString();
    state.nativeDocModeOverride = query.value("native_doc_mode_override").toString();
    return state;
}

quintptr currentThreadKey() {
    return reinterpret_cast<quintptr>(QThread::currentThreadId());
}
}  // namespace

SyncDatabaseConnectionHandle::SyncDatabaseConnectionHandle(const SyncDatabase* owner)
    : m_owner(owner) {}

QSqlDatabase SyncDatabaseConnectionHandle::database() const {
    return m_owner ? m_owner->databaseForCurrentThread() : QSqlDatabase();
}

SyncDatabaseConnectionHandle::operator QSqlDatabase() const {
    return database();
}

bool SyncDatabaseConnectionHandle::isOpen() const {
    return database().isOpen();
}

QString SyncDatabaseConnectionHandle::connectionName() const {
    return database().connectionName();
}

QStringList SyncDatabaseConnectionHandle::tables() const {
    return database().tables();
}

QSqlError SyncDatabaseConnectionHandle::lastError() const {
    return database().lastError();
}

SyncDatabase::SyncDatabase(QObject* parent)
    : QObject(parent), m_db(this), m_concurrentAccessCount(0) {
    // Set database path
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_dbPath = dataPath + "/" + DB_NAME;
}

SyncDatabase::~SyncDatabase() {
    close();
}

bool SyncDatabase::openConnectionUnlocked() {
    const quintptr threadKey = currentThreadKey();
    const QString connectionName = ensureConnectionNameForThreadUnlocked(threadKey);

    QSqlDatabase db;
    if (QSqlDatabase::contains(connectionName)) {
        db = QSqlDatabase::database(connectionName, false);
    } else if (!m_primaryConnectionName.isEmpty() && connectionName != m_primaryConnectionName &&
               QSqlDatabase::contains(m_primaryConnectionName)) {
        db = QSqlDatabase::cloneDatabase(m_primaryConnectionName, connectionName);
    } else {
        db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    }

    if (db.isOpen()) {
        return true;
    }

    db.setDatabaseName(m_dbPath);

    if (!db.open()) {
        logError("initialize", db.lastError().text());
        return false;
    }

    QSqlQuery walQuery(db);
    if (!walQuery.exec("PRAGMA journal_mode=WAL")) {
        qWarning() << "Failed to enable WAL mode:" << walQuery.lastError().text();
    }

    if (m_primaryConnectionName.isEmpty()) {
        m_primaryConnectionName = connectionName;
    }

    return true;
}

bool SyncDatabase::initialize() {
    QMutexLocker locker(&m_mutex);
    m_lastSchemaCompatibility = SchemaCompatibility::Current;
    m_connectionsReady = false;

    // Ensure data directory exists
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    closeUnlocked();
    if (!openConnectionUnlocked()) {
        return false;
    }

    if (!ensureSettingsTable()) {
        return false;
    }

    m_lastSchemaCompatibility = detectSchemaCompatibility();
    if (m_lastSchemaCompatibility == SchemaCompatibility::UnsupportedFutureSchema) {
        logError("initialize",
                 QString("Database schema is newer than supported (epoch=%1, legacy=%2)")
                     .arg(getStoredSchemaEpoch())
                     .arg(getStoredVersion()));
        return false;
    }

    if (m_lastSchemaCompatibility == SchemaCompatibility::ResetRequired) {
        logError("initialize",
                 QStringLiteral("Database schema is incompatible and must be recreated"));
        return false;
    }

    if (m_lastSchemaCompatibility == SchemaCompatibility::ResetBlockedByDirtyState) {
        logError("initialize", QStringLiteral("Database schema is incompatible and pending "
                                              "dirty uploads must be discarded explicitly"));
        return false;
    }

    // Ensure all tables/indexes exist for the current schema.
    if (!createTables()) {
        return false;
    }

    // Create FUSE-specific tables (isolated from Mirror Sync)
    if (!createFuseTables()) {
        return false;
    }

    if (!setStoredSchemaEpoch(CURRENT_SCHEMA_EPOCH)) {
        return false;
    }

    if (!setStoredVersion(DB_VERSION)) {
        return false;
    }

    m_lastSchemaCompatibility = SchemaCompatibility::Current;
    m_connectionsReady = true;

    qInfo() << "Sync database initialized at:" << m_dbPath;
    return true;
}

bool SyncDatabase::recreateCurrentSchema() {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen() && !openConnectionUnlocked()) {
        return false;
    }

    return recreateDatabaseUnlocked();
}

void SyncDatabase::close() {
    QMutexLocker locker(&m_mutex);
    closeUnlocked();
}

void SyncDatabase::closeCurrentThreadConnection() {
    QMutexLocker locker(&m_mutex);

    const quintptr threadKey = currentThreadKey();
    const QString connectionName = connectionNameForThreadUnlocked(threadKey);
    if (connectionName.isEmpty()) {
        return;
    }

    m_connectionNamesByThread.remove(threadKey);
    if (m_primaryConnectionName == connectionName) {
        m_primaryConnectionName.clear();
    }

    closeConnectionByNameUnlocked(connectionName);
}

void SyncDatabase::closeUnlocked() {
    m_connectionsReady = false;
    closeAllConnectionsUnlocked();
}

bool SyncDatabase::isOpen() const {
    QMutexLocker locker(&m_mutex);
    return databaseForCurrentThreadUnlocked().isOpen();
}

QSqlDatabase SyncDatabase::databaseForCurrentThread() const {
    QMutexLocker locker(&m_mutex);
    QSqlDatabase db = databaseForCurrentThreadUnlocked();
    if ((!db.isValid() || !db.isOpen()) && m_connectionsReady) {
        if (const_cast<SyncDatabase*>(this)->openConnectionUnlocked()) {
            db = databaseForCurrentThreadUnlocked();
        }
    }

    return db;
}

QSqlDatabase SyncDatabase::databaseForCurrentThreadUnlocked() const {
    const QString connectionName = connectionNameForThreadUnlocked(currentThreadKey());
    if (connectionName.isEmpty() || !QSqlDatabase::contains(connectionName)) {
        return QSqlDatabase();
    }

    return QSqlDatabase::database(connectionName, false);
}

void SyncDatabase::closeConnectionByNameUnlocked(const QString& connectionName) {
    if (connectionName.isEmpty() || !QSqlDatabase::contains(connectionName)) {
        return;
    }

    if (connectionName == connectionNameForThreadUnlocked(currentThreadKey())) {
        QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        if (db.isValid() && db.isOpen()) {
            db.close();
        }
    }

    QSqlDatabase::removeDatabase(connectionName);
}

void SyncDatabase::closeAllConnectionsUnlocked() {
    const QString currentThreadConnectionName = connectionNameForThreadUnlocked(currentThreadKey());
    const QList<QString> connectionNames = m_connectionNamesByThread.values();
    m_connectionNamesByThread.clear();
    m_primaryConnectionName.clear();

    for (const QString& connectionName : connectionNames) {
        if (connectionName != currentThreadConnectionName &&
            QSqlDatabase::contains(connectionName)) {
            QSqlDatabase::removeDatabase(connectionName);
            continue;
        }

        closeConnectionByNameUnlocked(connectionName);
    }
}

QString SyncDatabase::connectionNameForThreadUnlocked(quintptr threadKey) const {
    return m_connectionNamesByThread.value(threadKey);
}

QString SyncDatabase::ensureConnectionNameForThreadUnlocked(quintptr threadKey) {
    const auto existing = m_connectionNamesByThread.constFind(threadKey);
    if (existing != m_connectionNamesByThread.cend()) {
        return existing.value();
    }

    const QString connectionName = QStringLiteral("sync_connection_%1_%2")
                                       .arg(threadKey)
                                       .arg(s_connectionCounter.fetch_add(1));
    m_connectionNamesByThread.insert(threadKey, connectionName);
    return connectionName;
}

SyncDatabase::SchemaCompatibility SyncDatabase::lastSchemaCompatibility() const {
    QMutexLocker locker(&m_mutex);
    return m_lastSchemaCompatibility;
}

bool SyncDatabase::ensureSettingsTable() {
    QSqlQuery query(m_db);
    QString createSettingsTable = R"(
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT
        )
    )";

    if (!query.exec(createSettingsTable)) {
        logError("ensureSettingsTable", query.lastError().text());
        return false;
    }

    return true;
}

int SyncDatabase::getStoredSchemaEpoch() const {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(SCHEMA_EPOCH_KEY);
    if (query.exec() && query.next()) {
        bool ok = false;
        const int epoch = query.value(0).toInt(&ok);
        return ok ? epoch : 0;
    }

    return 0;
}

bool SyncDatabase::setStoredSchemaEpoch(int epoch) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)"));
    query.addBindValue(SCHEMA_EPOCH_KEY);
    query.addBindValue(epoch);
    if (!query.exec()) {
        logError("setStoredSchemaEpoch", query.lastError().text());
        return false;
    }

    return true;
}

int SyncDatabase::getStoredVersion() const {
    // TODO: PERFORMANCE / OPTIMIZATION
    // Preparing queries dynamically on every call incurs parsing and compilation overhead inside
    // SQLite. For high-frequency query patterns in other thread loops, caching prepared QSqlQuery
    // objects per thread-connection or utilizing a statically cached prepared statement map would
    // yield faster execution cycles and reduced CPU overhead.
    QSqlQuery query(m_db);
    query.prepare("SELECT value FROM settings WHERE key = 'version'");
    if (query.exec() && query.next()) {
        bool ok = false;
        int version = query.value(0).toInt(&ok);
        return ok ? version : 0;
    }

    return 0;
}

bool SyncDatabase::setStoredVersion(int version) {
    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO settings (key, value) VALUES ('version', ?)");
    query.addBindValue(version);
    if (!query.exec()) {
        logError("setStoredVersion", query.lastError().text());
        return false;
    }

    return true;
}

SyncDatabase::SchemaCompatibility SyncDatabase::detectSchemaCompatibility() const {
    const int schemaEpoch = getStoredSchemaEpoch();
    if (schemaEpoch > CURRENT_SCHEMA_EPOCH) {
        return SchemaCompatibility::UnsupportedFutureSchema;
    }
    if (schemaEpoch == CURRENT_SCHEMA_EPOCH) {
        return SchemaCompatibility::Current;
    }

    const int legacyVersion = getStoredVersion();
    if (legacyVersion > DB_VERSION) {
        return SchemaCompatibility::UnsupportedFutureSchema;
    }
    if (legacyVersion == DB_VERSION) {
        return SchemaCompatibility::Current;
    }
    if (legacyVersion > 0) {
        return hasPendingDirtyUploads() ? SchemaCompatibility::ResetBlockedByDirtyState
                                        : SchemaCompatibility::ResetRequired;
    }

    QStringList tables = m_db.tables();
    tables.removeIf([](const QString& tableName) {
        return tableName == QStringLiteral("settings") || tableName.startsWith("sqlite_");
    });
    return tables.isEmpty() ? SchemaCompatibility::Current : SchemaCompatibility::ResetRequired;
}

bool SyncDatabase::removeDatabaseFiles(const QString& dbPath) {
    const QStringList suffixes = {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")};
    for (const QString& suffix : suffixes) {
        const QString candidatePath = dbPath + suffix;
        if (QFile::exists(candidatePath) && !QFile::remove(candidatePath)) {
            return false;
        }
    }

    return true;
}

bool SyncDatabase::recreateDatabaseUnlocked() {
    closeUnlocked();

    if (!removeDatabaseFiles(m_dbPath)) {
        logError("recreateCurrentSchema", QStringLiteral("Failed to remove existing DB files"));
        return false;
    }

    if (!openConnectionUnlocked()) {
        return false;
    }

    if (!ensureSettingsTable()) {
        return false;
    }

    if (!createTables()) {
        return false;
    }

    if (!createFuseTables()) {
        return false;
    }

    if (!setStoredSchemaEpoch(CURRENT_SCHEMA_EPOCH)) {
        return false;
    }

    if (!setStoredVersion(DB_VERSION)) {
        return false;
    }

    m_lastSchemaCompatibility = SchemaCompatibility::Current;
    m_connectionsReady = true;
    return true;
}

bool SyncDatabase::createTables() {
    QSqlQuery query(m_db);

    // Files table (Drive file ID is canonical; local_path is mutable metadata)
    QString createFilesTable = R"(
        CREATE TABLE IF NOT EXISTS files (
            file_id TEXT PRIMARY KEY,
            local_path TEXT UNIQUE NOT NULL,
            modified_time_at_sync TEXT,
            is_folder INTEGER DEFAULT 0,
            remote_md5_at_sync TEXT,
            local_hash_at_sync TEXT
        )
    )";

    if (!query.exec(createFilesTable)) {
        logError("createTables", query.lastError().text());
        return false;
    }

    QString createNativeDocStateTable = R"(
        CREATE TABLE IF NOT EXISTS native_doc_state (
            file_id TEXT PRIMARY KEY,
            remote_name TEXT,
            remote_mime_type TEXT,
            web_view_link TEXT,
            native_doc_mode_override TEXT
        )
    )";

    if (!query.exec(createNativeDocStateTable)) {
        logError("createTables", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "native_doc_state", "remote_name TEXT")) {
        logError("createTables (native_doc_state.remote_name)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "native_doc_state", "remote_mime_type TEXT")) {
        logError("createTables (native_doc_state.remote_mime_type)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "native_doc_state", "web_view_link TEXT")) {
        logError("createTables (native_doc_state.web_view_link)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "native_doc_state", "native_doc_mode_override TEXT")) {
        logError("createTables (native_doc_state.native_doc_mode_override)",
                 query.lastError().text());
        return false;
    }

    // Settings table
    QString createSettingsTable = R"(
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT
        )
    )";

    if (!query.exec(createSettingsTable)) {
        logError("createTables", query.lastError().text());
        return false;
    }

    // Conflicts table
    QString createConflictsTable = R"(
        CREATE TABLE IF NOT EXISTS conflicts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            local_path TEXT NOT NULL,
            file_id TEXT,
            conflict_path TEXT,
            detected_at TEXT,
            resolved INTEGER DEFAULT 0
        )
    )";

    if (!query.exec(createConflictsTable)) {
        logError("createTables", query.lastError().text());
        return false;
    }

    // Conflict versions table
    QString createConflictVersionsTable = R"(
        CREATE TABLE IF NOT EXISTS conflict_versions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            conflict_id INTEGER NOT NULL,
            local_modified_time TEXT,
            remote_modified_time TEXT,
            db_sync_time TEXT,
            detected_at TEXT,
            FOREIGN KEY(conflict_id) REFERENCES conflicts(id)
        )
    )";

    if (!query.exec(createConflictVersionsTable)) {
        logError("createTables", query.lastError().text());
        return false;
    }

    // Deleted files table - tracks local deletions to prevent re-download
    QString createDeletedFilesTable = R"(
        CREATE TABLE IF NOT EXISTS deleted_files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            local_path TEXT UNIQUE NOT NULL,
            file_id TEXT,
            deleted_at TEXT
        )
    )";

    if (!query.exec(createDeletedFilesTable)) {
        logError("createTables", query.lastError().text());
        return false;
    }

    // Create indexes
    query.exec("CREATE INDEX IF NOT EXISTS idx_files_local_path ON files(local_path)");
    query.exec(
        "CREATE INDEX IF NOT EXISTS idx_files_file_id_local_path ON files(file_id, local_path)");
    query.exec(
        "CREATE INDEX IF NOT EXISTS idx_native_doc_state_remote_mime ON "
        "native_doc_state(remote_mime_type)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_conflicts_local_path ON conflicts(local_path)");
    query.exec(
        "CREATE INDEX IF NOT EXISTS idx_conflict_versions_conflict_id ON "
        "conflict_versions(conflict_id)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_deleted_files_file_id ON deleted_files(file_id)");

    return true;
}

void SyncDatabase::saveFileState(const FileSyncState& state) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(state.localPath, "saveFileState");
    requireFileId(state.fileId, "saveFileState");
    QSqlQuery query(m_db);

    query.prepare(R"(
        INSERT INTO files (
            file_id, local_path, modified_time_at_sync, is_folder, remote_md5_at_sync,
            local_hash_at_sync
        )
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_id) DO UPDATE SET
            local_path = excluded.local_path,
            modified_time_at_sync = excluded.modified_time_at_sync,
            is_folder = excluded.is_folder,
            remote_md5_at_sync = excluded.remote_md5_at_sync,
            local_hash_at_sync = excluded.local_hash_at_sync
    )");

    query.addBindValue(state.fileId);
    query.addBindValue(state.localPath);
    query.addBindValue(state.modifiedTimeAtSync.toString(Qt::ISODate));
    query.addBindValue(state.isFolder ? 1 : 0);
    query.addBindValue(state.remoteMd5AtSync);
    query.addBindValue(state.localHashAtSync);

    if (!query.exec()) {
        logError("saveFileState", query.lastError().text());
    }
}

FileSyncState SyncDatabase::getFileState(const QString& localPath) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "getFileState");
    FileSyncState state{};

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM files WHERE local_path = ?");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        state.localPath = query.value("local_path").toString();
        state.fileId = query.value("file_id").toString();

        state.modifiedTimeAtSync =
            QDateTime::fromString(query.value("modified_time_at_sync").toString(), Qt::ISODate);
        state.isFolder = query.value("is_folder").toInt() == 1;
        state.remoteMd5AtSync = query.value("remote_md5_at_sync").toString();
        state.localHashAtSync = query.value("local_hash_at_sync").toString();
    }

    return state;
}

FileSyncState SyncDatabase::getFileStateById(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    FileSyncState state{};
    if (fileId.isEmpty()) {
        return state;
    }

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM files WHERE file_id = ?");
    query.addBindValue(fileId);

    if (query.exec() && query.next()) {
        state.localPath = query.value("local_path").toString();
        state.fileId = query.value("file_id").toString();
        state.modifiedTimeAtSync =
            QDateTime::fromString(query.value("modified_time_at_sync").toString(), Qt::ISODate);
        state.isFolder = query.value("is_folder").toInt() == 1;
        state.remoteMd5AtSync = query.value("remote_md5_at_sync").toString();
        state.localHashAtSync = query.value("local_hash_at_sync").toString();
    }

    return state;
}

QString SyncDatabase::getFileId(const QString& localPath) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "getFileId");
    QSqlQuery query(m_db);
    query.prepare("SELECT file_id FROM files WHERE local_path = ?");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }

    return QString();
}

QString SyncDatabase::getLocalPath(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    if (fileId.isEmpty()) {
        return QString();
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT local_path FROM files WHERE file_id = ?");
    query.addBindValue(fileId);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }

    return QString();
}

void SyncDatabase::setFileId(const QString& localPath, const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "setFileId");
    requireFileId(fileId, "setFileId");
    QSqlQuery query(m_db);

    query.prepare("SELECT local_path FROM files WHERE file_id = ?");
    query.addBindValue(fileId);

    if (query.exec() && query.next()) {
        QString existingPath = query.value(0).toString();
        if (existingPath != localPath) {
            QSqlQuery conflictQuery(m_db);
            conflictQuery.prepare("SELECT file_id FROM files WHERE local_path = ?");
            conflictQuery.addBindValue(localPath);
            if (conflictQuery.exec() && conflictQuery.next()) {
                QString conflictId = conflictQuery.value(0).toString();
                if (!conflictId.isEmpty() && conflictId != fileId) {
                    QSqlQuery removeQuery(m_db);
                    removeQuery.prepare("DELETE FROM files WHERE file_id = ?");
                    removeQuery.addBindValue(conflictId);
                    if (!removeQuery.exec()) {
                        logError("setFileId (remove conflict)", removeQuery.lastError().text());
                    }
                }
            }

            QSqlQuery updateQuery(m_db);
            updateQuery.prepare("UPDATE files SET local_path = ? WHERE file_id = ?");
            updateQuery.addBindValue(localPath);
            updateQuery.addBindValue(fileId);
            if (!updateQuery.exec()) {
                logError("setFileId (update path)", updateQuery.lastError().text());
            }
        }
        return;
    }

    QSqlQuery pathQuery(m_db);
    pathQuery.prepare("SELECT file_id FROM files WHERE local_path = ?");
    pathQuery.addBindValue(localPath);

    if (pathQuery.exec() && pathQuery.next()) {
        QSqlQuery updateQuery(m_db);
        updateQuery.prepare("UPDATE files SET file_id = ? WHERE local_path = ?");
        updateQuery.addBindValue(fileId);
        updateQuery.addBindValue(localPath);
        if (!updateQuery.exec()) {
            logError("setFileId (update id)", updateQuery.lastError().text());
        }
        return;
    }

    QSqlQuery insertQuery(m_db);
    insertQuery.prepare("INSERT INTO files (file_id, local_path) VALUES (?, ?)");
    insertQuery.addBindValue(fileId);
    insertQuery.addBindValue(localPath);

    if (!insertQuery.exec()) {
        logError("setFileId (insert)", insertQuery.lastError().text());
    }
}

void SyncDatabase::setLocalPath(const QString& fileId, const QString& localPath) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "setLocalPath");
    requireFileId(fileId, "setLocalPath");
    QSqlQuery query(m_db);

    query.prepare("SELECT local_path FROM files WHERE file_id = ?");
    query.addBindValue(fileId);

    if (query.exec() && query.next()) {
        QSqlQuery conflictQuery(m_db);
        conflictQuery.prepare("SELECT file_id FROM files WHERE local_path = ?");
        conflictQuery.addBindValue(localPath);
        if (conflictQuery.exec() && conflictQuery.next()) {
            QString conflictId = conflictQuery.value(0).toString();
            if (!conflictId.isEmpty() && conflictId != fileId) {
                QSqlQuery removeQuery(m_db);
                removeQuery.prepare("DELETE FROM files WHERE file_id = ?");
                removeQuery.addBindValue(conflictId);
                if (!removeQuery.exec()) {
                    logError("setLocalPath (remove conflict)", removeQuery.lastError().text());
                }
            }
        }

        QSqlQuery updateQuery(m_db);
        updateQuery.prepare("UPDATE files SET local_path = ? WHERE file_id = ?");
        updateQuery.addBindValue(localPath);
        updateQuery.addBindValue(fileId);
        if (!updateQuery.exec()) {
            logError("setLocalPath (update path)", updateQuery.lastError().text());
        }
        return;
    }

    QSqlQuery insertQuery(m_db);
    insertQuery.prepare("INSERT INTO files (file_id, local_path) VALUES (?, ?)");
    insertQuery.addBindValue(fileId);
    insertQuery.addBindValue(localPath);

    if (!insertQuery.exec()) {
        logError("setLocalPath (insert)", insertQuery.lastError().text());
    }
}

bool SyncDatabase::updateLocalPathTree(const QString& fileId, const QString& oldLocalPath,
                                       const QString& newLocalPath) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(oldLocalPath, "updateLocalPathTree");
    requireRelativePath(newLocalPath, "updateLocalPathTree");
    requireFileId(fileId, "updateLocalPathTree");
    QSqlDatabase db = m_db;

    auto updatePathForFile = [this](const QString& currentFileId, const QString& targetPath,
                                    const char* operation) -> bool {
        QSqlQuery conflictQuery(m_db);
        conflictQuery.prepare("SELECT file_id FROM files WHERE local_path = ?");
        conflictQuery.addBindValue(targetPath);
        if (conflictQuery.exec() && conflictQuery.next()) {
            const QString conflictId = conflictQuery.value(0).toString();
            if (!conflictId.isEmpty() && conflictId != currentFileId) {
                QSqlQuery removeQuery(m_db);
                removeQuery.prepare("DELETE FROM files WHERE file_id = ?");
                removeQuery.addBindValue(conflictId);
                if (!removeQuery.exec()) {
                    logError(QString::fromLatin1(operation) + QStringLiteral(" (remove conflict)"),
                             removeQuery.lastError().text());
                    return false;
                }
            }
        }

        QSqlQuery existingQuery(m_db);
        existingQuery.prepare("SELECT 1 FROM files WHERE file_id = ?");
        existingQuery.addBindValue(currentFileId);
        const bool hasExistingRow = existingQuery.exec() && existingQuery.next();

        QSqlQuery writeQuery(m_db);
        if (hasExistingRow) {
            writeQuery.prepare("UPDATE files SET local_path = ? WHERE file_id = ?");
            writeQuery.addBindValue(targetPath);
            writeQuery.addBindValue(currentFileId);
        } else {
            writeQuery.prepare("INSERT INTO files (file_id, local_path) VALUES (?, ?)");
            writeQuery.addBindValue(currentFileId);
            writeQuery.addBindValue(targetPath);
        }

        if (!writeQuery.exec()) {
            logError(operation, writeQuery.lastError().text());
            return false;
        }

        return true;
    };

    QList<QPair<QString, QString>> descendantUpdates;
    QSqlQuery descendantQuery(m_db);
    if (!descendantQuery.exec("SELECT file_id, local_path FROM files")) {
        logError("updateLocalPathTree (select descendants)", descendantQuery.lastError().text());
        return false;
    }

    while (descendantQuery.next()) {
        const QString descendantFileId = descendantQuery.value("file_id").toString();
        const QString descendantOldPath = descendantQuery.value("local_path").toString();
        if (!descendantOldPath.startsWith(oldLocalPath + "/")) {
            continue;
        }
        const QString suffix = descendantOldPath.mid(oldLocalPath.size());
        descendantUpdates.append(qMakePair(descendantFileId, newLocalPath + suffix));
    }

    if (!db.transaction()) {
        logError("updateLocalPathTree (begin)", db.lastError().text());
        return false;
    }

    if (!updatePathForFile(fileId, newLocalPath, "updateLocalPathTree (root)")) {
        db.rollback();
        return false;
    }

    for (const auto& update : descendantUpdates) {
        if (!updatePathForFile(update.first, update.second, "updateLocalPathTree (descendant)")) {
            db.rollback();
            return false;
        }
    }

    if (!db.commit()) {
        logError("updateLocalPathTree (commit)", db.lastError().text());
        db.rollback();
        return false;
    }

    return true;
}

bool SyncDatabase::deleteFileStateById(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    if (fileId.isEmpty()) {
        return true;
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM files WHERE file_id = ?");
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("deleteFileStateById", query.lastError().text());
        return false;
    }

    return true;
}

QDateTime SyncDatabase::getModifiedTimeAtSync(const QString& localPath) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "getModifiedTimeAtSync");
    QSqlQuery query(m_db);
    query.prepare("SELECT modified_time_at_sync FROM files WHERE local_path = ?");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        return QDateTime::fromString(query.value(0).toString(), Qt::ISODate);
    }

    return QDateTime();
}

void SyncDatabase::setModifiedTimeAtSync(const QString& localPath, const QDateTime& time) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "setModifiedTimeAtSync");
    QSqlQuery query(m_db);
    query.prepare("UPDATE files SET modified_time_at_sync = ? WHERE local_path = ?");
    query.addBindValue(time.toString(Qt::ISODate));
    query.addBindValue(localPath);

    if (!query.exec()) {
        logError("setModifiedTimeAtSyncError", query.lastError().text());
    }
}

QString SyncDatabase::getRemoteMd5AtSync(const QString& localPath) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "getRemoteMd5AtSync");
    QSqlQuery query(m_db);
    query.prepare("SELECT remote_md5_at_sync FROM files WHERE local_path = ?");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }

    return QString();
}

void SyncDatabase::setRemoteMd5AtSync(const QString& localPath, const QString& remoteMd5) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "setRemoteMd5AtSync");
    QSqlQuery query(m_db);
    query.prepare("UPDATE files SET remote_md5_at_sync = ? WHERE local_path = ?");
    query.addBindValue(remoteMd5);
    query.addBindValue(localPath);

    if (!query.exec()) {
        logError("setRemoteMd5AtSync", query.lastError().text());
    }
}

QString SyncDatabase::getLocalHashAtSync(const QString& localPath) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "getLocalHashAtSync");
    QSqlQuery query(m_db);
    query.prepare("SELECT local_hash_at_sync FROM files WHERE local_path = ?");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }

    return QString();
}

void SyncDatabase::setLocalHashAtSync(const QString& localPath, const QString& localHash) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "setLocalHashAtSync");
    QSqlQuery query(m_db);
    query.prepare("UPDATE files SET local_hash_at_sync = ? WHERE local_path = ?");
    query.addBindValue(localHash);
    query.addBindValue(localPath);

    if (!query.exec()) {
        logError("setLocalHashAtSync", query.lastError().text());
    }
}

void SyncDatabase::setContentHashesAtSync(const QString& localPath, const QString& remoteMd5,
                                          const QString& localHash) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "setContentHashesAtSync");
    QSqlQuery query(m_db);
    query.prepare(
        "UPDATE files SET remote_md5_at_sync = ?, local_hash_at_sync = ? WHERE local_path = ?");
    query.addBindValue(remoteMd5);
    query.addBindValue(localHash);
    query.addBindValue(localPath);

    if (!query.exec()) {
        logError("setContentHashesAtSync", query.lastError().text());
    }
}

QList<FileSyncState> SyncDatabase::getAllFiles() const {
    QMutexLocker locker(&m_mutex);
    QList<FileSyncState> files;

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM files");

    if (query.exec()) {
        while (query.next()) {
            FileSyncState state;
            state.localPath = query.value("local_path").toString();
            state.fileId = query.value("file_id").toString();
            state.modifiedTimeAtSync =
                QDateTime::fromString(query.value("modified_time_at_sync").toString(), Qt::ISODate);
            state.isFolder = query.value("is_folder").toInt() == 1;
            state.remoteMd5AtSync = query.value("remote_md5_at_sync").toString();
            state.localHashAtSync = query.value("local_hash_at_sync").toString();
            files.append(state);
        }
    }

    return files;
}

QList<FileSyncState> SyncDatabase::getFileStatesByPrefix(const QString& pathPrefix) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(pathPrefix, "getFileStatesByPrefix");

    QList<FileSyncState> files;
    if (pathPrefix.isEmpty()) {
        return files;
    }

    QString escapedPrefix = pathPrefix;
    escapedPrefix.replace("\\", "\\\\");
    escapedPrefix.replace("%", "\\%");
    escapedPrefix.replace("_", "\\_");

    QSqlQuery query(m_db);
    query.prepare(
        "SELECT local_path, file_id, modified_time_at_sync, is_folder, remote_md5_at_sync, "
        "local_hash_at_sync "
        "FROM files WHERE local_path LIKE ? ESCAPE '\\'");
    query.addBindValue(escapedPrefix + "/%");

    if (query.exec()) {
        while (query.next()) {
            FileSyncState state;
            state.localPath = query.value("local_path").toString();
            state.fileId = query.value("file_id").toString();
            state.modifiedTimeAtSync =
                QDateTime::fromString(query.value("modified_time_at_sync").toString(), Qt::ISODate);
            state.isFolder = query.value("is_folder").toInt() == 1;
            state.remoteMd5AtSync = query.value("remote_md5_at_sync").toString();
            state.localHashAtSync = query.value("local_hash_at_sync").toString();
            files.append(state);
        }
    } else {
        logError("getFileStatesByPrefix", query.lastError().text());
    }

    return files;
}

NativeDocState SyncDatabase::getNativeDocState(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    NativeDocState state;
    if (fileId.isEmpty()) {
        return state;
    }

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM native_doc_state WHERE file_id = ?");
    query.addBindValue(fileId);

    if (query.exec() && query.next()) {
        state = readNativeDocStateRow(query);
    }

    return state;
}

bool SyncDatabase::saveNativeDocState(const NativeDocState& state) {
    QMutexLocker locker(&m_mutex);
    requireFileId(state.fileId, "saveNativeDocState");

    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT OR REPLACE INTO native_doc_state
        (file_id, remote_name, remote_mime_type, web_view_link, native_doc_mode_override)
        VALUES (?, ?, ?, ?, ?)
    )");

    query.addBindValue(state.fileId);
    query.addBindValue(state.remoteName);
    query.addBindValue(state.remoteMimeType);
    query.addBindValue(state.webViewLink);
    query.addBindValue(state.nativeDocModeOverride);

    if (!query.exec()) {
        logError("saveNativeDocState", query.lastError().text());
        return false;
    }

    return true;
}

bool SyncDatabase::deleteNativeDocState(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    if (fileId.isEmpty()) {
        return true;
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM native_doc_state WHERE file_id = ?");
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("deleteNativeDocState", query.lastError().text());
        return false;
    }

    return true;
}

QString SyncDatabase::getChangeToken() const {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("SELECT value FROM settings WHERE key = 'change_token'");

    // Returns empty QString when key doesn't exist (query.next() returns false)
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }

    return QString();
}

void SyncDatabase::setChangeToken(const QString& token) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO settings (key, value) VALUES ('change_token', ?)");
    query.addBindValue(token);

    if (!query.exec()) {
        logError("setChangeToken", query.lastError().text());
    }
}

int SyncDatabase::fileCount() const {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(*) FROM files");

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

void SyncDatabase::logError(const QString& operation, const QString& error) const {
    qWarning() << "SyncDatabase error in" << operation << ":" << error;
    emit databaseError(operation + ": " + error);
}

bool SyncDatabase::isRelativePath(const QString& path) {
    if (path.isEmpty()) {
        return true;
    }
    if (path.startsWith('/')) {
        return false;
    }
    if (path.startsWith("~/")) {
        return false;
    }
    if (path.startsWith("\\\\")) {
        return false;
    }
    if (path.length() >= 2 && path[1] == ':') {
        return false;
    }
    return true;
}

void SyncDatabase::requireRelativePath(const QString& path, const char* operation) const {
    if (!isRelativePath(path)) {
        throw std::invalid_argument(QString("SyncDatabase::%1 requires a relative path: %2")
                                        .arg(QString::fromUtf8(operation), path)
                                        .toStdString());
    }
}

void SyncDatabase::requireFileId(const QString& fileId, const char* operation) const {
    if (fileId.isEmpty()) {
        throw std::invalid_argument(QString("SyncDatabase::%1 requires a non-empty fileId")
                                        .arg(QString::fromUtf8(operation))
                                        .toStdString());
    }
}

void SyncDatabase::markFileDeleted(const QString& localPath, const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "markFileDeleted");
    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT OR REPLACE INTO deleted_files (local_path, file_id, deleted_at)
        VALUES (?, ?, ?)
    )");
    query.addBindValue(localPath);
    query.addBindValue(fileId);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));

    if (!query.exec()) {
        logError("markFileDeleted", query.lastError().text());
    }
}

bool SyncDatabase::wasFileDeleted(const QString& localPath) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "wasFileDeleted");
    QSqlQuery query(m_db);
    query.prepare("SELECT id FROM deleted_files WHERE local_path = ?");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        return true;
    }
    return false;
}

void SyncDatabase::clearDeletedFile(const QString& localPath) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "clearDeletedFile");
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM deleted_files WHERE local_path = ?");
    query.addBindValue(localPath);

    if (!query.exec()) {
        logError("clearDeletedFile", query.lastError().text());
    }
}

int SyncDatabase::purgeOldDeletedRecords(int maxAgeDays) {
    QMutexLocker locker(&m_mutex);
    QDateTime cutoffDate = QDateTime::currentDateTime().addDays(-maxAgeDays);

    // Query file records to clean. To be removed from files table as well
    QSqlQuery filesQuery(m_db);
    filesQuery.prepare("SELECT file_id FROM deleted_files WHERE deleted_at < ?");
    filesQuery.addBindValue(cutoffDate.toString(Qt::ISODate));
    if (filesQuery.exec()) {
        while (filesQuery.next()) {
            QString fileId = filesQuery.value(0).toString();
            // Remove from files table
            QSqlQuery removeFileQuery(m_db);
            removeFileQuery.prepare("DELETE FROM files WHERE file_id = ?");
            removeFileQuery.addBindValue(fileId);
            if (!removeFileQuery.exec()) {
                logError("purgeOldDeletedRecords (remove from files)",
                         removeFileQuery.lastError().text());
            }
        }
    } else {
        logError("purgeOldDeletedRecords (select file_ids)", filesQuery.lastError().text());
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM deleted_files WHERE deleted_at < ?");
    query.addBindValue(cutoffDate.toString(Qt::ISODate));

    if (!query.exec()) {
        logError("purgeOldDeletedRecords", query.lastError().text());
        return 0;
    }

    int rowsAffected = query.numRowsAffected();
    if (rowsAffected > 0) {
        qInfo() << "Purged" << rowsAffected << "deleted file records older than" << maxAgeDays
                << "days";
    }

    return rowsAffected;
}

int SyncDatabase::upsertConflictRecord(const QString& localPath, const QString& fileId,
                                       const QString& conflictPath) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "upsertConflictRecord");
    QSqlQuery query(m_db);
    query.prepare(
        "SELECT id FROM conflicts WHERE local_path = ? AND resolved = 0 "
        "ORDER BY detected_at DESC LIMIT 1");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        int conflictId = query.value(0).toInt();
        QSqlQuery updateQuery(m_db);
        updateQuery.prepare(
            "UPDATE conflicts SET file_id = ?, conflict_path = ?, detected_at = ? WHERE id = ?");
        updateQuery.addBindValue(fileId);
        updateQuery.addBindValue(conflictPath);
        updateQuery.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
        updateQuery.addBindValue(conflictId);
        if (!updateQuery.exec()) {
            logError("upsertConflictRecord (update)", updateQuery.lastError().text());
        }
        return conflictId;
    }

    QSqlQuery insertQuery(m_db);
    insertQuery.prepare(R"(
        INSERT INTO conflicts (local_path, file_id, conflict_path, detected_at, resolved)
        VALUES (?, ?, ?, ?, 0)
    )");
    insertQuery.addBindValue(localPath);
    insertQuery.addBindValue(fileId);
    insertQuery.addBindValue(conflictPath);
    insertQuery.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));

    if (!insertQuery.exec()) {
        logError("upsertConflictRecord (insert)", insertQuery.lastError().text());
        return -1;
    }

    return insertQuery.lastInsertId().toInt();
}

void SyncDatabase::addConflictVersion(int conflictId, const ConflictVersion& version) {
    QMutexLocker locker(&m_mutex);
    if (conflictId <= 0) {
        return;
    }
    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT INTO conflict_versions
            (conflict_id, local_modified_time, remote_modified_time, db_sync_time, detected_at)
        VALUES (?, ?, ?, ?, ?)
    )");
    query.addBindValue(conflictId);
    query.addBindValue(version.localModifiedTime.toString(Qt::ISODate));
    query.addBindValue(version.remoteModifiedTime.toString(Qt::ISODate));
    query.addBindValue(version.dbSyncTime.toString(Qt::ISODate));
    QDateTime detectedAt =
        version.detectedAt.isValid() ? version.detectedAt : QDateTime::currentDateTime();
    query.addBindValue(detectedAt.toString(Qt::ISODate));

    if (!query.exec()) {
        logError("addConflictVersion", query.lastError().text());
    }
}

bool SyncDatabase::hasUnresolvedConflict(const QString& localPath) const {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "hasUnresolvedConflict");
    QSqlQuery query(m_db);
    query.prepare("SELECT id FROM conflicts WHERE local_path = ? AND resolved = 0 LIMIT 1");
    query.addBindValue(localPath);

    if (query.exec() && query.next()) {
        return true;
    }
    return false;
}

QList<ConflictRecord> SyncDatabase::getUnresolvedConflicts() {
    QMutexLocker locker(&m_mutex);
    QList<ConflictRecord> records;
    QSqlQuery query(m_db);
    query.prepare(
        "SELECT id, local_path, file_id, conflict_path, detected_at, resolved "
        "FROM conflicts WHERE resolved = 0 ORDER BY detected_at ASC");

    if (!query.exec()) {
        logError("getUnresolvedConflicts", query.lastError().text());
        return records;
    }

    while (query.next()) {
        ConflictRecord record;
        record.id = query.value("id").toInt();
        record.localPath = query.value("local_path").toString();
        record.fileId = query.value("file_id").toString();
        record.conflictPath = query.value("conflict_path").toString();
        record.detectedAt =
            QDateTime::fromString(query.value("detected_at").toString(), Qt::ISODate);
        record.resolved = query.value("resolved").toInt() == 1;

        QSqlQuery versionQuery(m_db);
        versionQuery.prepare(
            "SELECT id, local_modified_time, remote_modified_time, "
            "db_sync_time, detected_at FROM conflict_versions "
            "WHERE conflict_id = ? ORDER BY detected_at ASC");
        versionQuery.addBindValue(record.id);
        if (!versionQuery.exec()) {
            logError("getUnresolvedConflicts (versions)", versionQuery.lastError().text());
        } else {
            while (versionQuery.next()) {
                ConflictVersion version;
                version.id = versionQuery.value("id").toInt();
                version.localModifiedTime = QDateTime::fromString(
                    versionQuery.value("local_modified_time").toString(), Qt::ISODate);
                version.remoteModifiedTime = QDateTime::fromString(
                    versionQuery.value("remote_modified_time").toString(), Qt::ISODate);
                version.dbSyncTime = QDateTime::fromString(
                    versionQuery.value("db_sync_time").toString(), Qt::ISODate);
                version.detectedAt = QDateTime::fromString(
                    versionQuery.value("detected_at").toString(), Qt::ISODate);
                record.versions.append(version);
            }
        }

        records.append(record);
    }

    return records;
}

void SyncDatabase::markConflictResolved(const QString& localPath) {
    QMutexLocker locker(&m_mutex);
    requireRelativePath(localPath, "markConflictResolved");
    QSqlQuery query(m_db);
    query.prepare("UPDATE conflicts SET resolved = 1 WHERE local_path = ? AND resolved = 0");
    query.addBindValue(localPath);
    if (!query.exec()) {
        logError("markConflictResolved", query.lastError().text());
    }
}

void SyncDatabase::markConflictResolved(int conflictId) {
    QMutexLocker locker(&m_mutex);
    if (conflictId <= 0) {
        return;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE conflicts SET resolved = 1 WHERE id = ?");
    query.addBindValue(conflictId);
    if (!query.exec()) {
        logError("markConflictResolved(id)", query.lastError().text());
    }
}

// ============================================================================
// FUSE-specific operations
// ============================================================================

bool SyncDatabase::createFuseTables() {
    QSqlQuery query(m_db);

    // FUSE metadata table
    QString createFuseMetadataTable = R"(
        CREATE TABLE IF NOT EXISTS fuse_metadata (
            file_id TEXT PRIMARY KEY,
            path TEXT NOT NULL,
            name TEXT NOT NULL,
            parent_id TEXT,
            is_folder INTEGER NOT NULL DEFAULT 0,
            size INTEGER DEFAULT 0,
            mime_type TEXT,
            created_time TEXT,
            modified_time TEXT,
            cached_at TEXT NOT NULL,
            last_accessed TEXT
        )
    )";

    if (!query.exec(createFuseMetadataTable)) {
        logError("createFuseTables (fuse_metadata)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "fuse_metadata", "remote_name TEXT")) {
        logError("createFuseTables (fuse_metadata.remote_name)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "fuse_metadata", "remote_mime_type TEXT")) {
        logError("createFuseTables (fuse_metadata.remote_mime_type)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "fuse_metadata", "web_view_link TEXT")) {
        logError("createFuseTables (fuse_metadata.web_view_link)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "fuse_metadata", "native_doc_mode_override TEXT")) {
        logError("createFuseTables (fuse_metadata.native_doc_mode_override)",
                 query.lastError().text());
        return false;
    }

    // FUSE dirty files table
    QString createFuseDirtyFilesTable = R"(
        CREATE TABLE IF NOT EXISTS fuse_dirty_files (
            file_id TEXT PRIMARY KEY,
            path TEXT NOT NULL,
            marked_dirty_at TEXT NOT NULL,
            last_upload_attempt TEXT,
            upload_failed INTEGER DEFAULT 0
        )
    )";

    if (!query.exec(createFuseDirtyFilesTable)) {
        logError("createFuseTables (fuse_dirty_files)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "fuse_dirty_files", "generation INTEGER NOT NULL DEFAULT 1")) {
        logError("createFuseTables (fuse_dirty_files.generation)", query.lastError().text());
        return false;
    }

    if (!addColumnIfMissing(m_db, "fuse_dirty_files",
                            "uploaded_generation INTEGER NOT NULL DEFAULT 0")) {
        logError("createFuseTables (fuse_dirty_files.uploaded_generation)",
                 query.lastError().text());
        return false;
    }

    // FUSE cache entries table
    QString createFuseCacheEntriesTable = R"(
        CREATE TABLE IF NOT EXISTS fuse_cache_entries (
            file_id TEXT PRIMARY KEY,
            cache_path TEXT NOT NULL,
            size INTEGER NOT NULL,
            last_accessed TEXT NOT NULL,
            download_completed TEXT NOT NULL
        )
    )";

    if (!query.exec(createFuseCacheEntriesTable)) {
        logError("createFuseTables (fuse_cache_entries)", query.lastError().text());
        return false;
    }

    // FUSE sync state table
    QString createFuseSyncStateTable = R"(
        CREATE TABLE IF NOT EXISTS fuse_sync_state (
            key TEXT PRIMARY KEY,
            value TEXT
        )
    )";

    if (!query.exec(createFuseSyncStateTable)) {
        logError("createFuseTables (fuse_sync_state)", query.lastError().text());
        return false;
    }

    // Create indexes for FUSE tables
    query.exec("CREATE INDEX IF NOT EXISTS idx_fuse_metadata_path ON fuse_metadata(path)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_fuse_metadata_parent ON fuse_metadata(parent_id)");
    query.exec(
        "CREATE INDEX IF NOT EXISTS idx_fuse_cache_last_accessed ON "
        "fuse_cache_entries(last_accessed)");

    qDebug() << "FUSE tables created successfully";
    return true;
}

FuseMetadata SyncDatabase::getFuseMetadata(const QString& fileId) const {
    QMutexLocker locker(&m_mutex);
    FuseMetadata metadata{};

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM fuse_metadata WHERE file_id = ?");
    query.addBindValue(fileId);

    if (query.exec() && query.next()) {
        metadata = readFuseMetadataRow(query);
    }

    return metadata;
}

FuseMetadata SyncDatabase::getFuseMetadataByPath(const QString& path) const {
    QMutexLocker locker(&m_mutex);
    FuseMetadata metadata{};

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM fuse_metadata WHERE path = ?");
    query.addBindValue(path);

    if (query.exec() && query.next()) {
        metadata = readFuseMetadataRow(query);
    }

    return metadata;
}

bool SyncDatabase::saveFuseMetadata(const FuseMetadata& metadata) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);

    QSqlQuery deleteConflictQuery(m_db);
    deleteConflictQuery.prepare("DELETE FROM fuse_metadata WHERE path = ? AND file_id != ?");
    deleteConflictQuery.addBindValue(metadata.path);
    deleteConflictQuery.addBindValue(metadata.fileId);
    if (!deleteConflictQuery.exec()) {
        logError("saveFuseMetadata (delete conflicting path)",
                 deleteConflictQuery.lastError().text());
        return false;
    }

    query.prepare(R"(
        INSERT OR REPLACE INTO fuse_metadata 
        (file_id, path, name, remote_name, native_doc_mode_override, parent_id,
         is_folder, size, mime_type,
         remote_mime_type, web_view_link,
         created_time, modified_time, cached_at, last_accessed)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");

    query.addBindValue(metadata.fileId);
    query.addBindValue(metadata.path);
    query.addBindValue(metadata.name);
    query.addBindValue(metadata.remoteName.isEmpty() ? metadata.name : metadata.remoteName);
    query.addBindValue(metadata.nativeDocModeOverride);
    query.addBindValue(metadata.parentId);
    query.addBindValue(metadata.isFolder ? 1 : 0);
    query.addBindValue(metadata.size);
    query.addBindValue(metadata.mimeType);
    query.addBindValue(metadata.remoteMimeType);
    query.addBindValue(metadata.webViewLink);
    query.addBindValue(metadata.createdTime.toString(Qt::ISODate));
    query.addBindValue(metadata.modifiedTime.toString(Qt::ISODate));
    query.addBindValue(metadata.cachedAt.toString(Qt::ISODate));
    query.addBindValue(metadata.lastAccessed.toString(Qt::ISODate));

    if (!query.exec()) {
        logError("saveFuseMetadata", query.lastError().text());
        return false;
    }

    NativeDocState state;
    state.fileId = metadata.fileId;
    state.remoteName = metadata.remoteName.isEmpty() ? metadata.name : metadata.remoteName;
    state.remoteMimeType = metadata.remoteMimeType;
    state.webViewLink = metadata.webViewLink;
    state.nativeDocModeOverride = metadata.nativeDocModeOverride;

    if (shouldPersistNativeDocState(state)) {
        if (!saveNativeDocState(state)) {
            return false;
        }
    } else if (!deleteNativeDocState(metadata.fileId)) {
        return false;
    }

    return true;
}

bool SyncDatabase::deleteFuseMetadata(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM fuse_metadata WHERE file_id = ?");
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("deleteFuseMetadata", query.lastError().text());
        return false;
    }

    return true;
}

QList<FuseMetadata> SyncDatabase::getFuseChildren(const QString& parentId) const {
    QMutexLocker locker(&m_mutex);
    QList<FuseMetadata> children;

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM fuse_metadata WHERE parent_id = ?");
    query.addBindValue(parentId);

    if (query.exec()) {
        while (query.next()) {
            children.append(readFuseMetadataRow(query));
        }
    }

    return children;
}

QList<FuseMetadata> SyncDatabase::getAllFuseMetadata() const {
    QMutexLocker locker(&m_mutex);
    QList<FuseMetadata> result;

    QSqlQuery query(m_db);
    if (query.exec("SELECT * FROM fuse_metadata")) {
        while (query.next()) {
            result.append(readFuseMetadataRow(query));
        }
    }

    return result;
}

int SyncDatabase::updateFuseChildrenPaths(const QString& parentFileId, const QString& oldParentPath,
                                          const QString& newParentPath) {
    QMutexLocker locker(&m_mutex);
    // Recursively update paths of all descendants
    int updated = 0;
    QList<FuseMetadata> children = getFuseChildren(parentFileId);

    for (const FuseMetadata& child : children) {
        FuseMetadata updatedChild = child;
        // Replace old prefix with new prefix in path
        if (updatedChild.path.startsWith(oldParentPath + "/")) {
            updatedChild.path = newParentPath + updatedChild.path.mid(oldParentPath.length());
        } else if (updatedChild.path == oldParentPath) {
            updatedChild.path = newParentPath;
        }
        updatedChild.cachedAt = QDateTime::currentDateTime();
        saveFuseMetadata(updatedChild);
        updated++;

        // Recurse into sub-directories
        if (updatedChild.isFolder) {
            updated +=
                updateFuseChildrenPaths(updatedChild.fileId, oldParentPath + "/" + child.name,
                                        newParentPath + "/" + child.name);
        }
    }

    return updated;
}

QList<FuseDirtyFile> SyncDatabase::getFuseDirtyFiles() const {
    QMutexLocker locker(&m_mutex);
    QList<FuseDirtyFile> dirtyFiles;

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM fuse_dirty_files");

    if (query.exec()) {
        while (query.next()) {
            FuseDirtyFile entry;
            entry.fileId = query.value("file_id").toString();
            entry.path = query.value("path").toString();
            entry.markedDirtyAt =
                QDateTime::fromString(query.value("marked_dirty_at").toString(), Qt::ISODate);
            entry.lastUploadAttempt =
                QDateTime::fromString(query.value("last_upload_attempt").toString(), Qt::ISODate);
            entry.uploadFailed = query.value("upload_failed").toInt() != 0;
            const int generationIndex = query.record().indexOf("generation");
            if (generationIndex >= 0) {
                entry.generation = query.value(generationIndex).toULongLong();
            }
            const int uploadedGenerationIndex = query.record().indexOf("uploaded_generation");
            if (uploadedGenerationIndex >= 0) {
                entry.uploadedGeneration = query.value(uploadedGenerationIndex).toULongLong();
            }
            dirtyFiles.append(entry);
        }
    }

    return dirtyFiles;
}

bool SyncDatabase::hasPendingDirtyUploads() const {
    QMutexLocker locker(&m_mutex);
    if (!m_db.isOpen() || !tableExists(m_db, QStringLiteral("fuse_dirty_files"))) {
        return false;
    }

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT 1 FROM fuse_dirty_files LIMIT 1"))) {
        return false;
    }

    return query.next();
}

bool SyncDatabase::markFuseDirty(const QString& fileId, const QString& path, quint64 generation,
                                 quint64 uploadedGeneration) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);

    // Use INSERT OR IGNORE to avoid overwriting existing entries
    // This preserves failure tracking information if file was already dirty
    query.prepare(R"(
        INSERT OR IGNORE INTO fuse_dirty_files 
        (file_id, path, marked_dirty_at, upload_failed, generation, uploaded_generation)
        VALUES (?, ?, ?, 0, ?, ?)
    )");

    query.addBindValue(fileId);
    query.addBindValue(path);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    query.addBindValue(static_cast<qulonglong>(generation));
    query.addBindValue(static_cast<qulonglong>(uploadedGeneration));

    if (!query.exec()) {
        logError("markFuseDirty", query.lastError().text());
        return false;
    }

    // If insert was ignored (file already dirty), update the marked_dirty_at timestamp
    // but preserve the upload_failed and last_upload_attempt values
    if (query.numRowsAffected() == 0) {
        query.prepare(R"(
            UPDATE fuse_dirty_files 
            SET path = ?, marked_dirty_at = ?, generation = ?,
                uploaded_generation = MAX(uploaded_generation, ?)
            WHERE file_id = ?
        )");
        query.addBindValue(path);
        query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
        query.addBindValue(static_cast<qulonglong>(generation));
        query.addBindValue(static_cast<qulonglong>(uploadedGeneration));
        query.addBindValue(fileId);

        if (!query.exec()) {
            logError("markFuseDirty (update)", query.lastError().text());
            return false;
        }
    }

    return true;
}

bool SyncDatabase::clearFuseDirty(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM fuse_dirty_files WHERE file_id = ?");
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("clearFuseDirty", query.lastError().text());
        return false;
    }

    return true;
}

bool SyncDatabase::markFuseUploadFailed(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(R"(
        UPDATE fuse_dirty_files 
        SET upload_failed = 1, last_upload_attempt = ?
        WHERE file_id = ?
    )");
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("markFuseUploadFailed", query.lastError().text());
        return false;
    }

    return true;
}

bool SyncDatabase::markFuseUploadedGeneration(const QString& fileId, quint64 uploadedGeneration) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(R"(
        UPDATE fuse_dirty_files
        SET uploaded_generation = MAX(uploaded_generation, ?),
            upload_failed = 0,
            last_upload_attempt = ?
        WHERE file_id = ?
    )");
    query.addBindValue(static_cast<qulonglong>(uploadedGeneration));
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("markFuseUploadedGeneration", query.lastError().text());
        return false;
    }

    return true;
}

bool SyncDatabase::clearAllFuseCacheEntries() {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);

    if (!query.exec("DELETE FROM fuse_cache_entries")) {
        logError("clearAllFuseCacheEntries", query.lastError().text());
        return false;
    }

    qInfo() << "Cleared all FUSE cache entries from database";
    return true;
}

QList<FuseCacheEntry> SyncDatabase::getFuseCacheEntries() const {
    QMutexLocker locker(&m_mutex);
    QList<FuseCacheEntry> entries;

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM fuse_cache_entries");

    if (query.exec()) {
        while (query.next()) {
            FuseCacheEntry entry;
            entry.fileId = query.value("file_id").toString();
            entry.cachePath = query.value("cache_path").toString();
            entry.size = query.value("size").toLongLong();
            entry.lastAccessed =
                QDateTime::fromString(query.value("last_accessed").toString(), Qt::ISODate);
            entry.downloadCompleted =
                QDateTime::fromString(query.value("download_completed").toString(), Qt::ISODate);
            entries.append(entry);
        }
    }

    return entries;
}

bool SyncDatabase::recordFuseCacheEntry(const QString& fileId, const QString& cachePath,
                                        qint64 size) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);

    QString now = QDateTime::currentDateTime().toString(Qt::ISODate);

    query.prepare(R"(
        INSERT OR REPLACE INTO fuse_cache_entries 
        (file_id, cache_path, size, last_accessed, download_completed)
        VALUES (?, ?, ?, ?, ?)
    )");

    query.addBindValue(fileId);
    query.addBindValue(cachePath);
    query.addBindValue(size);
    query.addBindValue(now);
    query.addBindValue(now);

    if (!query.exec()) {
        logError("recordFuseCacheEntry", query.lastError().text());
        return false;
    }

    return true;
}

bool SyncDatabase::updateCacheAccessTime(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);

    query.prepare("UPDATE fuse_cache_entries SET last_accessed = ? WHERE file_id = ?");
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("updateCacheAccessTime", query.lastError().text());
        return false;
    }

    return true;
}

bool SyncDatabase::evictFuseCacheEntry(const QString& fileId) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM fuse_cache_entries WHERE file_id = ?");
    query.addBindValue(fileId);

    if (!query.exec()) {
        logError("evictFuseCacheEntry", query.lastError().text());
        return false;
    }

    return true;
}

QString SyncDatabase::getFuseSyncState(const QString& key) const {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("SELECT value FROM fuse_sync_state WHERE key = ?");
    query.addBindValue(key);

    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }

    return QString();
}

bool SyncDatabase::setFuseSyncState(const QString& key, const QString& value) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO fuse_sync_state (key, value) VALUES (?, ?)");
    query.addBindValue(key);
    query.addBindValue(value);

    if (!query.exec()) {
        logError("setFuseSyncState", query.lastError().text());
        return false;
    }

    return true;
}

bool SyncDatabase::clearAllData() {
    QMutexLocker locker(&m_mutex);

    // Order matters: delete from child tables before parents to satisfy
    // any future foreign-key constraints without requiring PRAGMA changes.
    static const char* tables[] = {
        "conflict_versions", "conflicts",        "deleted_files",      "files",
        "native_doc_state",  "fuse_dirty_files", "fuse_cache_entries", "fuse_metadata",
        "fuse_sync_state",   "settings"};
    QSqlQuery query(m_db);
    for (const char* table : tables) {
        QString sql;
        if (QLatin1String(table) == QLatin1String("settings")) {
            // Preserve schema metadata so initialize() can reopen the cleared
            // database without treating sign-out as an incompatible reset.
            sql = QStringLiteral("DELETE FROM settings WHERE key NOT IN ('version', '%1')")
                      .arg(SCHEMA_EPOCH_KEY);
        } else {
            sql = QStringLiteral("DELETE FROM %1").arg(QLatin1String(table));
        }
        if (!query.exec(sql)) {
            logError("clearAllData", QStringLiteral("Failed to clear %1: %2")
                                         .arg(QLatin1String(table), query.lastError().text()));
            return false;
        }
    }

    qInfo() << "SyncDatabase: all user data cleared (account sign-out)";
    return true;
}

bool SyncDatabase::clearFuseRepresentationState() {
    QMutexLocker locker(&m_mutex);

    static const char* tables[] = {"fuse_metadata", "fuse_cache_entries", "fuse_sync_state"};
    QSqlQuery query(m_db);
    for (const char* table : tables) {
        QString sql = QStringLiteral("DELETE FROM %1").arg(QLatin1String(table));
        if (!query.exec(sql)) {
            logError("clearFuseRepresentationState",
                     QStringLiteral("Failed to clear %1: %2")
                         .arg(QLatin1String(table), query.lastError().text()));
            return false;
        }
    }

    qInfo() << "SyncDatabase: FUSE representation state cleared (mode change)";
    return true;
}
