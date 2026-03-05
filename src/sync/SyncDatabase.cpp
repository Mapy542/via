/**
 * @file SyncDatabase.cpp
 * @brief Implementation of SQLite sync database
 */

#include "SyncDatabase.h"

#include <QDebug>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <stdexcept>

const QString SyncDatabase::DB_NAME = "via_sync.db";
const int SyncDatabase::DB_VERSION = 5;

namespace {
bool tableHasColumn(QSqlDatabase& db, const QString& tableName, const QString& columnName) {
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

bool addColumnIfMissing(QSqlDatabase& db, const QString& tableName, const QString& columnDef) {
    const QString columnName = columnDef.section(' ', 0, 0);
    if (tableHasColumn(db, tableName, columnName)) {
        return true;
    }

    QSqlQuery query(db);
    return query.exec(QString("ALTER TABLE %1 ADD COLUMN %2").arg(tableName, columnDef));
}
}  // namespace

SyncDatabase::SyncDatabase(QObject* parent) : QObject(parent), m_concurrentAccessCount(0) {
    // Set database path
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_dbPath = dataPath + "/" + DB_NAME;
}

SyncDatabase::~SyncDatabase() { close(); }

bool SyncDatabase::initialize() {
    QMutexLocker locker(&m_mutex);
    // Ensure data directory exists
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // Open database
    m_db = QSqlDatabase::addDatabase("QSQLITE", "sync_connection");
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        logError("initialize", m_db.lastError().text());
        return false;
    }

    // Enable WAL mode for better concurrent read/write performance
    {
        QSqlQuery walQuery(m_db);
        if (!walQuery.exec("PRAGMA journal_mode=WAL")) {
            qWarning() << "Failed to enable WAL mode:" << walQuery.lastError().text();
        }
    }

    if (!ensureSettingsTable()) {
        return false;
    }

    int currentVersion = getStoredVersion();
    if (currentVersion > DB_VERSION) {
        logError("initialize",
                 QString("Database version %1 is newer than supported %2").arg(currentVersion).arg(DB_VERSION));
        return false;
    }

    if (currentVersion < DB_VERSION) {
        if (!migrateDatabase(currentVersion)) {
            return false;
        }
    }

    // Ensure all tables/indexes exist for the current schema.
    if (!createTables()) {
        return false;
    }

    if (!setStoredVersion(DB_VERSION)) {
        return false;
    }

    // Create FUSE-specific tables (isolated from Mirror Sync)
    if (!createFuseTables()) {
        return false;
    }

    qInfo() << "Sync database initialized at:" << m_dbPath;
    return true;
}

void SyncDatabase::close() {
    QMutexLocker locker(&m_mutex);
    if (m_db.isOpen()) {
        m_db.close();
    }

    const QString connectionName = m_db.connectionName();
    m_db = QSqlDatabase();
    if (!connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(connectionName);
    }
}

bool SyncDatabase::isOpen() const {
    QMutexLocker locker(&m_mutex);
    return m_db.isOpen();
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

int SyncDatabase::getStoredVersion() const {
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

bool SyncDatabase::migrateDatabase(int currentVersion) {
    if (currentVersion == 0) {
        return true;
    }

    if (currentVersion < 2) {
        if (!migrateFromV1ToV2()) {
            return false;
        }
        currentVersion = 2;
    }

    if (currentVersion < 3) {
        currentVersion = 3;
    }

    if (currentVersion < 4) {
        currentVersion = 4;
    }

    if (currentVersion < 5) {
        if (!addColumnIfMissing(m_db, "files", "remote_md5_at_sync TEXT")) {
            logError("migrateDatabase (add remote_md5_at_sync)", m_db.lastError().text());
            return false;
        }
        if (!addColumnIfMissing(m_db, "files", "local_hash_at_sync TEXT")) {
            logError("migrateDatabase (add local_hash_at_sync)", m_db.lastError().text());
            return false;
        }
        currentVersion = 5;
    }

    if (currentVersion == DB_VERSION) {
        return true;
    }

    logError("migrateDatabase", QString("No migration path from version %1 to %2").arg(currentVersion).arg(DB_VERSION));
    return false;
}

bool SyncDatabase::migrateFromV1ToV2() {
    QSqlQuery query(m_db);

    if (!query.exec("BEGIN")) {
        logError("migrateFromV1ToV2 (begin)", query.lastError().text());
        return false;
    }

    if (!query.exec(R"(
        CREATE TABLE IF NOT EXISTS files_new (
            file_id TEXT PRIMARY KEY,
            local_path TEXT UNIQUE NOT NULL,
            modified_time_at_sync TEXT,
            is_folder INTEGER DEFAULT 0
        )
    )")) {
        logError("migrateFromV1ToV2 (create files_new)", query.lastError().text());
        query.exec("ROLLBACK");
        return false;
    }

    QSqlQuery selectQuery(m_db);
    if (!selectQuery.exec("SELECT file_id, local_path, modified_time_at_sync, is_folder FROM files")) {
        logError("migrateFromV1ToV2 (select files)", selectQuery.lastError().text());
        query.exec("ROLLBACK");
        return false;
    }

    int skippedEmptyIds = 0;
    int duplicateIds = 0;

    while (selectQuery.next()) {
        const QString fileId = selectQuery.value(0).toString();
        const QString localPath = selectQuery.value(1).toString();
        const QString modifiedTime = selectQuery.value(2).toString();
        const int isFolder = selectQuery.value(3).toInt();

        if (fileId.isEmpty()) {
            skippedEmptyIds++;
            continue;
        }

        QSqlQuery insertQuery(m_db);
        insertQuery.prepare(
            "INSERT OR IGNORE INTO files_new (file_id, local_path, modified_time_at_sync, "
            "is_folder) "
            "VALUES (?, ?, ?, ?)");
        insertQuery.addBindValue(fileId);
        insertQuery.addBindValue(localPath);
        insertQuery.addBindValue(modifiedTime);
        insertQuery.addBindValue(isFolder);

        if (!insertQuery.exec()) {
            logError("migrateFromV1ToV2 (insert files_new)", insertQuery.lastError().text());
            query.exec("ROLLBACK");
            return false;
        }

        if (insertQuery.numRowsAffected() == 0) {
            duplicateIds++;
        }
    }

    if (!query.exec("DROP TABLE files")) {
        logError("migrateFromV1ToV2 (drop files)", query.lastError().text());
        query.exec("ROLLBACK");
        return false;
    }

    if (!query.exec("ALTER TABLE files_new RENAME TO files")) {
        logError("migrateFromV1ToV2 (rename files_new)", query.lastError().text());
        query.exec("ROLLBACK");
        return false;
    }

    if (!query.exec("COMMIT")) {
        logError("migrateFromV1ToV2 (commit)", query.lastError().text());
        query.exec("ROLLBACK");
        return false;
    }

    if (skippedEmptyIds > 0) {
        qWarning() << "Migration skipped" << skippedEmptyIds << "file rows without file_id";
    }

    if (duplicateIds > 0) {
        qWarning() << "Migration skipped" << duplicateIds << "duplicate file_id rows";
    }

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
    query.exec("CREATE INDEX IF NOT EXISTS idx_files_file_id_local_path ON files(file_id, local_path)");
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

        state.modifiedTimeAtSync = QDateTime::fromString(query.value("modified_time_at_sync").toString(), Qt::ISODate);
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
        state.modifiedTimeAtSync = QDateTime::fromString(query.value("modified_time_at_sync").toString(), Qt::ISODate);
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
    query.prepare("UPDATE files SET remote_md5_at_sync = ?, local_hash_at_sync = ? WHERE local_path = ?");
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

QString SyncDatabase::getChangeToken() const {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare("SELECT value FROM settings WHERE key = 'change_token'");

    // TODO: Validate edge case of no key existing
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
        throw std::invalid_argument(
            QString("SyncDatabase::%1 requires a non-empty fileId").arg(QString::fromUtf8(operation)).toStdString());
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
                logError("purgeOldDeletedRecords (remove from files)", removeFileQuery.lastError().text());
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
        qInfo() << "Purged" << rowsAffected << "deleted file records older than" << maxAgeDays << "days";
    }

    return rowsAffected;
}

int SyncDatabase::upsertConflictRecord(const QString& localPath, const QString& fileId, const QString& conflictPath) {
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
        updateQuery.prepare("UPDATE conflicts SET file_id = ?, conflict_path = ?, detected_at = ? WHERE id = ?");
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
    QDateTime detectedAt = version.detectedAt.isValid() ? version.detectedAt : QDateTime::currentDateTime();
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
        record.detectedAt = QDateTime::fromString(query.value("detected_at").toString(), Qt::ISODate);
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
                version.localModifiedTime =
                    QDateTime::fromString(versionQuery.value("local_modified_time").toString(), Qt::ISODate);
                version.remoteModifiedTime =
                    QDateTime::fromString(versionQuery.value("remote_modified_time").toString(), Qt::ISODate);
                version.dbSyncTime = QDateTime::fromString(versionQuery.value("db_sync_time").toString(), Qt::ISODate);
                version.detectedAt = QDateTime::fromString(versionQuery.value("detected_at").toString(), Qt::ISODate);
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
        metadata.fileId = query.value("file_id").toString();
        metadata.path = query.value("path").toString();
        metadata.name = query.value("name").toString();
        metadata.parentId = query.value("parent_id").toString();
        metadata.isFolder = query.value("is_folder").toInt() == 1;
        metadata.size = query.value("size").toLongLong();
        metadata.mimeType = query.value("mime_type").toString();
        metadata.createdTime = QDateTime::fromString(query.value("created_time").toString(), Qt::ISODate);
        metadata.modifiedTime = QDateTime::fromString(query.value("modified_time").toString(), Qt::ISODate);
        metadata.cachedAt = QDateTime::fromString(query.value("cached_at").toString(), Qt::ISODate);
        metadata.lastAccessed = QDateTime::fromString(query.value("last_accessed").toString(), Qt::ISODate);
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
        metadata.fileId = query.value("file_id").toString();
        metadata.path = query.value("path").toString();
        metadata.name = query.value("name").toString();
        metadata.parentId = query.value("parent_id").toString();
        metadata.isFolder = query.value("is_folder").toInt() == 1;
        metadata.size = query.value("size").toLongLong();
        metadata.mimeType = query.value("mime_type").toString();
        metadata.createdTime = QDateTime::fromString(query.value("created_time").toString(), Qt::ISODate);
        metadata.modifiedTime = QDateTime::fromString(query.value("modified_time").toString(), Qt::ISODate);
        metadata.cachedAt = QDateTime::fromString(query.value("cached_at").toString(), Qt::ISODate);
        metadata.lastAccessed = QDateTime::fromString(query.value("last_accessed").toString(), Qt::ISODate);
    }

    return metadata;
}

bool SyncDatabase::saveFuseMetadata(const FuseMetadata& metadata) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);

    query.prepare(R"(
        INSERT OR REPLACE INTO fuse_metadata 
        (file_id, path, name, parent_id, is_folder, size, mime_type, 
         created_time, modified_time, cached_at, last_accessed)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");

    query.addBindValue(metadata.fileId);
    query.addBindValue(metadata.path);
    query.addBindValue(metadata.name);
    query.addBindValue(metadata.parentId);
    query.addBindValue(metadata.isFolder ? 1 : 0);
    query.addBindValue(metadata.size);
    query.addBindValue(metadata.mimeType);
    query.addBindValue(metadata.createdTime.toString(Qt::ISODate));
    query.addBindValue(metadata.modifiedTime.toString(Qt::ISODate));
    query.addBindValue(metadata.cachedAt.toString(Qt::ISODate));
    query.addBindValue(metadata.lastAccessed.toString(Qt::ISODate));

    if (!query.exec()) {
        logError("saveFuseMetadata", query.lastError().text());
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
            FuseMetadata metadata;
            metadata.fileId = query.value("file_id").toString();
            metadata.path = query.value("path").toString();
            metadata.name = query.value("name").toString();
            metadata.parentId = query.value("parent_id").toString();
            metadata.isFolder = query.value("is_folder").toInt() == 1;
            metadata.size = query.value("size").toLongLong();
            metadata.mimeType = query.value("mime_type").toString();
            metadata.createdTime = QDateTime::fromString(query.value("created_time").toString(), Qt::ISODate);
            metadata.modifiedTime = QDateTime::fromString(query.value("modified_time").toString(), Qt::ISODate);
            metadata.cachedAt = QDateTime::fromString(query.value("cached_at").toString(), Qt::ISODate);
            metadata.lastAccessed = QDateTime::fromString(query.value("last_accessed").toString(), Qt::ISODate);
            children.append(metadata);
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
            FuseMetadata metadata;
            metadata.fileId = query.value("file_id").toString();
            metadata.path = query.value("path").toString();
            metadata.name = query.value("name").toString();
            metadata.parentId = query.value("parent_id").toString();
            metadata.isFolder = query.value("is_folder").toInt() == 1;
            metadata.size = query.value("size").toLongLong();
            metadata.mimeType = query.value("mime_type").toString();
            metadata.createdTime = QDateTime::fromString(query.value("created_time").toString(), Qt::ISODate);
            metadata.modifiedTime = QDateTime::fromString(query.value("modified_time").toString(), Qt::ISODate);
            metadata.cachedAt = QDateTime::fromString(query.value("cached_at").toString(), Qt::ISODate);
            metadata.lastAccessed = QDateTime::fromString(query.value("last_accessed").toString(), Qt::ISODate);
            result.append(metadata);
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
            updated += updateFuseChildrenPaths(updatedChild.fileId, oldParentPath + "/" + child.name,
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
            entry.markedDirtyAt = QDateTime::fromString(query.value("marked_dirty_at").toString(), Qt::ISODate);
            entry.lastUploadAttempt = QDateTime::fromString(query.value("last_upload_attempt").toString(), Qt::ISODate);
            entry.uploadFailed = query.value("upload_failed").toInt() != 0;
            dirtyFiles.append(entry);
        }
    }

    return dirtyFiles;
}

bool SyncDatabase::markFuseDirty(const QString& fileId, const QString& path) {
    QMutexLocker locker(&m_mutex);
    QSqlQuery query(m_db);

    // Use INSERT OR IGNORE to avoid overwriting existing entries
    // This preserves failure tracking information if file was already dirty
    query.prepare(R"(
        INSERT OR IGNORE INTO fuse_dirty_files 
        (file_id, path, marked_dirty_at, upload_failed)
        VALUES (?, ?, ?, 0)
    )");

    query.addBindValue(fileId);
    query.addBindValue(path);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));

    if (!query.exec()) {
        logError("markFuseDirty", query.lastError().text());
        return false;
    }

    // If insert was ignored (file already dirty), update the marked_dirty_at timestamp
    // but preserve the upload_failed and last_upload_attempt values
    if (query.numRowsAffected() == 0) {
        query.prepare(R"(
            UPDATE fuse_dirty_files 
            SET path = ?, marked_dirty_at = ?
            WHERE file_id = ?
        )");
        query.addBindValue(path);
        query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
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
            entry.lastAccessed = QDateTime::fromString(query.value("last_accessed").toString(), Qt::ISODate);
            entry.downloadCompleted = QDateTime::fromString(query.value("download_completed").toString(), Qt::ISODate);
            entries.append(entry);
        }
    }

    return entries;
}

bool SyncDatabase::recordFuseCacheEntry(const QString& fileId, const QString& cachePath, qint64 size) {
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
    static const char* tables[] = {"conflict_versions", "conflicts",          "deleted_files", "files",
                                   "fuse_dirty_files",  "fuse_cache_entries", "fuse_metadata", "fuse_sync_state",
                                   "settings"};

    QSqlQuery query(m_db);
    for (const char* table : tables) {
        const QString sql = QStringLiteral("DELETE FROM %1").arg(QLatin1String(table));
        if (!query.exec(sql)) {
            logError("clearAllData",
                     QStringLiteral("Failed to clear %1: %2").arg(QLatin1String(table), query.lastError().text()));
            return false;
        }
    }

    qInfo() << "SyncDatabase: all user data cleared (account sign-out)";
    return true;
}
