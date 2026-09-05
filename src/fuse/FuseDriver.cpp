/**
 * @file FuseDriver.cpp
 * @brief Implementation of FUSE virtual filesystem driver
 *
 * Implements the FUSE callbacks and coordinates between components as defined
 * in the FUSE Procedure Flow Chart (see src/sync/dataflow.md).
 */

#include "FuseDriver.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <functional>

#include "DirtySyncWorker.h"
#include "FileCache.h"
#include "FuseReplayWorker.h"
#include "MetadataCache.h"
#include "MetadataRefreshWorker.h"
#include "api/GoogleDriveClient.h"
#include "sync/RuntimePauseController.h"
#include "sync/SyncDatabase.h"
#include "sync/TrashPolicy.h"
#include "utils/NativeDocShortcutHandler.h"
#include "utils/NativeDocSupport.h"

namespace {

/// Recover the FuseDriver instance from FUSE's per-mount private_data.
static inline FuseDriver* self() {
    auto* ctx = fuse_get_context();
    Q_ASSERT_X(ctx && ctx->private_data, "FuseDriver::self",
               "self() requires a valid FUSE callback context");
    if (!ctx || !ctx->private_data) {
        return nullptr;
    }

    return static_cast<FuseDriver*>(ctx->private_data);
}

constexpr int FUSE_API_TIMEOUT_MS = 30000;

QString logicalPathFromFusePath(const QString& fusePath) {
    QString normalized = QDir::cleanPath(fusePath);
    if (normalized == QStringLiteral(".")) {
        normalized.clear();
    }
    if (normalized == QStringLiteral("/")) {
        return QString();
    }
    if (normalized.startsWith(QLatin1Char('/'))) {
        normalized.remove(0, 1);
    }
    return normalized;
}

QString joinFusePath(const QString& parentPath, const QString& childName) {
    if (parentPath.isEmpty() || parentPath == QStringLiteral("/")) {
        return QStringLiteral("/") + childName;
    }
    return QDir(parentPath).filePath(childName);
}

bool relativePathWithinSubtree(const QString& candidatePath, const QString& rootPath) {
    const QString normalizedCandidate = QDir::cleanPath(candidatePath);
    const QString normalizedRoot = QDir::cleanPath(rootPath);
    if (normalizedCandidate.isEmpty() || normalizedRoot.isEmpty()) {
        return false;
    }
    if (normalizedCandidate == normalizedRoot) {
        return true;
    }
    return normalizedCandidate.startsWith(normalizedRoot + QLatin1Char('/'));
}

bool fillStatFromPath(const QString& absolutePath, struct stat* stbuf) {
    const QByteArray encodedPath = QFile::encodeName(absolutePath);
    return ::lstat(encodedPath.constData(), stbuf) == 0;
}

QString nativeDocModeOverrideForFile(const SyncDatabase* database, const QString& fileId) {
    if (!database || fileId.isEmpty()) {
        return QString();
    }

    return database->getNativeDocState(fileId).nativeDocModeOverride;
}

bool copyFileToPath(const QString& sourcePath, const QString& targetPath, QString* errorOut) {
    if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Local content missing for trash snapshot");
        }
        return false;
    }

    QDir().mkpath(QFileInfo(targetPath).dir().absolutePath());
    QFile::remove(targetPath);
    if (!QFile::copy(sourcePath, targetPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to copy local content into trash overlay");
        }
        return false;
    }

    QFile(targetPath).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool writeBytesToPath(const QByteArray& bytes, const QString& targetPath, QString* errorOut) {
    QDir().mkpath(QFileInfo(targetPath).dir().absolutePath());

    QFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to create trash overlay file");
        }
        return false;
    }

    if (file.write(bytes) != bytes.size()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write trash overlay file");
        }
        return false;
    }

    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool invokeDriveCall(GoogleDriveClient* driveClient, const std::function<void()>& call) {
    if (!driveClient) {
        return false;
    }

    return QMetaObject::invokeMethod(driveClient, [call]() { call(); }, Qt::QueuedConnection);
}

bool isAuthOrPermissionFailure(int httpStatus, const QString& errorMsg) {
    if (httpStatus == 401 || httpStatus == 403) {
        return true;
    }

    const QString lowered = errorMsg.toLower();
    return lowered.contains(QStringLiteral("authentication")) ||
           lowered.contains(QStringLiteral("auth")) ||
           lowered.contains(QStringLiteral("permission")) ||
           lowered.contains(QStringLiteral("credential")) ||
           lowered.contains(QStringLiteral("unauthorized"));
}

FuseFileMetadata toCacheMetadata(const FuseMetadata& meta) {
    FuseFileMetadata metadata;
    metadata.fileId = meta.fileId;
    metadata.path = meta.path;
    metadata.name = meta.name;
    metadata.remoteName = meta.remoteName;
    metadata.nativeDocModeOverride = meta.nativeDocModeOverride;
    metadata.parentId = meta.parentId;
    metadata.isFolder = meta.isFolder;
    metadata.size = meta.size;
    metadata.mimeType = meta.mimeType;
    metadata.remoteMimeType = meta.remoteMimeType;
    metadata.webViewLink = meta.webViewLink;
    metadata.createdTime = meta.createdTime;
    metadata.modifiedTime = meta.modifiedTime;
    metadata.cachedAt = meta.cachedAt;
    metadata.lastAccessed = meta.lastAccessed;
    return metadata;
}

QString remoteRenameTargetForMetadata(const FuseMetadata& meta, const QString& requestedName) {
    const QString remoteMimeType =
        meta.remoteMimeType.isEmpty() ? meta.mimeType : meta.remoteMimeType;
    if (!isNativeDocMimeType(remoteMimeType)) {
        return requestedName;
    }

    const QString currentModeSetting =
        QSettings().value("advanced/nativeDocMode", "hide").toString();
    const NativeDocRepresentation representation = effectiveNativeDocRepresentation(
        remoteMimeType, meta.nativeDocModeOverride, nativeDocModeFromString(currentModeSetting));
    return nativeDocRemoteNameFromVisibleName(requestedName, representation);
}

bool waitForFolderCreate(GoogleDriveClient* driveClient, const QString& requestLocalPath,
                         const std::function<bool()>& startRequest, DriveFile* createdFolder,
                         QString* errorOut, bool* authFailureOut = nullptr) {
    if (!driveClient) {
        if (errorOut) {
            *errorOut = QStringLiteral("GoogleDriveClient unavailable");
        }
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool success = false;
    QString error;
    int errorStatus = 0;
    DriveFile result;

    QMetaObject::Connection createdConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    createdConn = QObject::connect(driveClient, &GoogleDriveClient::folderCreatedDetailed, &loop,
                                   [&](const DriveFile& folder, const QString& localPath) {
                                       if (localPath != requestLocalPath) {
                                           return;
                                       }
                                       success = folder.isValid();
                                       result = folder;
                                       loop.quit();
                                   });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg,
                                     int httpStatus, const QString&, const QString& localPath) {
                                     if (operation != QStringLiteral("createFolder")) {
                                         return;
                                     }
                                     if (localPath != requestLocalPath) {
                                         return;
                                     }
                                     error = errorMsg;
                                     errorStatus = httpStatus;
                                     loop.quit();
                                 });

    timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("createFolder timeout");
        loop.quit();
    });

    if (!startRequest || !startRequest()) {
        error = QStringLiteral("Failed to dispatch createFolder request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(createdConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("createFolder failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    if (success && createdFolder) {
        *createdFolder = result;
    }

    return success;
}

bool waitForUpload(GoogleDriveClient* driveClient, const QString& localPath,
                   DriveFile* uploadedFile, const std::function<bool()>& startRequest,
                   QString* errorOut, bool* authFailureOut = nullptr) {
    if (!driveClient) {
        if (errorOut) {
            *errorOut = QStringLiteral("GoogleDriveClient unavailable");
        }
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool success = false;
    QString error;
    int errorStatus = 0;
    DriveFile result;

    QMetaObject::Connection uploadedConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    uploadedConn = QObject::connect(driveClient, &GoogleDriveClient::fileUploadedDetailed, &loop,
                                    [&](const DriveFile& file, const QString& uploadedPath) {
                                        if (uploadedPath != localPath) {
                                            return;
                                        }
                                        success = file.isValid();
                                        result = file;
                                        loop.quit();
                                    });

    errorConn =
        QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                         [&](const QString& operation, const QString& errorMsg, int httpStatus,
                             const QString&, const QString& errorLocalPath) {
                             if (operation != QStringLiteral("uploadFile")) {
                                 return;
                             }
                             if (errorLocalPath != localPath) {
                                 return;
                             }
                             error = errorMsg;
                             errorStatus = httpStatus;
                             loop.quit();
                         });

    timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("uploadFile timeout");
        loop.quit();
    });

    if (!startRequest || !startRequest()) {
        error = QStringLiteral("Failed to dispatch uploadFile request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(uploadedConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("uploadFile failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    if (success && uploadedFile) {
        *uploadedFile = result;
    }

    return success;
}

bool waitForUpdate(GoogleDriveClient* driveClient, const QString& fileId, DriveFile* updatedFile,
                   const std::function<bool()>& startRequest, QString* errorOut,
                   bool* authFailureOut = nullptr) {
    if (!driveClient) {
        if (errorOut) {
            *errorOut = QStringLiteral("GoogleDriveClient unavailable");
        }
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool success = false;
    QString error;
    int errorStatus = 0;
    DriveFile result;

    QMetaObject::Connection updatedConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    updatedConn = QObject::connect(driveClient, &GoogleDriveClient::fileUpdated, &loop,
                                   [&](const DriveFile& file) {
                                       if (file.id != fileId) {
                                           return;
                                       }
                                       success = file.isValid();
                                       result = file;
                                       loop.quit();
                                   });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg,
                                     int httpStatus, const QString& errorFileId, const QString&) {
                                     if (operation != QStringLiteral("updateFile")) {
                                         return;
                                     }
                                     if (!errorFileId.isEmpty() && errorFileId != fileId) {
                                         return;
                                     }
                                     error = errorMsg;
                                     errorStatus = httpStatus;
                                     loop.quit();
                                 });

    timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("updateFile timeout");
        loop.quit();
    });

    if (!startRequest || !startRequest()) {
        error = QStringLiteral("Failed to dispatch updateFile request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(updatedConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("updateFile failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    if (success && updatedFile) {
        *updatedFile = result;
    }

    return success;
}

bool waitForTrash(GoogleDriveClient* driveClient, const QString& fileId,
                  const std::function<bool()>& startRequest, QString* errorOut,
                  bool* authFailureOut = nullptr) {
    if (!driveClient) {
        if (errorOut) {
            *errorOut = QStringLiteral("GoogleDriveClient unavailable");
        }
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool success = false;
    QString error;
    int errorStatus = 0;

    QMetaObject::Connection trashedConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    trashedConn = QObject::connect(driveClient, &GoogleDriveClient::fileTrashed, &loop,
                                   [&](const QString& trashedId) {
                                       if (trashedId != fileId) {
                                           return;
                                       }
                                       success = true;
                                       loop.quit();
                                   });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg,
                                     int httpStatus, const QString& errorFileId, const QString&) {
                                     if (operation != QStringLiteral("trashFile")) {
                                         return;
                                     }
                                     if (!errorFileId.isEmpty() && errorFileId != fileId) {
                                         return;
                                     }
                                     error = errorMsg;
                                     errorStatus = httpStatus;
                                     loop.quit();
                                 });

    timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("trashFile timeout");
        loop.quit();
    });

    timeout.start(FUSE_API_TIMEOUT_MS);

    if (!startRequest()) {
        QObject::disconnect(trashedConn);
        QObject::disconnect(errorConn);
        QObject::disconnect(timeoutConn);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to start trashFile request");
        }
        return false;
    }

    if (!success && error.isEmpty()) {
        loop.exec();
    }

    QObject::disconnect(trashedConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("trashFile failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    return success;
}

bool waitForListFiles(GoogleDriveClient* driveClient, const QString& parentId,
                      QList<DriveFile>* resultFiles, QString* errorOut) {
    if (!driveClient) {
        if (errorOut) {
            *errorOut = QStringLiteral("GoogleDriveClient unavailable");
        }
        return false;
    }

    if (!resultFiles) {
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool success = false;
    QString error;
    QList<DriveFile> allFiles;

    QMetaObject::Connection listedConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    listedConn = QObject::connect(
        driveClient, &GoogleDriveClient::filesListed, &loop,
        [&](const QList<DriveFile>& files, const QString& nextPageToken) {
            allFiles.append(files);
            if (nextPageToken.isEmpty()) {
                // Final page — we're done
                success = true;
                loop.quit();
            } else {
                // More pages — dispatch next request and reset timeout
                timeout.start(FUSE_API_TIMEOUT_MS);
                invokeDriveCall(driveClient, [driveClient, parentId, nextPageToken]() {
                    driveClient->listFiles(parentId, nextPageToken);
                });
            }
        });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg, int,
                                     const QString&, const QString&) {
                                     if (!operation.startsWith(QStringLiteral("listFiles"))) {
                                         return;
                                     }
                                     error = errorMsg;
                                     loop.quit();
                                 });

    timeoutConn = QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("listFiles timeout");
        loop.quit();
    });

    if (!invokeDriveCall(driveClient,
                         [driveClient, parentId]() { driveClient->listFiles(parentId); })) {
        error = QStringLiteral("Failed to dispatch listFiles request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(listedConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (success) {
        *resultFiles = allFiles;
    } else if (errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("listFiles failed") : error;
    }

    return success;
}

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

FuseDriver::FuseDriver(GoogleDriveClient* driveClient, SyncDatabase* database, QObject* parent)
    : QObject(parent),
      m_driveClient(driveClient),
      m_database(database),
      m_metadataCache(nullptr),
      m_fileCache(nullptr),
      m_maxCacheSizeBytes(0),
      m_mounted(false),
      m_fuseThread(nullptr),
      m_fuse(nullptr),
      m_session(nullptr),
      m_dirtySyncThread(nullptr),
      m_metadataRefreshThread(nullptr),
      m_replayThread(nullptr),
      m_dirtySyncWorker(nullptr),
      m_metadataRefreshWorker(nullptr),
      m_replayWorker(nullptr),
      m_pauseController(nullptr),
      m_backgroundSyncPaused(false),
      m_syncSettings(SyncSettings::load()),
      m_nextFileHandle(1) {
    // Set default mount point
    m_mountPoint = QDir::homePath() + "/GoogleDriveFuse";

    qDebug() << "FuseDriver: Initialized with mount point:" << m_mountPoint;
}

FuseDriver::~FuseDriver() {
    if (m_mounted) {
        unmount();
    }

    // Clean up file cache
    delete m_fileCache;
    m_fileCache = nullptr;

    delete m_metadataCache;
    m_metadataCache = nullptr;
}

// ============================================================================
// Static Utility Methods
// ============================================================================

bool FuseDriver::isFuseAvailable() {
    // Check if FUSE is available by checking for /dev/fuse
    return QFile::exists("/dev/fuse");
}

// ============================================================================
// Configuration
// ============================================================================

QString FuseDriver::mountPoint() const {
    return m_mountPoint;
}

void FuseDriver::setMountPoint(const QString& path) {
    if (m_mounted) {
        qWarning() << "FuseDriver: Cannot change mount point while mounted";
        return;
    }
    m_mountPoint = path;
    qDebug() << "FuseDriver: Mount point set to:" << path;
}

void FuseDriver::setCacheDirectory(const QString& path) {
    if (m_mounted) {
        qWarning() << "FuseDriver: Cannot change cache directory while mounted";
        return;
    }

    m_cacheDirectory = path;
}

void FuseDriver::setMaxCacheSizeBytes(qint64 bytes) {
    if (m_mounted) {
        qWarning() << "FuseDriver: Cannot change cache size while mounted";
        return;
    }

    m_maxCacheSizeBytes = qMax<qint64>(bytes, 0);
}

bool FuseDriver::isMounted() const {
    return m_mounted;
}

FileCache* FuseDriver::fileCache() const {
    return m_fileCache;
}

SyncDatabase* FuseDriver::database() const {
    return m_database;
}

GoogleDriveClient* FuseDriver::driveClient() const {
    return m_driveClient;
}

void FuseDriver::setPauseController(RuntimePauseController* pauseController) {
    m_pauseController = pauseController;
    if (m_fileCache) {
        m_fileCache->setPauseController(pauseController);
    }
}

bool FuseDriver::isDriveApiAllowed() const {
    return !m_pauseController || m_pauseController->isDriveApiAllowed();
}

// ============================================================================
// Public Slots
// ============================================================================

bool FuseDriver::mount() {
    if (m_mounted) {
        qWarning() << "FuseDriver: Filesystem already mounted";
        return true;
    }

    if (!isFuseAvailable()) {
        emit mountError("FUSE is not available on this system");
        return false;
    }

    qInfo() << "FuseDriver: Starting mount process...";

    // Step 1: Create mount point directory if needed
    QDir dir(m_mountPoint);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qWarning() << "FuseDriver: Failed to create mount point directory:" << m_mountPoint;
            emit mountError("Failed to create mount point directory: " + m_mountPoint);
            return false;
        }
        qDebug() << "FuseDriver: Created mount point directory";
    }

    // Check for existing FUSE mount (stale from crash, or left over).
    // We check /proc/mounts which is authoritative — statvfs() can fail with
    // various errno values (ENOTCONN, EIO, EACCES) or even succeed on stale mounts.
    {
        bool isMounted = false;
        QFile procMounts("/proc/mounts");
        if (procMounts.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray mountPointBytes = m_mountPoint.toUtf8();
            while (!procMounts.atEnd()) {
                QByteArray line = procMounts.readLine();
                // /proc/mounts format: device mountpoint fstype options ...
                QList<QByteArray> fields = line.split(' ');
                if (fields.size() >= 2 && fields[1] == mountPointBytes) {
                    isMounted = true;
                    qDebug() << "FuseDriver: Found existing mount in /proc/mounts:"
                             << line.trimmed();
                    break;
                }
            }
            procMounts.close();
        }

        if (isMounted) {
            qWarning() << "FuseDriver: Detected existing FUSE mount at" << m_mountPoint
                       << "- attempting automatic unmount";
            QProcess fusermount;
            fusermount.start("fusermount3", {"-u", m_mountPoint});
            if (!fusermount.waitForFinished(5000) || fusermount.exitCode() != 0) {
                // Try fusermount (v2) as fallback
                fusermount.start("fusermount", {"-u", m_mountPoint});
                fusermount.waitForFinished(5000);
            }
            if (fusermount.exitCode() == 0) {
                qInfo() << "FuseDriver: Successfully unmounted existing FUSE mount";
            } else {
                // Try lazy unmount as last resort
                qWarning() << "FuseDriver: Normal unmount failed, trying lazy unmount";
                fusermount.start("fusermount3", {"-u", "-z", m_mountPoint});
                fusermount.waitForFinished(5000);
                if (fusermount.exitCode() != 0) {
                    qWarning() << "FuseDriver: Failed to unmount existing FUSE mount:"
                               << fusermount.readAllStandardError().trimmed();
                    emit mountError(
                        "Mount point has an existing FUSE mount that could not be cleaned up: " +
                        m_mountPoint);
                    return false;
                }
                qInfo() << "FuseDriver: Lazy unmount succeeded";
            }
        }
    }

    // Check if mount point is empty (after possible unmount above)
    if (!dir.isEmpty()) {
        qWarning() << "FuseDriver: Mount point is not empty:" << m_mountPoint
                   << "contents:" << dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        emit mountError("Mount point directory is not empty: " + m_mountPoint);
        return false;
    }

    // Step 2: Initialize MetadataCache from database/API
    if (!initializeMetadataCache()) {
        emit mountError("Failed to initialize metadata cache");
        return false;
    }

    // Step 3: Initialize FileCache manager
    if (!initializeFileCache()) {
        emit mountError("Failed to initialize file cache");
        return false;
    }

    // Step 4: Setup FUSE operations structure
    static struct fuse_operations ops = {};
    ops.getattr = fuseGetattr;
    ops.readdir = fuseReaddir;
    ops.open = fuseOpen;
    ops.read = fuseRead;
    ops.write = fuseWrite;
    ops.release = fuseRelease;
    ops.mkdir = fuseMkdir;
    ops.rmdir = fuseRmdir;
    ops.unlink = fuseUnlink;
    ops.rename = fuseRename;
    ops.truncate = fuseTruncate;
    ops.create = fuseCreate;
    ops.init = fuseInit;
    ops.destroy = fuseDestroy;
    // M5: Add missing FUSE operations as no-op stubs
    ops.statfs = fuseStatfs;
    ops.chmod = fuseChmod;
    ops.chown = fuseChown;
    ops.utimens = fuseUtimens;
    ops.fsync = fuseFsync;

    // Start FUSE in a separate thread
    m_fuseThread = QThread::create([this]() {
        // libfuse3: fuse_new does NOT accept the mount point in argv.
        // The mount point is passed separately to fuse_mount().
        // Foreground behaviour is implicit when using fuse_loop() directly.
        const char* argv[] = {
            "via",
        };
        int argc = 1;
        struct fuse_args args = FUSE_ARGS_INIT(argc, const_cast<char**>(argv));

        qDebug() << "FuseDriver: Creating FUSE instance";
        qDebug() << "FuseDriver:   mount point =" << m_mountPoint;
        qDebug() << "FuseDriver:   uid=" << getuid() << "euid=" << geteuid();

        // Detect snap/sandbox confinement that blocks mount()
        QFile appArmorAttr("/proc/self/attr/current");
        if (appArmorAttr.open(QIODevice::ReadOnly)) {
            QString profile = QString::fromUtf8(appArmorAttr.readAll()).trimmed();
            appArmorAttr.close();
            qDebug() << "FuseDriver:   AppArmor profile:" << profile;
            if (profile.contains("snap.")) {
                qWarning()
                    << "FuseDriver: Running inside Snap confinement (" << profile
                    << "). FUSE mount may fail with EPERM. "
                       "Launch the application from a native terminal (not a Snap-confined one).";
            }
        }

        errno = 0;
        m_fuse = fuse_new(&args, &ops, sizeof(ops), this);
        int savedErrno = errno;
        fuse_opt_free_args(&args);
        if (!m_fuse) {
            qCritical() << "FuseDriver: Failed to create FUSE instance"
                        << "- errno=" << savedErrno << "(" << strerror(savedErrno) << ")";
            m_mountReadySemaphore.release();
            return;
        }
        qDebug() << "FuseDriver: fuse_new() succeeded";

        errno = 0;
        int mountResult = fuse_mount(m_fuse, m_mountPoint.toUtf8().constData());
        savedErrno = errno;
        if (mountResult != 0) {
            qCritical() << "FuseDriver: fuse_mount() failed"
                        << "- errno=" << savedErrno << "(" << strerror(savedErrno) << ")"
                        << "mountPoint=" << m_mountPoint;
            if (savedErrno == EPERM) {
                qCritical() << "FuseDriver: EPERM - This is typically caused by:"
                            << "\n  1. Running inside a Snap or Flatpak sandbox"
                            << "\n  2. Missing fusermount3 setuid bit"
                            << "\n  3. User namespace restrictions"
                            << "\n  Try running from a native (non-sandboxed) terminal.";
            }
            fuse_destroy(m_fuse);
            m_fuse = nullptr;
            m_mountReadySemaphore.release();
            return;
        }
        qDebug() << "FuseDriver: fuse_mount() succeeded at" << m_mountPoint;

        m_session = fuse_get_session(m_fuse);

        m_mountReadySemaphore.release();

        qInfo() << "FuseDriver: FUSE event loop starting";

        // Run FUSE event loop (blocks until unmount)
        int loopResult = fuse_loop(m_fuse);
        qInfo() << "FuseDriver: FUSE event loop ended, result=" << loopResult;

        // Cleanup is handled by unmount() — do NOT call fuse_unmount/fuse_destroy here
        // because unmount() calls fuse_unmount() to force this loop to exit, then
        // calls fuse_destroy() after the thread has finished.
    });

    if (!m_fuseThread) {
        emit mountError("Failed to create FUSE thread");
        return false;
    }

    m_fuseThread->start();

    // ROB-03: Wait for FUSE mount with proper synchronization instead of sleep+poll
    if (!m_mountReadySemaphore.tryAcquire(1, 5000)) {
        qCritical() << "FuseDriver: Timed out waiting for FUSE mount";
        m_fuseThread->wait(2000);
        delete m_fuseThread;
        m_fuseThread = nullptr;
        emit mountError("FUSE initialization timed out");
        return false;
    }

    if (!m_fuse) {
        m_fuseThread->wait(2000);
        delete m_fuseThread;
        m_fuseThread = nullptr;
        emit mountError("FUSE initialization failed");
        return false;
    }

    m_mounted = true;

    // Step 5: Start background workers
    startBackgroundWorkers();

    qInfo() << "FuseDriver: Filesystem mounted at:" << m_mountPoint;

    // Step 6: Emit mounted signal
    emit mounted();
    return true;
}

void FuseDriver::unmount() {
    if (!m_mounted) {
        qWarning() << "FuseDriver: Filesystem not mounted";
        return;
    }

    qInfo() << "FuseDriver: Starting unmount process...";

    // Step 1: Unmount from kernel FIRST.
    // This kills all pending FUSE operations, causing fuse_loop() to return,
    // and prevents new FUSE callbacks from triggering during shutdown.
    // Background workers may have in-flight API calls that reference the old mount,
    // so we need to detach from the kernel before stopping them.
    if (m_fuse) {
        fuse_exit(m_fuse);
        fuse_unmount(m_fuse);
        qDebug() << "FuseDriver: fuse_unmount() called";
    }

    // Step 2: Wait for FUSE thread (should return immediately now)
    if (m_fuseThread) {
        if (!m_fuseThread->wait(3000)) {
            qWarning() << "FuseDriver: FUSE thread did not exit in time, terminating";
            m_fuseThread->terminate();
            m_fuseThread->wait();
        }
        delete m_fuseThread;
        m_fuseThread = nullptr;
    }

    // Step 3: Destroy FUSE instance (must be after thread has exited)
    if (m_fuse) {
        fuse_destroy(m_fuse);
        m_fuse = nullptr;
        m_session = nullptr;
        qDebug() << "FuseDriver: fuse_destroy() called";
    }

    // Step 4: Flush dirty files and stop background workers.
    // Now that FUSE is detached, no new dirty files can appear.
    flushDirtyFiles();
    stopBackgroundWorkers();

    // Step 5: Fallback — use fusermount3 if the mount is still present.
    // Check /proc/mounts for authoritative status.
    {
        QFile procMounts("/proc/mounts");
        if (procMounts.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray mountPointBytes = m_mountPoint.toUtf8();
            bool stillMounted = false;
            while (!procMounts.atEnd()) {
                QList<QByteArray> fields = procMounts.readLine().split(' ');
                if (fields.size() >= 2 && fields[1] == mountPointBytes) {
                    stillMounted = true;
                    break;
                }
            }
            procMounts.close();
            if (stillMounted) {
                qWarning() << "FuseDriver: Mount point still present in /proc/mounts after "
                              "fuse_unmount, using fusermount3 -u -z";
                QProcess::execute("fusermount3", {"-u", "-z", m_mountPoint});
            }
        }
    }

    // Step 6: Clear open files
    {
        QMutexLocker locker(&m_openFilesMutex);
        m_openFiles.clear();
    }

    // Step 7: Purge in-memory caches so no stale data leaks across accounts
    if (m_metadataCache) {
        m_metadataCache->clearAll();
    }
    if (m_fileCache) {
        m_fileCache->clearCache();
    }

    m_mounted = false;

    qInfo() << "FuseDriver: Filesystem unmounted";

    // Step 8: Emit unmounted signal
    emit unmounted();
}

void FuseDriver::refreshMetadata() {
    if (!m_mounted) {
        qWarning() << "FuseDriver: Cannot refresh metadata - not mounted";
        return;
    }

    if (!isDriveApiAllowed()) {
        qInfo() << "FuseDriver: Skipping metadata refresh while Drive access is paused";
        return;
    }

    qDebug() << "FuseDriver: Refreshing metadata from remote";
    emit metadataRefreshStarted();

    if (m_metadataRefreshWorker) {
        QMetaObject::invokeMethod(m_metadataRefreshWorker, "checkNow", Qt::QueuedConnection);
    }
}

void FuseDriver::reloadSyncSettings() {
    {
        QMutexLocker locker(&m_syncSettingsMutex);
        m_syncSettings = SyncSettings::load();
    }

    if (m_dirtySyncWorker) {
        QMetaObject::invokeMethod(m_dirtySyncWorker, "reloadSettings", Qt::QueuedConnection);
    }
    if (m_replayWorker) {
        QMetaObject::invokeMethod(m_replayWorker, "reloadSettings", Qt::QueuedConnection);
    }

    qInfo() << "FuseDriver: Reloaded sync settings, mode=" << currentSyncSettings().syncMode;
}

void FuseDriver::pauseSync() {
    m_backgroundSyncPaused = true;

    if (m_dirtySyncWorker) {
        QMetaObject::invokeMethod(m_dirtySyncWorker, "pause", Qt::QueuedConnection);
    }
    if (m_metadataRefreshWorker) {
        QMetaObject::invokeMethod(m_metadataRefreshWorker, "pause", Qt::QueuedConnection);
    }
    if (m_replayWorker) {
        QMetaObject::invokeMethod(m_replayWorker, "pause", Qt::QueuedConnection);
    }

    stageDirtyFilesForPause();
}

void FuseDriver::resumeSync() {
    m_backgroundSyncPaused = false;

    if (m_dirtySyncWorker) {
        QMetaObject::invokeMethod(m_dirtySyncWorker, "resume", Qt::QueuedConnection);
    }
    if (m_metadataRefreshWorker) {
        QMetaObject::invokeMethod(m_metadataRefreshWorker, "resume", Qt::QueuedConnection);
    }
    if (m_replayWorker) {
        QMetaObject::invokeMethod(m_replayWorker, "resume", Qt::QueuedConnection);
    }
}

void FuseDriver::flushDirtyFiles() {
    if (!m_fileCache) {
        return;
    }

    qDebug() << "FuseDriver: Flushing dirty files";

    QList<DirtyFileEntry> dirtyFiles = m_fileCache->getDirtyFiles();

    if (dirtyFiles.isEmpty()) {
        qDebug() << "FuseDriver: No dirty files to flush";
        emit dirtyFilesFlushed(0);
        return;
    }

    qInfo() << "FuseDriver: Flushing" << dirtyFiles.size() << "dirty files";

    if (!currentSyncSettings().allowsRemoteMutation(RemoteMutationType::Upload)) {
        qInfo() << "FuseDriver: Leaving" << dirtyFiles.size()
                << "dirty files pending because sync mode blocks uploads";
        emit dirtyFilesFlushed(0);
        return;
    }

    int uploadedCount = 0;
    for (const DirtyFileEntry& entry : dirtyFiles) {
        const UploadSnapshotResult snapshot =
            m_fileCache->createUploadSnapshot(entry.fileId, entry.generation);
        if (snapshot.status == UploadSnapshotStatus::AlreadyUploaded) {
            if (m_fileCache->finalizeUploadedGeneration(entry.fileId)) {
                uploadedCount++;
            }
            continue;
        }

        if (snapshot.status == UploadSnapshotStatus::BlockedByWriter) {
            qInfo() << "FuseDriver: Preserving dirty state for" << entry.path
                    << "- writable FUSE handle still open during flush";
            continue;
        }

        if (snapshot.status == UploadSnapshotStatus::StaleGeneration) {
            continue;
        }

        if (snapshot.status != UploadSnapshotStatus::Ready || !m_driveClient ||
            !QFile::exists(snapshot.snapshotPath)) {
            m_fileCache->markUploadFailed(entry.fileId);
            qWarning() << "FuseDriver: Unmount flush could not prepare snapshot for" << entry.path;
            continue;
        }

        QString error;
        DriveFile updated;
        bool authFailure = false;
        if (waitForUpdate(
                m_driveClient, entry.fileId, &updated,
                [&]() {
                    return invokeDriveCall(m_driveClient, [&]() {
                        m_driveClient->updateFile(entry.fileId, snapshot.snapshotPath);
                    });
                },
                &error, &authFailure)) {
            m_fileCache->markUploadedGeneration(entry.fileId, entry.generation);
            if (m_fileCache->clearDirty(entry.fileId, entry.generation) ||
                m_fileCache->finalizeUploadedGeneration(entry.fileId)) {
                uploadedCount++;
            } else {
                qInfo() << "FuseDriver: Unmount flush preserved dirty state for" << entry.path
                        << "- a newer generation or open writable handle still exists";
            }
        } else {
            m_fileCache->markUploadFailed(entry.fileId);
            qWarning() << "FuseDriver: Unmount flush upload failed for" << entry.path << ":"
                       << error << "authFailure=" << authFailure;
        }

        m_fileCache->cleanupUploadSnapshot(snapshot.snapshotPath);
    }

    qInfo() << "FuseDriver: Flushed" << uploadedCount << "files";
    emit dirtyFilesFlushed(uploadedCount);
}

int FuseDriver::pausedMutationErrorCode() const {
    if (m_pauseController && m_pauseController->hasEffectiveAutoPauseReason(
                                 RuntimePauseController::AutoPauseReason::Offline)) {
        return -ENETDOWN;
    }

    return -EAGAIN;
}

int FuseDriver::enforceSyncModeForRemoteMutation(RemoteMutationType mutation, const QString& action,
                                                 const QString& path) {
    const SyncSettings settings = currentSyncSettings();
    if (settings.allowsRemoteMutation(mutation)) {
        return 0;
    }

    const SyncMode syncMode = settings.syncModeValue();
    emitDriveOperationBlocked(action, path, syncModeBlockedMessage(syncMode, action));
    return syncModeBlockedErrorCode(syncMode);
}

SyncSettings FuseDriver::currentSyncSettings() const {
    QMutexLocker locker(&m_syncSettingsMutex);
    return m_syncSettings;
}

int FuseDriver::syncModeBlockedErrorCode(SyncMode syncMode) {
    switch (syncMode) {
        case SyncMode::RemoteReadOnly:
            return -EROFS;
        case SyncMode::RemoteNoDelete:
            return -EPERM;
        case SyncMode::KeepNewest:
            break;
    }

    return -EACCES;
}

QString FuseDriver::syncModeBlockedMessage(SyncMode syncMode, const QString& action) {
    switch (syncMode) {
        case SyncMode::RemoteReadOnly:
            return QStringLiteral(
                       "Sync mode is Remote Read-Only, so Via blocks %1 from the "
                       "FUSE mount.")
                .arg(action);
        case SyncMode::RemoteNoDelete:
            return QStringLiteral(
                       "Sync mode is Remote No Delete, so Via blocks %1 from the "
                       "FUSE mount.")
                .arg(action);
        case SyncMode::KeepNewest:
            break;
    }

    return QStringLiteral("The current sync mode blocks this Drive mutation.");
}

void FuseDriver::emitDriveOperationBlocked(const QString& action, const QString& path,
                                           const QString& message) {
    const QString resolvedMessage =
        message.isEmpty()
            ? (m_pauseController ? m_pauseController->blockedOperationMessage(action)
                                 : QStringLiteral("Drive access is currently unavailable."))
            : message;

    QMetaObject::invokeMethod(
        this,
        [this, action, path, resolvedMessage]() {
            emit driveOperationBlocked(action, path, resolvedMessage);
        },
        Qt::QueuedConnection);
}

// ============================================================================
// Native-doc stub helpers
// ============================================================================

/**
 * @brief Check if a FUSE metadata entry represents a Google-native document
 */
static bool isNativeDoc(const FuseMetadata& meta) {
    return !meta.isFolder && isNativeDocMimeType(meta.remoteMimeType);
}

static bool isNativeDoc(const FuseFileMetadata& meta) {
    return !meta.isFolder && isNativeDocMimeType(meta.remoteMimeType);
}

static enum fuse_fill_dir_flags readdirFlagsForChild(const FuseFileMetadata& child,
                                                     NativeDocMode globalMode) {
    if (isNativeDoc(child)) {
        NativeDocRepresentation repr = effectiveNativeDocRepresentation(
            child.remoteMimeType, child.nativeDocModeOverride, globalMode);
        if (repr.visible && !repr.synthetic) {
            return static_cast<enum fuse_fill_dir_flags>(0);
        }
    }

    return FUSE_FILL_DIR_PLUS;
}

static void queueNativeDocPrefetch(FileCache* fileCache, const FuseFileMetadata& child,
                                   NativeDocMode globalMode) {
    if (!fileCache || !isNativeDoc(child)) {
        return;
    }

    const NativeDocRepresentation repr = effectiveNativeDocRepresentation(
        child.remoteMimeType, child.nativeDocModeOverride, globalMode);
    if (!repr.visible || repr.synthetic || repr.outputMimeType.isEmpty()) {
        return;
    }

    fileCache->queueExportedPath(child.fileId, repr.outputMimeType);
}

static bool isLocalNativeDocExportFailure(const QString& error) {
    return error == QStringLiteral("Export produced an empty file") ||
           error == QStringLiteral("Export timed out after 30 seconds") ||
           error == QStringLiteral("Export completed but no file was written") ||
           error == QStringLiteral("No Google Drive client available for export");
}

static NativeDocMode globalNativeDocMode() {
    QSettings settings;
    return nativeDocModeFromString(settings.value("advanced/nativeDocMode", "hide").toString());
}

static NativeDocMode effectiveNativeDocModeFor(const FuseMetadata& meta, NativeDocMode globalMode) {
    return effectiveNativeDocMode(meta.nativeDocModeOverride, globalMode);
}

static QString nativeDocFusePath(const FuseMetadata& meta) {
    QString path = meta.path;
    if (path.isEmpty()) {
        path = meta.name;
    }
    if (!path.startsWith(QLatin1Char('/'))) {
        path.prepend(QLatin1Char('/'));
    }
    return path;
}

static QString fusePathFromMetadataPath(const QString& metadataPath) {
    QString path = metadataPath;
    if (path.isEmpty()) {
        return QStringLiteral("/");
    }
    if (!path.startsWith(QLatin1Char('/'))) {
        path.prepend(QLatin1Char('/'));
    }
    return path;
}

static void invalidateFusePath(struct fuse* fuseHandle, const QString& path) {
    if (!fuseHandle || path.isEmpty()) {
        return;
    }

    const QByteArray encodedPath = QFile::encodeName(path);
    const int rc = fuse_invalidate_path(fuseHandle, encodedPath.constData());
    if (rc != 0 && rc != -ENOENT) {
        qWarning() << "FuseDriver: Failed to invalidate FUSE path" << path << ":" << rc;
    }
}

int openLocalHandle(const QString& localPath, bool writable) {
    const QByteArray encodedPath = QFile::encodeName(localPath);
    const int flags = writable ? O_RDWR : O_RDONLY;
    return ::open(encodedPath.constData(), flags);
}

qint64 sizeForOpenHandle(int fd) {
    if (fd < 0) {
        return 0;
    }

    struct stat stbuf;
    if (::fstat(fd, &stbuf) != 0) {
        return 0;
    }

    return static_cast<qint64>(stbuf.st_size);
}

qint64 sizeForExistingPath(const QString& localPath) {
    QFileInfo info(localPath);
    return info.exists() ? info.size() : 0;
}

// ============================================================================
// FUSE Callback Implementations
// ============================================================================

int FuseDriver::fuseGetattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    Q_UNUSED(fi)
    auto* drv = self();

    memset(stbuf, 0, sizeof(struct stat));

    QString qpath = normalizePath(path);

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->getattrTrashOverlay(qpath, stbuf);
    }

    // Root directory
    if (qpath.isEmpty() || qpath == "/") {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    // Remove leading slash for database lookup
    QString lookupPath = qpath;
    if (lookupPath.startsWith("/")) {
        lookupPath = lookupPath.mid(1);
    }

    const auto fillNodeBackedStat = [&](const FuseNode& node) -> bool {
        if (node.nodeId.isEmpty()) {
            return false;
        }

        if (node.isFolder) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
            stbuf->st_mtime = node.modifiedTime.toSecsSinceEpoch();
            stbuf->st_atime = node.lastAccessed.isValid() ? node.lastAccessed.toSecsSinceEpoch()
                                                          : stbuf->st_mtime;
            stbuf->st_ctime =
                node.createdTime.isValid() ? node.createdTime.toSecsSinceEpoch() : stbuf->st_mtime;
            return true;
        }

        const FuseNodeContentState state = drv->m_database->getFuseNodeContentState(node.nodeId);
        QFileInfo localInfo(state.localContentPath);
        if (!localInfo.exists()) {
            return false;
        }

        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        stbuf->st_size = localInfo.size();
        stbuf->st_mtime = localInfo.lastModified().toSecsSinceEpoch();
        stbuf->st_atime = stbuf->st_mtime;
        stbuf->st_ctime = stbuf->st_mtime;
        return true;
    };

    FuseNode localNode;
    if (drv && drv->m_database) {
        localNode = drv->m_database->getFuseNodeByPath(qpath);
        if (!localNode.nodeId.isEmpty() && localNode.remoteFileId.isEmpty() &&
            fillNodeBackedStat(localNode)) {
            return 0;
        }
    }

    // Look up in FUSE metadata database
    if (drv && drv->m_database) {
        FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);

        // If not found, try lazy-populating the parent directory from the API
        if (meta.fileId.isEmpty() && drv->m_driveClient) {
            // Extract parent path and entry name
            QString parentPath;
            int lastSlash = lookupPath.lastIndexOf('/');
            if (lastSlash >= 0) {
                parentPath = lookupPath.left(lastSlash);
            }
            // else parentPath stays empty (parent is root)

            // Find parent folder ID
            QString parentId;
            if (parentPath.isEmpty()) {
                parentId = drv->m_metadataCache ? drv->m_metadataCache->rootFolderId()
                                                : QStringLiteral("root");
                if (parentId.isEmpty()) {
                    parentId = QStringLiteral("root");
                }
            } else {
                FuseMetadata parentMeta = drv->m_database->getFuseMetadataByPath(parentPath);
                if (!parentMeta.fileId.isEmpty()) {
                    parentId = parentMeta.fileId;
                }
            }

            // Only fetch if we found the parent
            if (!parentId.isEmpty()) {
                // Check if we already have children for this parent (avoid re-fetching)
                QList<FuseMetadata> siblings = drv->m_database->getFuseChildren(parentId);
                if (siblings.isEmpty()) {
                    qDebug() << "FuseDriver::getattr: Cache miss for" << lookupPath
                             << ", fetching parent children from API...";

                    QList<DriveFile> apiFiles;
                    QString listError;
                    if (!waitForListFiles(drv->m_driveClient, parentId, &apiFiles, &listError)) {
                        qWarning() << "FuseDriver::getattr: API fetch failed:" << listError;
                    }

                    if (drv->m_metadataCache) {
                        if (parentPath.isEmpty() && !apiFiles.isEmpty()) {
                            const QString actualRootId = apiFiles.first().parentId();
                            if (!actualRootId.isEmpty() &&
                                actualRootId != drv->m_metadataCache->rootFolderId()) {
                                drv->m_metadataCache->setRootFolderId(actualRootId);
                                parentId = actualRootId;
                            }
                        }

                        drv->m_metadataCache->replaceRemoteChildren(parentId, apiFiles);
                    }

                    // Retry lookup after populating
                    meta = drv->m_database->getFuseMetadataByPath(lookupPath);
                }
            }
        }

        if (!meta.fileId.isEmpty()) {
            localNode = drv->ensureLocalNodeForMetadata(qpath, meta);
            const FuseNodeContentState localState =
                localNode.nodeId.isEmpty()
                    ? FuseNodeContentState()
                    : drv->m_database->getFuseNodeContentState(localNode.nodeId);
            if (!localNode.nodeId.isEmpty() &&
                localState.localGeneration > localState.remoteAckGeneration &&
                fillNodeBackedStat(localNode)) {
                return 0;
            }

            if (meta.isFolder) {
                stbuf->st_mode = S_IFDIR | 0755;
                stbuf->st_nlink = 2;
            } else if (isNativeDoc(meta)) {
                // Native docs are always read-only in FUSE
                stbuf->st_mode = S_IFREG | 0444;
                stbuf->st_nlink = 1;
                const NativeDocMode mode = effectiveNativeDocModeFor(meta, globalNativeDocMode());
                stbuf->st_size = drv->nativeDocReportedSize(meta, mode);
            } else {
                stbuf->st_mode = S_IFREG | 0644;
                stbuf->st_nlink = 1;

                // For a file with pending local modifications, report the on-disk
                // size and mtime so that read-after-write sees the correct state.
                // Without this, apps that stat() after a write get the stale remote
                // size and may allocate too-small buffers or think the write failed.
                if (drv->m_fileCache && drv->m_fileCache->isDirty(meta.fileId)) {
                    QString localPath = drv->m_fileCache->getContentPath(meta.fileId);
                    QFileInfo localInfo(localPath);
                    if (localInfo.exists()) {
                        stbuf->st_size = localInfo.size();
                        stbuf->st_uid = getuid();
                        stbuf->st_gid = getgid();
                        stbuf->st_mtime = localInfo.lastModified().toSecsSinceEpoch();
                        stbuf->st_atime = stbuf->st_mtime;
                        stbuf->st_ctime = stbuf->st_mtime;
                        return 0;
                    }
                    qWarning() << "FuseDriver::getattr: dirty file missing from disk for"
                               << meta.fileId;
                }

                stbuf->st_size = meta.size;
            }

            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
            stbuf->st_mtime = meta.modifiedTime.toSecsSinceEpoch();
            stbuf->st_atime = meta.lastAccessed.isValid() ? meta.lastAccessed.toSecsSinceEpoch()
                                                          : stbuf->st_mtime;
            stbuf->st_ctime =
                meta.createdTime.isValid() ? meta.createdTime.toSecsSinceEpoch() : stbuf->st_mtime;

            return 0;
        }
    }

    return -ENOENT;
}

qint64 FuseDriver::nativeDocReportedSize(const FuseMetadata& meta, NativeDocMode mode) {
    if (mode == NativeDocMode::BrowserShortcut) {
        return nativeDocShortcutPayload(meta.webViewLink, meta.remoteMimeType).size();
    }

    NativeDocRepresentation repr =
        effectiveNativeDocRepresentation(meta.remoteMimeType, meta.nativeDocModeOverride, mode);
    if (!m_fileCache || !repr.visible || repr.outputMimeType.isEmpty()) {
        return 0;
    }

    const QString localPath = m_fileCache->getContentPath(meta.fileId, repr.outputMimeType);
    const qint64 cachedSize = sizeForExistingPath(localPath);
    if (cachedSize > 0) {
        return cachedSize;
    }

    return 0;
}

int FuseDriver::fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    Q_UNUSED(offset)
    Q_UNUSED(fi)
    Q_UNUSED(flags)
    auto* drv = self();
    const NativeDocMode mode = globalNativeDocMode();

    QString qpath = normalizePath(path);

    // Add . and .. entries
    filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->readdirTrashOverlay(qpath, buf, filler);
    }

    if (!drv || !drv->m_database || !drv->m_driveClient || !drv->m_metadataCache) {
        return 0;
    }

    // ── Cache / SQLite path ──
    // Serve warm directory listings from memory when available. On a cold cache after startup,
    // hydrate the listing from SQLite before falling back to the Drive API.
    QString cacheLookupPath;
    if (qpath.isEmpty() || qpath == "/") {
        cacheLookupPath = QStringLiteral("/");
    } else {
        cacheLookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;
    }

    QList<FuseFileMetadata> cached;
    if (drv->m_metadataCache) {
        cached = drv->m_metadataCache->getOrFetchChildren(cacheLookupPath);
    }

    if (drv->m_metadataCache && drv->m_metadataCache->hasChildrenCached(cacheLookupPath)) {
        qDebug() << "FuseDriver::readdir: Serving" << cached.size() << "entries from cache for"
                 << qpath;
        QSet<QString> emittedNames;
        for (const FuseFileMetadata& child : cached) {
            if (drv->isTrashFusePath(child.path)) {
                continue;
            }
            filler(buf, child.name.toUtf8().constData(), nullptr, 0,
                   readdirFlagsForChild(child, mode));
            emittedNames.insert(child.name);
            queueNativeDocPrefetch(drv->m_fileCache, child, mode);
        }

        drv->appendLocalNodeChildrenToListing(qpath, buf, filler, emittedNames);

        if (qpath == QStringLiteral("/")) {
            QDir overlayDir(drv->trashOverlayRoot());
            if (overlayDir.exists()) {
                const QFileInfoList overlayEntries = overlayDir.entryInfoList(
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDir::Name);
                for (const QFileInfo& overlayEntry : overlayEntries) {
                    if (emittedNames.contains(overlayEntry.fileName())) {
                        continue;
                    }
                    filler(buf, overlayEntry.fileName().toUtf8().constData(), nullptr, 0,
                           overlayEntry.isDir() ? FUSE_FILL_DIR_PLUS
                                                : static_cast<enum fuse_fill_dir_flags>(0));
                }
            }
        }

        return 0;
    }

    const FuseNode localDirNode = drv->m_database->getFuseNodeByPath(qpath);
    if (!localDirNode.nodeId.isEmpty() && localDirNode.isFolder &&
        localDirNode.remoteFileId.isEmpty()) {
        QSet<QString> emittedNames;
        drv->appendLocalNodeChildrenToListing(qpath, buf, filler, emittedNames);
        return 0;
    }

    // ── Fallback: fetch from API ─────────────────────────────────────
    // Get parent folder ID
    QString parentId;
    if (qpath.isEmpty() || qpath == "/") {
        parentId =
            drv->m_metadataCache ? drv->m_metadataCache->rootFolderId() : QStringLiteral("root");
        if (parentId.isEmpty()) {
            parentId = QStringLiteral("root");
        }
    } else {
        QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;
        FuseMetadata parentMeta = drv->m_database->getFuseMetadataByPath(lookupPath);
        if (!parentMeta.fileId.isEmpty()) {
            parentId = parentMeta.fileId;
        } else {
            return -ENOENT;
        }
    }

    qDebug() << "FuseDriver::readdir: Cache miss — fetching children for" << qpath
             << "(parentId=" << parentId << ") from API...";

    QList<DriveFile> apiFiles;
    QString listError;
    if (!waitForListFiles(drv->m_driveClient, parentId, &apiFiles, &listError)) {
        qWarning() << "FuseDriver::readdir: API fetch failed:" << listError;
        return -EIO;
    }

    qDebug() << "FuseDriver::readdir: Fetched" << apiFiles.size() << "files from API";

    // Resolve actual root folder ID from the API response so that the
    // MetadataCache signal handler (onApiChildrenReceived) can map root
    // children correctly and mark the cache as fresh.
    if ((qpath.isEmpty() || qpath == "/") && !apiFiles.isEmpty() && drv->m_metadataCache) {
        QString actualRootId = apiFiles.first().parentId();
        if (!actualRootId.isEmpty() && actualRootId != drv->m_metadataCache->rootFolderId()) {
            qDebug() << "FuseDriver::readdir: Updating root folder ID from"
                     << drv->m_metadataCache->rootFolderId() << "to" << actualRootId;
            drv->m_metadataCache->setRootFolderId(actualRootId);
        }
    }

    const QString effectiveParentId = ((qpath.isEmpty() || qpath == "/") && !apiFiles.isEmpty())
                                          ? apiFiles.first().parentId()
                                          : parentId;
    QList<DriveFile> filteredApiFiles;
    filteredApiFiles.reserve(apiFiles.size());
    for (const DriveFile& file : apiFiles) {
        if (qpath == QStringLiteral("/") && TrashPolicy::isTrashRelativePath(file.name)) {
            continue;
        }
        filteredApiFiles.append(file);
    }

    QList<FuseFileMetadata> children =
        drv->m_metadataCache->replaceRemoteChildren(effectiveParentId, filteredApiFiles);

    QSet<QString> emittedNames;
    for (const FuseFileMetadata& child : children) {
        if (drv->isTrashFusePath(child.path)) {
            continue;
        }
        filler(buf, child.name.toUtf8().constData(), nullptr, 0, readdirFlagsForChild(child, mode));
        emittedNames.insert(child.name);
        queueNativeDocPrefetch(drv->m_fileCache, child, mode);
    }

    drv->appendLocalNodeChildrenToListing(qpath, buf, filler, emittedNames);

    if (qpath == QStringLiteral("/")) {
        QDir overlayDir(drv->trashOverlayRoot());
        if (overlayDir.exists()) {
            const QFileInfoList overlayEntries = overlayDir.entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            for (const QFileInfo& overlayEntry : overlayEntries) {
                if (emittedNames.contains(overlayEntry.fileName())) {
                    continue;
                }
                filler(buf, overlayEntry.fileName().toUtf8().constData(), nullptr, 0,
                       overlayEntry.isDir() ? FUSE_FILL_DIR_PLUS
                                            : static_cast<enum fuse_fill_dir_flags>(0));
            }
        }
    }

    return 0;
}

int FuseDriver::fuseOpen(const char* path, struct fuse_file_info* fi) {
    auto* drv = self();
    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->openTrashOverlay(qpath, fi);
    }

    if (!drv || !drv->m_database || !drv->m_fileCache) {
        return -EIO;
    }

    FuseNode localNode = drv->m_database->getFuseNodeByPath(qpath);

    // Get file metadata
    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty()) {
        if (localNode.nodeId.isEmpty()) {
            return -ENOENT;
        }

        if (localNode.isFolder) {
            return -EISDIR;
        }

        if ((fi->flags & O_WRONLY) || (fi->flags & O_RDWR)) {
            const int syncModeError = drv->enforceSyncModeForRemoteMutation(
                RemoteMutationType::Upload, QStringLiteral("modify files"), qpath);
            if (syncModeError != 0) {
                return syncModeError;
            }
        }

        const QString contentPath = drv->authoritativeContentPathForNode(localNode);
        if (contentPath.isEmpty() || !QFile::exists(contentPath)) {
            return -EIO;
        }

        FuseOpenFile openFile;
        openFile.nodeId = localNode.nodeId;
        openFile.cacheKey = localNode.nodeId;
        openFile.path = qpath;
        openFile.contentPath = contentPath;
        openFile.writable = (fi->flags & O_WRONLY) || (fi->flags & O_RDWR);
        openFile.dirty = false;
        openFile.synthetic = false;
        openFile.localFd = openLocalHandle(contentPath, openFile.writable);
        if (openFile.localFd < 0) {
            return -EIO;
        }
        openFile.size = sizeForOpenHandle(openFile.localFd);

        fi->fh = drv->registerOpenFile(openFile);
        emit drv->fileAccessed(qpath);
        return 0;
    }

    localNode = drv->ensureLocalNodeForMetadata(qpath, meta);

    if (meta.isFolder) {
        return -EISDIR;
    }

    // Native Google docs are always read-only; reject write attempts early.
    if (isNativeDoc(meta)) {
        if ((fi->flags & O_WRONLY) || (fi->flags & O_RDWR)) {
            return -EACCES;
        }

        NativeDocMode mode = effectiveNativeDocModeFor(meta, globalNativeDocMode());

        if (mode == NativeDocMode::BrowserShortcut) {
            // Synthetic stub — bypass FileCache entirely; content is
            // generated on-the-fly in fuseRead from nativeDocShortcutPayload().
            FuseOpenFile openFile;
            openFile.nodeId = localNode.nodeId;
            openFile.fileId = meta.fileId;
            openFile.cacheKey = meta.fileId;
            openFile.path = qpath;
            openFile.size = nativeDocShortcutPayload(meta.webViewLink, meta.remoteMimeType).size();
            openFile.writable = false;
            openFile.dirty = false;
            openFile.synthetic = true;

            fi->fh = drv->registerOpenFile(openFile);
            if (drv) {
                emit drv->fileAccessed(qpath);
            }
            return 0;
        }

        // Export modes (OpenDocument / Text) — use Drive export API
        NativeDocRepresentation repr =
            effectiveNativeDocRepresentation(meta.remoteMimeType, meta.nativeDocModeOverride, mode);
        if (repr.visible && !repr.synthetic && !repr.outputMimeType.isEmpty()) {
            if (!drv->isDriveApiAllowed() &&
                !drv->m_fileCache->hasLocalContent(meta.fileId, repr.outputMimeType)) {
                drv->emitDriveOperationBlocked(QStringLiteral("export native documents"), qpath);
                return -EHOSTDOWN;
            }

            QString cachePath = drv->m_fileCache->getExportedPath(meta.fileId, repr.outputMimeType);
            if (cachePath.isEmpty()) {
                qWarning() << "FuseDriver::open: export failed for" << meta.fileId << "("
                           << meta.name << ") as" << repr.outputMimeType;
                return -EIO;
            }

            // Native-doc exports can be opened immediately after a cached size=0 getattr.
            // Force direct I/O so the first read comes from the backing file descriptor.
            fi->direct_io = 1;

            FuseOpenFile openFile;
            openFile.nodeId = localNode.nodeId;
            openFile.fileId = meta.fileId;
            openFile.cacheKey = meta.fileId + QLatin1Char('|') + repr.outputMimeType;
            openFile.path = qpath;
            openFile.contentPath = cachePath;
            openFile.writable = false;
            openFile.dirty = false;
            openFile.synthetic = false;
            openFile.localFd = openLocalHandle(cachePath, false);
            if (openFile.localFd < 0) {
                return -EIO;
            }
            openFile.size = sizeForOpenHandle(openFile.localFd);

            fi->fh = drv->registerOpenFile(openFile);
            if (drv) {
                emit drv->fileAccessed(qpath);
            }
            return 0;
        }

        // Unsupported native doc type in this mode — should not reach here
        // since MetadataCache filters them, but guard anyway.
        return -ENOENT;
    }

    if ((fi->flags & O_WRONLY) || (fi->flags & O_RDWR)) {
        const int syncModeError = drv->enforceSyncModeForRemoteMutation(
            RemoteMutationType::Upload, QStringLiteral("modify files"), qpath);
        if (syncModeError != 0) {
            return syncModeError;
        }
    }

    QString cachePath = drv->authoritativeContentPathForNode(localNode);
    if (!cachePath.isEmpty() && !QFile::exists(cachePath)) {
        cachePath.clear();
    }

    // Get cached file path (may trigger download)
    if (cachePath.isEmpty() && !drv->isDriveApiAllowed() &&
        !drv->m_fileCache->hasLocalContent(meta.fileId)) {
        drv->emitDriveOperationBlocked(QStringLiteral("download uncached files"), qpath);
        return -EHOSTDOWN;
    }

    if (cachePath.isEmpty()) {
        cachePath = drv->m_fileCache->getCachedPath(meta.fileId, meta.size);
        if (cachePath.isEmpty()) {
            return -EIO;
        }
        if (!localNode.nodeId.isEmpty()) {
            drv->updateNodeContentPath(localNode.nodeId, cachePath);
        }
    }

    // Create open file handle
    FuseOpenFile openFile;
    openFile.nodeId = localNode.nodeId;
    openFile.fileId = meta.fileId;
    openFile.cacheKey = meta.fileId;
    openFile.path = qpath;
    openFile.contentPath = cachePath;
    openFile.writable = (fi->flags & O_WRONLY) || (fi->flags & O_RDWR);
    openFile.dirty = false;
    openFile.synthetic = false;
    openFile.localFd = openLocalHandle(cachePath, openFile.writable);
    if (openFile.localFd < 0) {
        return -EIO;
    }
    openFile.size = sizeForOpenHandle(openFile.localFd);

    fi->fh = drv->registerOpenFile(openFile);

    if (drv) {
        emit drv->fileAccessed(qpath);
    }

    return 0;
}

int FuseDriver::fuseRead(const char* path, char* buf, size_t size, off_t offset,
                         struct fuse_file_info* fi) {
    Q_UNUSED(path)
    auto* drv = self();

    if (!drv) {
        return -EIO;
    }

    auto openFileOpt = drv->getOpenFile(fi->fh);
    if (!openFileOpt) {
        return -EBADF;
    }
    FuseOpenFile openFile = *openFileOpt;

    // Browser-shortcut native docs have synthetic content instead of fd-backed bytes.
    if (openFile.synthetic) {
        // Look up metadata to regenerate stub content
        QString lookupPath = openFile.path.startsWith("/") ? openFile.path.mid(1) : openFile.path;
        FuseMetadata meta;
        if (drv->m_database) {
            meta = drv->m_database->getFuseMetadataByPath(lookupPath);
        }
        if (meta.fileId.isEmpty() || !isNativeDoc(meta)) {
            return -EIO;
        }
        QByteArray stub = nativeDocShortcutPayload(meta.webViewLink, meta.remoteMimeType);
        if (offset >= stub.size()) {
            return 0;
        }
        qint64 available = stub.size() - offset;
        qint64 toRead = qMin(static_cast<qint64>(size), available);
        memcpy(buf, stub.constData() + offset, toRead);
        return static_cast<int>(toRead);
    }

    // Update access time
    if (drv->m_fileCache && (!openFile.cacheKey.isEmpty() || !openFile.fileId.isEmpty())) {
        drv->m_fileCache->updateAccessTime(openFile.cacheKey.isEmpty() ? openFile.fileId
                                                                       : openFile.cacheKey);
    }

    if (openFile.localFd < 0) {
        return -EBADF;
    }

    const ssize_t bytesRead = ::pread(openFile.localFd, buf, size, offset);
    if (bytesRead < 0) {
        return -EIO;
    }

    return static_cast<int>(bytesRead);
}

int FuseDriver::fuseWrite(const char* path, const char* buf, size_t size, off_t offset,
                          struct fuse_file_info* fi) {
    Q_UNUSED(path)
    auto* drv = self();

    if (!drv) {
        return -EIO;
    }

    auto openFileOpt = drv->getOpenFile(fi->fh);
    if (!openFileOpt) {
        return -EBADF;
    }
    FuseOpenFile openFile = *openFileOpt;

    if (!openFile.writable) {
        return -EACCES;
    }

    const int syncModeError = drv->enforceSyncModeForRemoteMutation(
        RemoteMutationType::Upload, QStringLiteral("modify files"), openFile.path);
    if (syncModeError != 0) {
        return syncModeError;
    }

    if (openFile.localFd < 0) {
        return -EIO;
    }

    const ssize_t written = ::pwrite(openFile.localFd, buf, size, offset);
    if (written < 0) {
        return -EIO;
    }

    QString lookupPath = openFile.path.startsWith("/") ? openFile.path.mid(1) : openFile.path;

    if (!openFile.nodeId.isEmpty() &&
        !drv->commitNodeContentMutation(openFile, FuseJournalOperationType::WriteGeneration)) {
        return -EIO;
    }

    // Mark file as dirty (for DirtySyncWorker to upload).  We bump the
    // FileCache generation on every successful write so an older upload can
    // never clear newer bytes written through the same handle.
    if (drv->m_fileCache && !openFile.fileId.isEmpty()) {
        drv->m_fileCache->markDirty(openFile.fileId, lookupPath);
    }

    if (::fsync(openFile.localFd) != 0) {
        return -EIO;
    }

    if (!openFile.dirty) {
        drv->markOpenFileDirty(fi->fh);
        emit drv->fileModified(openFile.path);
    }

    return static_cast<int>(written);
}

int FuseDriver::fuseRelease(const char* path, struct fuse_file_info* fi) {
    Q_UNUSED(path)
    auto* drv = self();

    if (drv) {
        auto openFileOpt = drv->getOpenFile(fi->fh);

        if (openFileOpt && openFileOpt->dirty && !openFileOpt->fileId.isEmpty()) {
            FuseOpenFile openFile = *openFileOpt;
            drv->stageDirtyFileForUpload(openFile.fileId, openFile.path, openFile.localFd,
                                         openFile.nodeId, openFile.contentPath);
        }

        drv->unregisterOpenFile(fi->fh);
    }

    return 0;
}

int FuseDriver::fuseFsync(const char* path, int datasync, struct fuse_file_info* fi) {
    Q_UNUSED(path)
    Q_UNUSED(datasync)
    Q_UNUSED(fi)

    // The file data has already been written to a local cache file by fuseWrite,
    // so it is on stable local storage.  Doing a synchronous Drive upload here
    // blocks the single-threaded FUSE loop for the full network round-trip
    // (up to FUSE_API_TIMEOUT_MS = 30 s), which freezes every other FUSE
    // operation and makes KiCad's UI hang whenever SQLite fsyncs its journal.
    //
    // Instead, the cloud upload is deferred:
    //   fuseRelease() → moveToDirtyStore() → DirtySyncWorker uploads in the
    //   background.
    //
    // This matches the semantics of every major cloud-backed FUSE filesystem
    // (rclone, google-drive-ocamlfuse, etc.): fsync guarantees local durability,
    // not remote durability.
    return 0;
}

int FuseDriver::fuseMkdir(const char* path, mode_t mode) {
    Q_UNUSED(mode)
    auto* drv = self();

    QString qpath = normalizePath(path);
    QString parentPath = getParentPath(qpath);

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->mkdirTrashOverlay(qpath);
    }

    if (!drv || !drv->m_database) {
        return -EIO;
    }

    const int syncModeError = drv->enforceSyncModeForRemoteMutation(
        RemoteMutationType::CreateFolder, QStringLiteral("create folders"), qpath);
    if (syncModeError != 0) {
        return syncModeError;
    }

    if (!drv->createAuthoritativeLocalDirectory(qpath)) {
        return drv->m_database->getFuseNodeByPath(qpath).nodeId.isEmpty() ? -EIO : -EEXIST;
    }

    drv->invalidateFusePaths({parentPath, qpath});

    QMetaObject::invokeMethod(
        drv, [drv, qpath]() { emit drv->fuseFolderCreated(qpath); }, Qt::QueuedConnection);

    return 0;
}

int FuseDriver::fuseRmdir(const char* path) {
    auto* drv = self();
    QString qpath = normalizePath(path);

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->rmdirTrashOverlay(qpath);
    }

    if (!drv || !drv->m_database) {
        return -EIO;
    }

    const int syncModeError = drv->enforceSyncModeForRemoteMutation(
        RemoteMutationType::Trash, QStringLiteral("trash folders"), qpath);
    if (syncModeError != 0) {
        return syncModeError;
    }

    const int result = drv->applyLocalNodeRemoval(qpath, true, true);
    if (result != 0) {
        return result;
    }

    QMetaObject::invokeMethod(
        drv, [drv, qpath]() { emit drv->fuseItemTrashed(qpath); }, Qt::QueuedConnection);

    return 0;
}

int FuseDriver::fuseUnlink(const char* path) {
    auto* drv = self();
    QString qpath = normalizePath(path);

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->unlinkTrashOverlay(qpath);
    }

    if (!drv || !drv->m_database) {
        return -EIO;
    }

    const int syncModeError = drv->enforceSyncModeForRemoteMutation(
        RemoteMutationType::Trash, QStringLiteral("trash files"), qpath);
    if (syncModeError != 0) {
        return syncModeError;
    }

    const int result = drv->applyLocalNodeRemoval(qpath, false, false);
    if (result != 0) {
        return result;
    }

    QMetaObject::invokeMethod(
        drv, [drv, qpath]() { emit drv->fuseItemTrashed(qpath); }, Qt::QueuedConnection);

    return 0;
}

int FuseDriver::fuseRename(const char* from, const char* to, unsigned int flags) {
    Q_UNUSED(flags)
    auto* drv = self();

    QString fromPath = normalizePath(from);
    QString toPath = normalizePath(to);
    QString fromLookup = fromPath.startsWith("/") ? fromPath.mid(1) : fromPath;
    QString toLookup = toPath.startsWith("/") ? toPath.mid(1) : toPath;
    const bool fromIsTrash = drv && drv->isTrashFusePath(fromPath);
    const bool toIsTrash = drv && drv->isTrashFusePath(toPath);

    if (drv && fromIsTrash && toIsTrash) {
        return drv->renameWithinTrashOverlay(fromPath, toPath);
    }

    if (!drv || !drv->m_database) {
        return -EIO;
    }

    if (fromIsTrash && !toIsTrash) {
        const int syncModeError = drv->enforceSyncModeForRemoteMutation(
            RemoteMutationType::CreateFile, QStringLiteral("restore items from local trash"),
            toPath);
        if (syncModeError != 0) {
            return syncModeError;
        }

        if (!drv->m_database->getFuseMetadataByPath(toLookup).fileId.isEmpty() ||
            !drv->m_database->getFuseNodeByPath(toPath).nodeId.isEmpty()) {
            return -EEXIST;
        }

        QString error;
        if (!drv->restoreTrashEntryToLive(fromPath, toPath, &error)) {
            qWarning() << "FuseDriver: restore-from-trash failed" << fromPath << "->" << toPath
                       << ":" << error;
            return error.contains(QStringLiteral("parent"), Qt::CaseInsensitive) ? -ENOENT : -EIO;
        }

        QMetaObject::invokeMethod(
            drv, [drv, fromPath, toPath]() { emit drv->fuseItemMoved(fromPath, toPath); },
            Qt::QueuedConnection);
        return 0;
    }

    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(fromLookup);
    FuseNode localNode = drv->m_database->getFuseNodeByPath(fromPath);
    if (localNode.nodeId.isEmpty() && !meta.fileId.isEmpty()) {
        localNode = drv->ensureLocalNodeForMetadata(fromPath, meta);
    }
    if (meta.fileId.isEmpty() && localNode.nodeId.isEmpty()) {
        return -ENOENT;
    }

    if (toIsTrash) {
        const int syncModeError = drv->enforceSyncModeForRemoteMutation(
            RemoteMutationType::Trash, QStringLiteral("trash items"), fromPath);
        if (syncModeError != 0) {
            return syncModeError;
        }

        const int result =
            drv->applyLocalNodeRemoval(fromPath, localNode.isFolder, localNode.isFolder);
        if (result != 0) {
            return result;
        }

        QMetaObject::invokeMethod(
            drv, [drv, fromPath]() { emit drv->fuseItemTrashed(fromPath); }, Qt::QueuedConnection);
        return 0;
    }

    QString oldName = getFileName(fromPath);
    QString newName = getFileName(toPath);
    QString oldParentPath = getParentPath(fromPath);
    QString newParentPath = getParentPath(toPath);
    const QString remoteNewName = remoteRenameTargetForMetadata(meta, newName);

    bool isRename = (oldName != newName);
    bool isMove = (oldParentPath != newParentPath);

    if (!isRename && !isMove) {
        return 0;
    }

    if (isMove) {
        const int syncModeError = drv->enforceSyncModeForRemoteMutation(
            RemoteMutationType::Move, QStringLiteral("move items"), fromPath);
        if (syncModeError != 0) {
            return syncModeError;
        }
    }
    if (isRename) {
        const int syncModeError = drv->enforceSyncModeForRemoteMutation(
            RemoteMutationType::Rename, QStringLiteral("rename items"), fromPath);
        if (syncModeError != 0) {
            return syncModeError;
        }
    }

    if (!drv->applyLocalNodeRename(fromPath, toPath)) {
        return -EIO;
    }

    // Emit activity signal based on operation type
    if (isMove) {
        QMetaObject::invokeMethod(
            drv, [drv, fromPath, toPath]() { emit drv->fuseItemMoved(fromPath, toPath); },
            Qt::QueuedConnection);
    } else if (isRename) {
        QMetaObject::invokeMethod(
            drv, [drv, fromPath, toPath]() { emit drv->fuseItemRenamed(fromPath, toPath); },
            Qt::QueuedConnection);
    }

    return 0;
}

int FuseDriver::fuseTruncate(const char* path, off_t size, struct fuse_file_info* fi) {
    auto* drv = self();

    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (drv && drv->isTrashFusePath(qpath)) {
        if (fi) {
            auto openFileOpt = drv->getOpenFile(fi->fh);
            if (openFileOpt && openFileOpt->fileId.isEmpty() && openFileOpt->path == qpath) {
                return (openFileOpt->localFd >= 0 && ::ftruncate(openFileOpt->localFd, size) == 0)
                           ? 0
                           : -EIO;
            }
        }

        QFile overlayFile(drv->trashOverlayPathForFusePath(qpath));
        if (!overlayFile.exists()) {
            return -ENOENT;
        }
        return overlayFile.resize(size) ? 0 : -EIO;
    }

    if (!drv || !drv->m_database || !drv->m_fileCache) {
        return -EIO;
    }

    FuseNode localNode = drv->m_database->getFuseNodeByPath(qpath);
    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty() && localNode.nodeId.isEmpty()) {
        return -ENOENT;
    }

    if (localNode.nodeId.isEmpty() && !meta.fileId.isEmpty()) {
        localNode = drv->ensureLocalNodeForMetadata(qpath, meta);
    }

    // Native docs are read-only — reject truncate
    if (isNativeDoc(meta)) {
        return -EACCES;
    }

    const int syncModeError = drv->enforceSyncModeForRemoteMutation(
        RemoteMutationType::Upload, QStringLiteral("modify files"), qpath);
    if (syncModeError != 0) {
        return syncModeError;
    }

    bool openedViaHandle = false;
    FuseOpenFile openFile;
    if (fi) {
        auto openFileOpt = drv->getOpenFile(fi->fh);
        if (openFileOpt &&
            ((!localNode.nodeId.isEmpty() && openFileOpt->nodeId == localNode.nodeId) ||
             (!meta.fileId.isEmpty() && openFileOpt->fileId == meta.fileId))) {
            openFile = *openFileOpt;
            openedViaHandle = true;
        }
    }

    if (!openedViaHandle) {
        if (!localNode.nodeId.isEmpty()) {
            const QString contentPath = drv->authoritativeContentPathForNode(localNode);
            if (contentPath.isEmpty()) {
                return -EIO;
            }

            const int localFd = openLocalHandle(contentPath, true);
            if (localFd < 0) {
                return -EIO;
            }

            int result = 0;
            if (::ftruncate(localFd, size) != 0) {
                result = -EIO;
            } else {
                FuseOpenFile stagedOpenFile;
                stagedOpenFile.nodeId = localNode.nodeId;
                stagedOpenFile.fileId = localNode.remoteFileId;
                stagedOpenFile.cacheKey =
                    localNode.remoteFileId.isEmpty() ? localNode.nodeId : localNode.remoteFileId;
                stagedOpenFile.path = qpath;
                stagedOpenFile.contentPath = contentPath;
                stagedOpenFile.localFd = localFd;
                stagedOpenFile.writable = true;

                if (!drv->commitNodeContentMutation(stagedOpenFile,
                                                    FuseJournalOperationType::Truncate)) {
                    result = -EIO;
                } else if (!localNode.remoteFileId.isEmpty()) {
                    drv->m_fileCache->markDirty(localNode.remoteFileId, lookupPath);
                    if (!drv->stageDirtyFileForUpload(localNode.remoteFileId, qpath, localFd,
                                                      localNode.nodeId, contentPath)) {
                        result = -EIO;
                    }
                } else if (::fsync(localFd) != 0) {
                    result = -EIO;
                }
            }

            ::close(localFd);
            return result;
        }

        return drv->truncateWithoutHandle(meta.fileId, meta.size, qpath, size);
    }

    if (openFile.localFd < 0 || ::ftruncate(openFile.localFd, size) != 0) {
        return -EIO;
    }

    if (!openFile.nodeId.isEmpty() &&
        !drv->commitNodeContentMutation(openFile, FuseJournalOperationType::Truncate)) {
        return -EIO;
    }

    // Mark as dirty for upload
    drv->markOpenFileDirty(fi->fh);
    if (!openFile.fileId.isEmpty()) {
        drv->m_fileCache->markDirty(openFile.fileId, lookupPath);
    }

    if (::fsync(openFile.localFd) != 0) {
        return -EIO;
    }

    return 0;
}

int FuseDriver::fuseCreate(const char* path, mode_t mode, struct fuse_file_info* fi) {
    Q_UNUSED(mode)
    auto* drv = self();

    QString qpath = normalizePath(path);
    QString parentPath = getParentPath(qpath);

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->createTrashOverlay(qpath, fi);
    }

    if (!drv || !drv->m_database || !drv->m_fileCache) {
        return -EIO;
    }

    const int syncModeError = drv->enforceSyncModeForRemoteMutation(
        RemoteMutationType::CreateFile, QStringLiteral("create files"), qpath);
    if (syncModeError != 0) {
        return syncModeError;
    }
    FuseOpenFile openFile;
    if (!drv->createAuthoritativeLocalFile(qpath, &openFile)) {
        return -EIO;
    }

    drv->invalidateFusePaths({parentPath, qpath});
    fi->fh = drv->registerOpenFile(openFile);

    QMetaObject::invokeMethod(
        drv, [drv, qpath]() { emit drv->fuseFileCreated(qpath); }, Qt::QueuedConnection);

    return 0;
}

// --- M5: Missing FUSE operations ---

int FuseDriver::fuseStatfs(const char* path, struct statvfs* stbuf) {
    Q_UNUSED(path)
    auto* drv = self();
    if (!drv)
        return -EIO;

    // Provide sensible defaults; a real implementation could query
    // GoogleDriveClient::getAboutInfo() for quota, but that is an
    // async network call.  For now, report a very large virtual FS.
    memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize = 4096;         // block size
    stbuf->f_frsize = 4096;        // fragment size
    stbuf->f_blocks = 1ULL << 30;  // ~4 PB total
    stbuf->f_bfree = 1ULL << 30;
    stbuf->f_bavail = 1ULL << 30;
    stbuf->f_files = 1000000;  // max inodes
    stbuf->f_ffree = 1000000;
    stbuf->f_namemax = 255;
    return 0;
}

int FuseDriver::fuseChmod(const char* path, mode_t mode, struct fuse_file_info* fi) {
    Q_UNUSED(path)
    Q_UNUSED(mode)
    Q_UNUSED(fi)
    // Google Drive does not support Unix permissions; silently succeed.
    return 0;
}

int FuseDriver::fuseChown(const char* path, uid_t uid, gid_t gid, struct fuse_file_info* fi) {
    Q_UNUSED(path)
    Q_UNUSED(uid)
    Q_UNUSED(gid)
    Q_UNUSED(fi)
    // Google Drive does not support Unix ownership; silently succeed.
    return 0;
}

int FuseDriver::fuseUtimens(const char* path, const struct timespec tv[2],
                            struct fuse_file_info* fi) {
    Q_UNUSED(path)
    Q_UNUSED(tv)
    Q_UNUSED(fi)
    // Timestamps are managed by Google Drive; silently succeed.
    return 0;
}

void* FuseDriver::fuseInit(struct fuse_conn_info* conn, struct fuse_config* cfg) {
    Q_UNUSED(conn)

    // M6 fix: set kernel-level attribute/entry cache timeouts to match
    // the metadata cache TTL, dramatically reducing DB hits.
    if (cfg) {
        cfg->attr_timeout = 300;  // 5 minutes (matches MetadataCache default)
        cfg->entry_timeout = 300;
        cfg->negative_timeout = 5;  // brief negative lookup caching
    }

    qDebug() << "FuseDriver: FUSE initialized";
    return fuse_get_context()->private_data;
}

void FuseDriver::fuseDestroy(void* private_data) {
    Q_UNUSED(private_data)
    qDebug() << "FuseDriver: FUSE destroyed";
}

// ============================================================================
// Internal Helper Methods
// ============================================================================

bool FuseDriver::initializeMetadataCache() {
    if (!m_database) {
        qWarning() << "FuseDriver: No database available for metadata cache";
        return false;
    }

    delete m_metadataCache;
    m_metadataCache = new MetadataCache(m_database, m_driveClient, this);

    if (!m_metadataCache->initialize()) {
        delete m_metadataCache;
        m_metadataCache = nullptr;
        qWarning() << "FuseDriver: Failed to initialize MetadataCache";
        return false;
    }

    if (m_metadataCache->rootFolderId().isEmpty()) {
        QString rootFolderId;
        if (m_driveClient) {
            rootFolderId = m_driveClient->getRootFolderId();
        }
        m_metadataCache->setRootFolderId(rootFolderId.isEmpty() ? QStringLiteral("root")
                                                                : rootFolderId);
    }

    qDebug() << "FuseDriver: Metadata cache initialized";
    return true;
}

bool FuseDriver::initializeFileCache() {
    if (!m_database || !m_driveClient) {
        return false;
    }

    m_fileCache = new FileCache(m_database, m_driveClient, this);
    m_fileCache->setPauseController(m_pauseController);

    if (!m_cacheDirectory.isEmpty()) {
        m_fileCache->setCacheDirectory(m_cacheDirectory);
    }

    if (m_maxCacheSizeBytes > 0) {
        m_fileCache->setMaxCacheSize(m_maxCacheSizeBytes);
    }

    if (!m_fileCache->initialize()) {
        delete m_fileCache;
        m_fileCache = nullptr;
        return false;
    }

    qDebug() << "FuseDriver: File cache initialized";
    return true;
}

bool FuseDriver::saveMetadataEntry(const FuseMetadata& metadata) {
    if (!m_database || !m_database->saveFuseMetadata(metadata)) {
        return false;
    }

    if (m_metadataCache) {
        m_metadataCache->setMetadata(toCacheMetadata(metadata), false);
    }

    return true;
}

bool FuseDriver::createAuthoritativeLocalFile(const QString& path, FuseOpenFile* openFile,
                                              const QString& sourcePath) {
    if (!m_database || !m_fileCache || !openFile || path.isEmpty() || path == QStringLiteral("/")) {
        return false;
    }

    const QString parentPath = getParentPath(path);
    const QString fileName = getFileName(path);
    if (fileName.isEmpty()) {
        return false;
    }

    const FuseNode existingNode = m_database->getFuseNodeByPath(path);
    if (!existingNode.nodeId.isEmpty()) {
        return false;
    }

    const QString relativePath = logicalPathFromFusePath(path);
    const FuseMetadata existingMetadata = m_database->getFuseMetadataByPath(relativePath);
    if (!existingMetadata.fileId.isEmpty()) {
        return false;
    }

    QString parentRemoteId;

    if (parentPath != QStringLiteral("/")) {
        const FuseNode parentNode = m_database->getFuseNodeByPath(parentPath);
        if (!parentNode.nodeId.isEmpty()) {
            if (!parentNode.isFolder) {
                return false;
            }
            parentRemoteId = parentNode.remoteFileId;
        } else {
            const QString parentRelative = parentPath.mid(1);
            const FuseMetadata parentMeta = m_database->getFuseMetadataByPath(parentRelative);
            if (parentMeta.fileId.isEmpty() || !parentMeta.isFolder) {
                return false;
            }
            parentRemoteId = parentMeta.fileId;
        }
    }

    const QString nodeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString contentPath = m_fileCache->getDirtyPathForFile(nodeId);
    if (!QDir().mkpath(QFileInfo(contentPath).dir().absolutePath())) {
        return false;
    }

    qint64 initialSize = 0;
    if (!sourcePath.isEmpty()) {
        if (!copyFileToPath(sourcePath, contentPath, nullptr)) {
            return false;
        }
        initialSize = QFileInfo(sourcePath).size();
    } else {
        QFile localFile(contentPath);
        if (!localFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        localFile.close();
    }

    FuseNode node;
    node.nodeId = nodeId;
    const FuseNode parentNode = m_database->getFuseNodeByPath(parentPath);
    node.parentNodeId = parentNode.nodeId;
    node.path = path;
    node.name = fileName;
    node.remoteName = fileName;
    node.isFolder = false;
    node.isPendingCreate = true;
    node.size = initialSize;
    node.mimeType = QStringLiteral("application/octet-stream");
    node.createdTime = QDateTime::currentDateTimeUtc();
    node.modifiedTime = node.createdTime;
    node.lastAccessed = node.createdTime;

    FuseNodeContentState state;
    state.nodeId = nodeId;
    state.localContentPath = contentPath;
    state.localGeneration = 0;
    state.remoteAckGeneration = 0;
    state.size = initialSize;
    state.lastLocalWrite = node.createdTime;

    FuseMutationTransaction mutation;
    mutation.nodesToUpsert.append(node);
    mutation.contentStatesToUpsert.append(state);
    mutation.journalEntry.idempotencyKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
    mutation.journalEntry.operationType = FuseJournalOperationType::CreateFile;
    mutation.journalEntry.nodeId = nodeId;
    mutation.journalEntry.parentNodeId = node.parentNodeId;
    mutation.journalEntry.path = path;

    if (!mutation.journalEntry.parentNodeId.isEmpty()) {
        const FuseNode persistedParent = m_database->getFuseNode(node.parentNodeId);
        mutation.journalEntry.remoteParentId = persistedParent.remoteFileId;
    } else if (parentPath != QStringLiteral("/")) {
        mutation.journalEntry.remoteParentId = parentRemoteId;
    }

    qint64 journalEntryId = 0;
    if (!m_database->commitFuseMutationTransaction(mutation, &journalEntryId)) {
        QFile::remove(contentPath);
        return false;
    }

    requestReplaySync();

    const int localFd = openLocalHandle(contentPath, true);
    if (localFd < 0) {
        return false;
    }

    if (::fsync(localFd) != 0) {
        ::close(localFd);
        return false;
    }

    openFile->nodeId = nodeId;
    openFile->fileId.clear();
    openFile->cacheKey = nodeId;
    openFile->path = path;
    openFile->contentPath = contentPath;
    openFile->localFd = localFd;
    openFile->size = initialSize;
    openFile->writable = true;
    openFile->dirty = false;
    openFile->synthetic = false;
    Q_UNUSED(journalEntryId)
    return true;
}

bool FuseDriver::createAuthoritativeLocalDirectory(const QString& path) {
    if (!m_database || path.isEmpty() || path == QStringLiteral("/")) {
        return false;
    }

    const QString parentPath = getParentPath(path);
    const QString directoryName = getFileName(path);
    if (directoryName.isEmpty()) {
        return false;
    }

    if (!m_database->getFuseNodeByPath(path).nodeId.isEmpty()) {
        return false;
    }

    const QString relativePath = logicalPathFromFusePath(path);
    if (!m_database->getFuseMetadataByPath(relativePath).fileId.isEmpty()) {
        return false;
    }

    QString parentRemoteId;
    QString parentNodeId;
    if (parentPath != QStringLiteral("/")) {
        const FuseNode parentNode = m_database->getFuseNodeByPath(parentPath);
        if (!parentNode.nodeId.isEmpty()) {
            if (!parentNode.isFolder) {
                return false;
            }
            parentNodeId = parentNode.nodeId;
            parentRemoteId = parentNode.remoteFileId;
        } else {
            const FuseMetadata parentMeta =
                m_database->getFuseMetadataByPath(logicalPathFromFusePath(parentPath));
            if (parentMeta.fileId.isEmpty() || !parentMeta.isFolder) {
                return false;
            }
            parentRemoteId = parentMeta.fileId;
        }
    }

    FuseNode node;
    node.nodeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    node.parentNodeId = parentNodeId;
    node.path = path;
    node.name = directoryName;
    node.remoteName = directoryName;
    node.isFolder = true;
    node.isPendingCreate = true;
    node.size = 0;
    node.mimeType = QStringLiteral("application/vnd.google-apps.folder");
    node.createdTime = QDateTime::currentDateTimeUtc();
    node.modifiedTime = node.createdTime;
    node.lastAccessed = node.createdTime;

    FuseMutationTransaction mutation;
    mutation.nodesToUpsert.append(node);
    mutation.journalEntry.idempotencyKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
    mutation.journalEntry.operationType = FuseJournalOperationType::CreateDirectory;
    mutation.journalEntry.nodeId = node.nodeId;
    mutation.journalEntry.parentNodeId = node.parentNodeId;
    mutation.journalEntry.path = path;
    mutation.journalEntry.remoteParentId = parentRemoteId;

    const bool committed = m_database->commitFuseMutationTransaction(mutation, nullptr);
    if (committed) {
        requestReplaySync();
    }
    return committed;
}

FuseNode FuseDriver::ensureLocalNodeForMetadata(const QString& path, const FuseMetadata& metadata) {
    if (!m_database || path.isEmpty() || metadata.fileId.isEmpty()) {
        return {};
    }

    FuseNode node = m_database->getFuseNodeByPath(path);
    if (!node.nodeId.isEmpty()) {
        if (node.remoteFileId != metadata.fileId || node.remoteParentId != metadata.parentId ||
            node.size != metadata.size || node.modifiedTime != metadata.modifiedTime) {
            node.remoteFileId = metadata.fileId;
            node.remoteParentId = metadata.parentId;
            node.name = getFileName(path);
            node.remoteName = metadata.remoteName.isEmpty() ? metadata.name : metadata.remoteName;
            node.mimeType = metadata.mimeType;
            node.remoteMimeType = metadata.remoteMimeType;
            node.webViewLink = metadata.webViewLink;
            node.nativeDocModeOverride = metadata.nativeDocModeOverride;
            node.isFolder = metadata.isFolder;
            node.isPendingCreate = false;
            node.size = metadata.size;
            node.createdTime = metadata.createdTime;
            node.modifiedTime = metadata.modifiedTime;
            node.lastAccessed = metadata.lastAccessed;
            node.lastSyncedAt = QDateTime::currentDateTimeUtc();
            if (!m_database->saveFuseNode(node)) {
                return {};
            }
        }
        return node;
    }

    node.nodeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString parentPath = getParentPath(path);
    node.parentNodeId = m_database->getFuseNodeByPath(parentPath).nodeId;
    node.remoteFileId = metadata.fileId;
    node.remoteParentId = metadata.parentId;
    node.path = path;
    node.name = getFileName(path);
    node.remoteName = metadata.remoteName.isEmpty() ? metadata.name : metadata.remoteName;
    node.mimeType = metadata.mimeType;
    node.remoteMimeType = metadata.remoteMimeType;
    node.webViewLink = metadata.webViewLink;
    node.nativeDocModeOverride = metadata.nativeDocModeOverride;
    node.isFolder = metadata.isFolder;
    node.isPendingCreate = false;
    node.size = metadata.size;
    node.createdTime = metadata.createdTime;
    node.modifiedTime = metadata.modifiedTime;
    node.lastAccessed = metadata.lastAccessed;
    node.lastSyncedAt = QDateTime::currentDateTimeUtc();
    if (!m_database->saveFuseNode(node)) {
        return {};
    }
    return node;
}

QString FuseDriver::authoritativeContentPathForNode(const FuseNode& node) const {
    if (!m_database || node.nodeId.isEmpty()) {
        return QString();
    }

    const FuseNodeContentState state = m_database->getFuseNodeContentState(node.nodeId);
    if (!state.nodeId.isEmpty() && !state.localContentPath.isEmpty()) {
        return state.localContentPath;
    }

    if (!node.remoteFileId.isEmpty() && m_fileCache) {
        return m_fileCache->getContentPath(node.remoteFileId);
    }

    return QString();
}

QString FuseDriver::contentIdentityForOpenFile(const FuseOpenFile& openFile) const {
    if (!openFile.fileId.isEmpty()) {
        return openFile.fileId;
    }
    return openFile.nodeId;
}

bool FuseDriver::commitNodeContentMutation(const FuseOpenFile& openFile,
                                           FuseJournalOperationType operation) {
    if (!m_database || openFile.nodeId.isEmpty() || openFile.localFd < 0) {
        return false;
    }

    FuseNode node = m_database->getFuseNode(openFile.nodeId);
    if (node.nodeId.isEmpty()) {
        return false;
    }

    FuseNodeContentState state = m_database->getFuseNodeContentState(node.nodeId);
    if (state.nodeId.isEmpty()) {
        state.nodeId = node.nodeId;
        state.remoteAckGeneration = 0;
    }

    const qint64 size = sizeForOpenHandle(openFile.localFd);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    state.localContentPath = openFile.contentPath;
    state.localGeneration += 1;
    state.size = size;
    state.lastLocalWrite = now;

    node.size = size;
    node.modifiedTime = now;
    node.lastAccessed = now;

    FuseMutationTransaction mutation;
    mutation.nodesToUpsert.append(node);
    mutation.contentStatesToUpsert.append(state);
    mutation.journalEntry.idempotencyKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
    mutation.journalEntry.operationType = operation;
    mutation.journalEntry.nodeId = node.nodeId;
    mutation.journalEntry.parentNodeId = node.parentNodeId;
    mutation.journalEntry.path = node.path;
    mutation.journalEntry.remoteFileId = node.remoteFileId;
    mutation.journalEntry.remoteParentId = node.remoteParentId;
    mutation.journalEntry.localGeneration = state.localGeneration;
    mutation.journalEntry.payloadJson = QStringLiteral("{\"size\":%1,\"contentPath\":\"%2\"}")
                                            .arg(size)
                                            .arg(state.localContentPath);

    const bool committed = m_database->commitFuseMutationTransaction(mutation, nullptr);
    if (committed) {
        requestReplaySync();
    }
    return committed;
}

bool FuseDriver::updateNodeContentPath(const QString& nodeId, const QString& contentPath) {
    if (!m_database || nodeId.isEmpty() || contentPath.isEmpty()) {
        return false;
    }

    FuseNodeContentState state = m_database->getFuseNodeContentState(nodeId);
    if (state.nodeId.isEmpty()) {
        state.nodeId = nodeId;
    }
    state.localContentPath = contentPath;
    state.size = sizeForExistingPath(contentPath);
    state.lastLocalWrite = QDateTime::currentDateTimeUtc();
    return m_database->saveFuseNodeContentState(state);
}

bool FuseDriver::applyLocalNodeRename(const QString& fromPath, const QString& toPath) {
    if (!m_database || fromPath.isEmpty() || toPath.isEmpty() || fromPath == toPath) {
        return false;
    }

    const QString fromLookup = logicalPathFromFusePath(fromPath);
    const QString toLookup = logicalPathFromFusePath(toPath);
    FuseMetadata meta = m_database->getFuseMetadataByPath(fromLookup);
    FuseNode node = m_database->getFuseNodeByPath(fromPath);
    if (node.nodeId.isEmpty() && !meta.fileId.isEmpty()) {
        node = ensureLocalNodeForMetadata(fromPath, meta);
    }
    if (node.nodeId.isEmpty()) {
        return false;
    }

    const FuseNode existingDestinationNode = m_database->getFuseNodeByPath(toPath);
    if (!existingDestinationNode.nodeId.isEmpty() &&
        existingDestinationNode.nodeId != node.nodeId) {
        return false;
    }
    const FuseMetadata existingDestinationMeta = m_database->getFuseMetadataByPath(toLookup);
    if (!existingDestinationMeta.fileId.isEmpty() &&
        existingDestinationMeta.fileId != node.remoteFileId) {
        return false;
    }

    const QString destinationParentPath = getParentPath(toPath);
    QString destinationParentNodeId;
    QString destinationParentRemoteId;
    if (destinationParentPath != QStringLiteral("/")) {
        const FuseNode parentNode = m_database->getFuseNodeByPath(destinationParentPath);
        if (!parentNode.nodeId.isEmpty()) {
            if (!parentNode.isFolder) {
                return false;
            }
            destinationParentNodeId = parentNode.nodeId;
            destinationParentRemoteId = parentNode.remoteFileId;
        } else {
            const FuseMetadata parentMeta =
                m_database->getFuseMetadataByPath(logicalPathFromFusePath(destinationParentPath));
            if (parentMeta.fileId.isEmpty() || !parentMeta.isFolder) {
                return false;
            }
            destinationParentRemoteId = parentMeta.fileId;
        }
    }

    QList<FuseNode> updatedNodes;
    const QList<FuseNode> allNodes = m_database->getAllFuseNodes();
    for (const FuseNode& candidate : allNodes) {
        if (!relativePathWithinSubtree(candidate.path, fromPath)) {
            continue;
        }

        FuseNode updated = candidate;
        updated.path = toPath + candidate.path.mid(fromPath.size());
        updated.name = getFileName(updated.path);
        if (candidate.nodeId == node.nodeId) {
            updated.parentNodeId = destinationParentNodeId;
            if (!updated.remoteFileId.isEmpty()) {
                updated.remoteName = updated.name;
            }
        }
        updated.modifiedTime = QDateTime::currentDateTimeUtc();
        updated.lastAccessed = updated.modifiedTime;
        updatedNodes.append(updated);
    }

    if (updatedNodes.isEmpty()) {
        return false;
    }

    FuseMutationTransaction mutation;
    mutation.nodesToUpsert = updatedNodes;
    mutation.journalEntry.idempotencyKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
    mutation.journalEntry.operationType = getParentPath(fromPath) == destinationParentPath
                                              ? FuseJournalOperationType::Rename
                                              : FuseJournalOperationType::Move;
    mutation.journalEntry.nodeId = node.nodeId;
    mutation.journalEntry.parentNodeId = node.parentNodeId;
    mutation.journalEntry.destinationParentNodeId = destinationParentNodeId;
    mutation.journalEntry.path = fromPath;
    mutation.journalEntry.destinationPath = toPath;
    mutation.journalEntry.remoteFileId = node.remoteFileId;
    mutation.journalEntry.remoteParentId =
        node.remoteParentId.isEmpty() ? destinationParentRemoteId : node.remoteParentId;

    if (!m_database->commitFuseMutationTransaction(mutation, nullptr)) {
        return false;
    }

    if (!meta.fileId.isEmpty()) {
        const QString newName = getFileName(toPath);
        const QString remoteNewName = remoteRenameTargetForMetadata(meta, newName);
        meta.path = toLookup;
        meta.name = newName;
        meta.remoteName = remoteNewName;
        if (!destinationParentRemoteId.isEmpty()) {
            meta.parentId = destinationParentRemoteId;
        }
        meta.cachedAt = QDateTime::currentDateTimeUtc();
        meta.lastAccessed = QDateTime::currentDateTimeUtc();
        if (!saveMetadataEntry(meta)) {
            return false;
        }
        if (meta.isFolder) {
            m_database->updateFuseChildrenPaths(meta.fileId, fromLookup, toLookup);
        }
    }

    invalidateFusePaths({fromPath, toPath, getParentPath(fromPath), destinationParentPath});
    requestReplaySync();
    return true;
}

int FuseDriver::applyLocalNodeRemoval(const QString& path, bool expectDirectory,
                                      bool requireEmptyDirectory) {
    if (!m_database || path.isEmpty() || path == QStringLiteral("/")) {
        return -EIO;
    }

    const QString lookupPath = logicalPathFromFusePath(path);
    FuseMetadata meta = m_database->getFuseMetadataByPath(lookupPath);
    FuseNode node = m_database->getFuseNodeByPath(path);
    if (node.nodeId.isEmpty() && !meta.fileId.isEmpty()) {
        node = ensureLocalNodeForMetadata(path, meta);
    }
    if (node.nodeId.isEmpty()) {
        return -ENOENT;
    }

    if (expectDirectory && !node.isFolder) {
        return -ENOTDIR;
    }
    if (!expectDirectory && node.isFolder) {
        return -EISDIR;
    }

    const QList<FuseNode> allNodes = m_database->getAllFuseNodes();
    QList<QString> nodeIdsToDelete;
    QList<QString> contentStatesToDelete;
    QStringList contentPathsToDelete;
    for (const FuseNode& candidate : allNodes) {
        if (!relativePathWithinSubtree(candidate.path, path)) {
            continue;
        }
        if (candidate.path != path && requireEmptyDirectory) {
            return -ENOTEMPTY;
        }
        nodeIdsToDelete.append(candidate.nodeId);
        const FuseNodeContentState contentState =
            m_database->getFuseNodeContentState(candidate.nodeId);
        if (!contentState.nodeId.isEmpty()) {
            contentStatesToDelete.append(candidate.nodeId);
            if (!contentState.localContentPath.isEmpty()) {
                contentPathsToDelete.append(contentState.localContentPath);
            }
        }
    }

    if (nodeIdsToDelete.isEmpty()) {
        nodeIdsToDelete.append(node.nodeId);
    }

    FuseMutationTransaction mutation;
    mutation.nodeIdsToDelete = nodeIdsToDelete;
    mutation.contentStateNodeIdsToDelete = contentStatesToDelete;
    mutation.journalEntry.idempotencyKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
    mutation.journalEntry.operationType = FuseJournalOperationType::Trash;
    mutation.journalEntry.nodeId = node.nodeId;
    mutation.journalEntry.parentNodeId = node.parentNodeId;
    mutation.journalEntry.path = path;
    mutation.journalEntry.remoteFileId = node.remoteFileId;
    mutation.journalEntry.remoteParentId = node.remoteParentId;

    if (!m_database->commitFuseMutationTransaction(mutation, nullptr)) {
        return -EIO;
    }

    for (const QString& contentPath : contentPathsToDelete) {
        QFile::remove(contentPath);
    }

    if (!meta.fileId.isEmpty()) {
        if (meta.isFolder) {
            deleteFuseMetadataSubtree(meta.path);
        } else {
            if (m_fileCache) {
                m_fileCache->removeFromCache(meta.fileId);
            }
            m_database->deleteNativeDocState(meta.fileId);
            m_database->deleteFuseMetadata(meta.fileId);
            removeMetadataEntryFromCache(meta);
        }
    }

    invalidateFusePaths({getParentPath(path), path});
    requestReplaySync();
    return 0;
}

void FuseDriver::appendLocalNodeChildrenToListing(const QString& path, void* buf,
                                                  fuse_fill_dir_t filler,
                                                  QSet<QString>& emittedNames) const {
    if (!m_database) {
        return;
    }

    const QList<FuseNode> nodes = m_database->getAllFuseNodes();
    for (const FuseNode& node : nodes) {
        if (node.isTrashed) {
            continue;
        }

        if (getParentPath(node.path) != path) {
            continue;
        }

        if (emittedNames.contains(node.name)) {
            continue;
        }

        filler(buf, node.name.toUtf8().constData(), nullptr, 0,
               node.isFolder ? FUSE_FILL_DIR_PLUS : static_cast<enum fuse_fill_dir_flags>(0));
        emittedNames.insert(node.name);
    }
}

void FuseDriver::requestReplaySync() {
    if (m_replayWorker) {
        QMetaObject::invokeMethod(m_replayWorker, "syncNow", Qt::QueuedConnection);
    }
}

QString FuseDriver::trashOverlayRoot() const {
    return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                           QStringLiteral("/Via/trash"));
}

bool FuseDriver::isTrashFusePath(const QString& path) const {
    return TrashPolicy::isTrashRelativePath(logicalPathFromFusePath(path));
}

QString FuseDriver::trashOverlayPathForFusePath(const QString& path) const {
    const QString logicalPath = logicalPathFromFusePath(path);
    if (logicalPath.isEmpty()) {
        return trashOverlayRoot();
    }

    return QDir(trashOverlayRoot()).filePath(logicalPath);
}

bool FuseDriver::ensureTrashOverlayParent(const QString& path) const {
    const QFileInfo overlayInfo(trashOverlayPathForFusePath(path));
    QDir parentDir = overlayInfo.dir();
    return parentDir.exists() || parentDir.mkpath(QStringLiteral("."));
}

bool FuseDriver::ensureTrashOverlayDirectory(const QString& path) const {
    QDir dir(trashOverlayPathForFusePath(path));
    return dir.exists() || dir.mkpath(QStringLiteral("."));
}

int FuseDriver::getattrTrashOverlay(const QString& path, struct stat* stbuf) const {
    const QString overlayPath = trashOverlayPathForFusePath(path);
    const QFileInfo info(overlayPath);
    if (!info.exists() && !info.isSymLink()) {
        return -ENOENT;
    }

    if (!fillStatFromPath(overlayPath, stbuf)) {
        return -EIO;
    }

    return 0;
}

int FuseDriver::readdirTrashOverlay(const QString& path, void* buf, fuse_fill_dir_t filler) const {
    const QString overlayPath = trashOverlayPathForFusePath(path);
    const QFileInfo info(overlayPath);
    if (!info.exists()) {
        return -ENOENT;
    }
    if (!info.isDir()) {
        return -ENOTDIR;
    }

    QDir dir(overlayPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo& entry : entries) {
        filler(buf, entry.fileName().toUtf8().constData(), nullptr, 0,
               entry.isDir() ? FUSE_FILL_DIR_PLUS : static_cast<enum fuse_fill_dir_flags>(0));
    }

    return 0;
}

int FuseDriver::openTrashOverlay(const QString& path, struct fuse_file_info* fi) {
    const QString overlayPath = trashOverlayPathForFusePath(path);
    const QFileInfo info(overlayPath);
    if (!info.exists()) {
        return -ENOENT;
    }
    if (info.isDir()) {
        return -EISDIR;
    }

    FuseOpenFile openFile;
    openFile.path = path;
    openFile.writable = (fi->flags & O_WRONLY) || (fi->flags & O_RDWR);
    openFile.dirty = false;
    openFile.synthetic = false;
    openFile.localFd = openLocalHandle(overlayPath, openFile.writable);
    if (openFile.localFd < 0) {
        return -EIO;
    }
    openFile.size = sizeForOpenHandle(openFile.localFd);

    fi->fh = registerOpenFile(openFile);
    emit fileAccessed(path);
    return 0;
}

int FuseDriver::createTrashOverlay(const QString& path, struct fuse_file_info* fi) {
    if (!ensureTrashOverlayParent(path)) {
        return -EIO;
    }

    const QString overlayPath = trashOverlayPathForFusePath(path);
    const QByteArray encodedPath = QFile::encodeName(overlayPath);
    const int openFlags = (fi ? fi->flags : O_RDWR) | O_CREAT | O_TRUNC;
    const int localFd = ::open(encodedPath.constData(), openFlags, 0600);
    if (localFd < 0) {
        return -EIO;
    }

    FuseOpenFile openFile;
    openFile.path = path;
    openFile.writable = true;
    openFile.dirty = false;
    openFile.synthetic = false;
    openFile.localFd = localFd;
    openFile.size = 0;

    if (fi) {
        fi->fh = registerOpenFile(openFile);
    }

    return 0;
}

int FuseDriver::mkdirTrashOverlay(const QString& path) const {
    const QFileInfo info(trashOverlayPathForFusePath(path));
    if (info.exists()) {
        return -EEXIST;
    }

    return ensureTrashOverlayDirectory(path) ? 0 : -EIO;
}

int FuseDriver::unlinkTrashOverlay(const QString& path) const {
    const QString overlayPath = trashOverlayPathForFusePath(path);
    const QFileInfo info(overlayPath);
    if (!info.exists()) {
        return -ENOENT;
    }
    if (info.isDir()) {
        return -EISDIR;
    }

    return QFile::remove(overlayPath) ? 0 : -EIO;
}

int FuseDriver::rmdirTrashOverlay(const QString& path) const {
    const QString overlayPath = trashOverlayPathForFusePath(path);
    const QFileInfo info(overlayPath);
    if (!info.exists()) {
        return -ENOENT;
    }
    if (!info.isDir()) {
        return -ENOTDIR;
    }

    const QDir dir(overlayPath);
    if (!dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)
             .isEmpty()) {
        return -ENOTEMPTY;
    }

    QDir parentDir = info.dir();
    return parentDir.rmdir(info.fileName()) ? 0 : -EIO;
}

int FuseDriver::renameWithinTrashOverlay(const QString& fromPath, const QString& toPath) const {
    const QString fromOverlayPath = trashOverlayPathForFusePath(fromPath);
    const QFileInfo fromInfo(fromOverlayPath);
    if (!fromInfo.exists()) {
        return -ENOENT;
    }
    if (!ensureTrashOverlayParent(toPath)) {
        return -EIO;
    }

    const QString toOverlayPath = trashOverlayPathForFusePath(toPath);
    const QByteArray encodedFrom = QFile::encodeName(fromOverlayPath);
    const QByteArray encodedTo = QFile::encodeName(toOverlayPath);
    return ::rename(encodedFrom.constData(), encodedTo.constData()) == 0 ? 0 : -EIO;
}

QString FuseDriver::visibleNameForRemoteFile(const DriveFile& file) const {
    if (file.isShortcut) {
        return QString();
    }

    if (!file.isGoogleDoc() || file.isFolder) {
        return file.name;
    }

    const QString nativeDocModeOverride = nativeDocModeOverrideForFile(m_database, file.id);
    const NativeDocRepresentation representation = effectiveNativeDocRepresentation(
        file.mimeType, nativeDocModeOverride,
        nativeDocModeFromString(QSettings().value("advanced/nativeDocMode", "hide").toString()));
    if (!representation.visible) {
        return QString();
    }

    return nativeDocVisibleName(file.name, representation);
}

bool FuseDriver::snapshotFuseMetadataToTrash(const FuseMetadata& metadata, const QString& trashPath,
                                             QString* errorOut) {
    const auto snapshotVisibleFile =
        [&](const QString& fileId, qint64 size, const QString& mimeType,
            const QString& remoteMimeType, const QString& webViewLink,
            const QString& nativeDocModeOverride, const QString& destinationPath) -> bool {
        const QString effectiveRemoteMimeType =
            remoteMimeType.isEmpty() ? mimeType : remoteMimeType;
        const QString overlayPath = trashOverlayPathForFusePath(destinationPath);

        if (isNativeDocMimeType(effectiveRemoteMimeType)) {
            const NativeDocRepresentation representation = effectiveNativeDocRepresentation(
                effectiveRemoteMimeType, nativeDocModeOverride,
                nativeDocModeFromString(
                    QSettings().value("advanced/nativeDocMode", "hide").toString()));
            if (!representation.visible) {
                if (errorOut) {
                    *errorOut =
                        QStringLiteral("Hidden native document cannot be moved to local trash");
                }
                return false;
            }

            if (representation.synthetic) {
                return writeBytesToPath(
                    nativeDocShortcutPayload(webViewLink, effectiveRemoteMimeType), overlayPath,
                    errorOut);
            }

            if (!m_fileCache) {
                if (errorOut) {
                    *errorOut =
                        QStringLiteral("File cache unavailable for native-doc trash snapshot");
                }
                return false;
            }

            const QString exportMimeType = representation.outputMimeType;
            QString sourcePath = m_fileCache->hasLocalContent(fileId, exportMimeType)
                                     ? m_fileCache->getContentPath(fileId, exportMimeType)
                                     : m_fileCache->getExportedPath(fileId, exportMimeType);
            if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
                if (errorOut) {
                    *errorOut = QStringLiteral("Failed to materialize native-doc trash snapshot");
                }
                return false;
            }

            return copyFileToPath(sourcePath, overlayPath, errorOut);
        }

        if (!m_fileCache) {
            if (errorOut) {
                *errorOut = QStringLiteral("File cache unavailable for trash snapshot");
            }
            return false;
        }

        QString sourcePath = m_fileCache->hasLocalContent(fileId)
                                 ? m_fileCache->getContentPath(fileId)
                                 : m_fileCache->getCachedPath(fileId, size);
        if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to materialize local trash snapshot");
            }
            return false;
        }

        return copyFileToPath(sourcePath, overlayPath, errorOut);
    };

    if (!ensureTrashOverlayParent(trashPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to create trash overlay parent directory");
        }
        return false;
    }

    if (!metadata.isFolder) {
        return snapshotVisibleFile(metadata.fileId, metadata.size, metadata.mimeType,
                                   metadata.remoteMimeType, metadata.webViewLink,
                                   metadata.nativeDocModeOverride, trashPath);
    }

    if (!ensureTrashOverlayDirectory(trashPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to create trash overlay directory");
        }
        return false;
    }

    if (!m_driveClient) {
        if (errorOut) {
            *errorOut = QStringLiteral("Google Drive client unavailable for folder trash snapshot");
        }
        return false;
    }

    const std::function<bool(const DriveFile&, const QString&)> snapshotTree =
        [&](const DriveFile& file, const QString& currentTrashPath) -> bool {
        if (file.isShortcut) {
            return true;
        }

        if (!file.isFolder) {
            return snapshotVisibleFile(
                file.id, file.size, file.mimeType, file.mimeType, file.webViewLink,
                nativeDocModeOverrideForFile(m_database, file.id), currentTrashPath);
        }

        if (!ensureTrashOverlayDirectory(currentTrashPath)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to create local trash folder snapshot");
            }
            return false;
        }

        QList<DriveFile> children;
        QString listError;
        if (!waitForListFiles(m_driveClient, file.id, &children, &listError)) {
            if (errorOut) {
                *errorOut = listError;
            }
            return false;
        }

        for (const DriveFile& child : children) {
            if (child.trashed || child.isShortcut) {
                continue;
            }

            const QString childName = visibleNameForRemoteFile(child);
            if (childName.isEmpty()) {
                continue;
            }

            const QString childTrashPath = joinFusePath(currentTrashPath, childName);
            const FuseMetadata existingMetadata =
                m_database ? m_database->getFuseMetadata(child.id) : FuseMetadata();
            if (!existingMetadata.fileId.isEmpty()) {
                if (!snapshotFuseMetadataToTrash(existingMetadata, childTrashPath, errorOut)) {
                    return false;
                }
                continue;
            }

            if (!snapshotTree(child, childTrashPath)) {
                return false;
            }
        }

        return true;
    };

    DriveFile rootFile;
    rootFile.id = metadata.fileId;
    rootFile.name = metadata.remoteName.isEmpty() ? metadata.name : metadata.remoteName;
    rootFile.mimeType =
        metadata.remoteMimeType.isEmpty() ? metadata.mimeType : metadata.remoteMimeType;
    rootFile.size = metadata.size;
    rootFile.createdTime = metadata.createdTime;
    rootFile.modifiedTime = metadata.modifiedTime;
    rootFile.parents = metadata.parentId.isEmpty() ? QStringList{} : QStringList{metadata.parentId};
    rootFile.isFolder = true;
    rootFile.webViewLink = metadata.webViewLink;

    return snapshotTree(rootFile, trashPath);
}

bool FuseDriver::moveLiveEntryToTrash(const FuseMetadata& metadata, const QString& fromPath,
                                      const QString& toPath, QString* errorOut) {
    if (!snapshotFuseMetadataToTrash(metadata, toPath, errorOut)) {
        return false;
    }

    QString error;
    bool authFailure = false;
    if (!waitForTrash(
            m_driveClient, metadata.fileId,
            [&]() {
                return invokeDriveCall(m_driveClient,
                                       [&]() { m_driveClient->trashFile(metadata.fileId); });
            },
            &error, &authFailure)) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }

    deleteFuseMetadataSubtree(metadata.path);
    invalidateFusePaths({fromPath, toPath, getParentPath(fromPath), getParentPath(toPath)});
    return true;
}

bool FuseDriver::restoreTrashEntryToLive(const QString& fromPath, const QString& toPath,
                                         QString* errorOut) {
    const bool driveAllowed = isDriveApiAllowed();
    if (!m_database || !m_fileCache || (driveAllowed && !m_driveClient)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Fuse restore dependencies unavailable");
        }
        return false;
    }

    const QString sourceOverlayPath = trashOverlayPathForFusePath(fromPath);
    const QFileInfo sourceInfo(sourceOverlayPath);
    if (!sourceInfo.exists()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Trash overlay entry does not exist");
        }
        return false;
    }

    if (!driveAllowed) {
        std::function<bool(const QFileInfo&, const QString&)> restoreTreeLocally;
        restoreTreeLocally = [&](const QFileInfo& entryInfo,
                                 const QString& destinationFusePath) -> bool {
            if (entryInfo.isDir()) {
                if (!createAuthoritativeLocalDirectory(destinationFusePath)) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("Failed to restore local trash folder snapshot");
                    }
                    return false;
                }

                QDir dir(entryInfo.absoluteFilePath());
                const QFileInfoList children = dir.entryInfoList(
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDir::Name);
                for (const QFileInfo& child : children) {
                    if (!restoreTreeLocally(child,
                                            joinFusePath(destinationFusePath, child.fileName()))) {
                        return false;
                    }
                }

                return true;
            }

            FuseOpenFile restoredFile;
            if (!createAuthoritativeLocalFile(destinationFusePath, &restoredFile,
                                              entryInfo.absoluteFilePath())) {
                if (errorOut) {
                    *errorOut = QStringLiteral("Failed to restore local trash file snapshot");
                }
                return false;
            }

            if (restoredFile.localFd >= 0) {
                ::close(restoredFile.localFd);
            }

            return true;
        };

        if (!restoreTreeLocally(sourceInfo, toPath)) {
            return false;
        }

        if (sourceInfo.isDir()) {
            QDir(sourceOverlayPath).removeRecursively();
        } else {
            QFile::remove(sourceOverlayPath);
        }

        invalidateFusePaths({fromPath, toPath, getParentPath(fromPath), getParentPath(toPath)});
        return true;
    }

    QString parentId = QStringLiteral("root");
    const QString destinationParentPath = getParentPath(toPath);
    if (!destinationParentPath.isEmpty() && destinationParentPath != QStringLiteral("/")) {
        const QString parentLookup = logicalPathFromFusePath(destinationParentPath);
        const FuseMetadata parentMeta = m_database->getFuseMetadataByPath(parentLookup);
        if (parentMeta.fileId.isEmpty() || !parentMeta.isFolder) {
            if (errorOut) {
                *errorOut = QStringLiteral("Restore destination parent is missing");
            }
            return false;
        }
        parentId = parentMeta.fileId;
    }

    const std::function<bool(const QFileInfo&, const QString&, const QString&)> restoreTree =
        [&](const QFileInfo& entryInfo, const QString& destinationFusePath,
            const QString& currentParentId) -> bool {
        const QString requestLocalPath = logicalPathFromFusePath(destinationFusePath);
        const QString entryName = getFileName(destinationFusePath);

        if (entryInfo.isDir()) {
            QString error;
            DriveFile createdFolder;
            bool authFailure = false;
            if (!waitForFolderCreate(
                    m_driveClient, requestLocalPath,
                    [&]() {
                        return invokeDriveCall(m_driveClient, [&]() {
                            m_driveClient->createFolder(entryName, currentParentId,
                                                        requestLocalPath);
                        });
                    },
                    &createdFolder, &error, &authFailure)) {
                Q_UNUSED(authFailure)
                if (errorOut) {
                    *errorOut = error;
                }
                return false;
            }

            FuseMetadata folderMeta;
            folderMeta.fileId = createdFolder.id;
            folderMeta.path = requestLocalPath;
            folderMeta.name = entryName;
            folderMeta.remoteName = createdFolder.name.isEmpty() ? entryName : createdFolder.name;
            folderMeta.parentId = currentParentId;
            folderMeta.isFolder = true;
            folderMeta.size = 0;
            folderMeta.mimeType = createdFolder.mimeType;
            folderMeta.createdTime = createdFolder.createdTime;
            folderMeta.modifiedTime = createdFolder.modifiedTime;
            folderMeta.cachedAt = QDateTime::currentDateTimeUtc();
            folderMeta.lastAccessed = QDateTime::currentDateTimeUtc();
            if (!saveMetadataEntry(folderMeta)) {
                if (errorOut) {
                    *errorOut = QStringLiteral("Failed to save restored folder metadata");
                }
                return false;
            }

            QDir dir(entryInfo.absoluteFilePath());
            const QFileInfoList children = dir.entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            for (const QFileInfo& child : children) {
                if (!restoreTree(child, joinFusePath(destinationFusePath, child.fileName()),
                                 createdFolder.id)) {
                    return false;
                }
            }

            return true;
        }

        QString error;
        DriveFile uploadedFile;
        bool authFailure = false;
        if (!waitForUpload(
                m_driveClient, entryInfo.absoluteFilePath(), &uploadedFile,
                [&]() {
                    return invokeDriveCall(m_driveClient, [&]() {
                        m_driveClient->uploadFile(entryInfo.absoluteFilePath(), currentParentId,
                                                  entryName);
                    });
                },
                &error, &authFailure)) {
            Q_UNUSED(authFailure)
            if (errorOut) {
                *errorOut = error;
            }
            return false;
        }

        const QString canonicalCachePath = m_fileCache->getCachePathForFile(uploadedFile.id);
        if (!canonicalCachePath.isEmpty()) {
            copyFileToPath(entryInfo.absoluteFilePath(), canonicalCachePath, nullptr);
            m_fileCache->recordCacheEntry(uploadedFile.id, canonicalCachePath, entryInfo.size());
        }

        FuseMetadata fileMeta;
        fileMeta.fileId = uploadedFile.id;
        fileMeta.path = requestLocalPath;
        fileMeta.name = entryName;
        fileMeta.remoteName = uploadedFile.name.isEmpty() ? entryName : uploadedFile.name;
        fileMeta.parentId = currentParentId;
        fileMeta.isFolder = false;
        fileMeta.size = entryInfo.size();
        fileMeta.mimeType = uploadedFile.mimeType;
        fileMeta.createdTime = uploadedFile.createdTime;
        fileMeta.modifiedTime = uploadedFile.modifiedTime;
        fileMeta.cachedAt = QDateTime::currentDateTimeUtc();
        fileMeta.lastAccessed = QDateTime::currentDateTimeUtc();
        if (!saveMetadataEntry(fileMeta)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to save restored file metadata");
            }
            return false;
        }

        return true;
    };

    if (!restoreTree(sourceInfo, toPath, parentId)) {
        return false;
    }

    if (sourceInfo.isDir()) {
        QDir(sourceOverlayPath).removeRecursively();
    } else {
        QFile::remove(sourceOverlayPath);
    }

    invalidateFusePaths({fromPath, toPath, getParentPath(fromPath), getParentPath(toPath)});
    return true;
}

void FuseDriver::deleteFuseMetadataSubtree(const QString& rootPath) {
    if (!m_database || rootPath.isEmpty()) {
        return;
    }

    QList<FuseMetadata> allEntries = m_database->getAllFuseMetadata();
    std::sort(allEntries.begin(), allEntries.end(),
              [](const FuseMetadata& lhs, const FuseMetadata& rhs) {
                  return lhs.path.size() > rhs.path.size();
              });

    for (const FuseMetadata& entry : allEntries) {
        if (!relativePathWithinSubtree(entry.path, rootPath)) {
            continue;
        }

        if (!entry.isFolder && m_fileCache) {
            m_fileCache->removeFromCache(entry.fileId);
        }

        m_database->deleteNativeDocState(entry.fileId);
        m_database->deleteFuseMetadata(entry.fileId);
        removeMetadataEntryFromCache(entry);
    }
}

void FuseDriver::removeMetadataEntryFromCache(const FuseMetadata& metadata) {
    if (!m_metadataCache || metadata.fileId.isEmpty()) {
        return;
    }

    m_metadataCache->removeByFileId(metadata.fileId, false);
}

bool FuseDriver::reconcileMovedMetadata(const FuseMetadata& previousMetadata,
                                        const FuseMetadata& updatedMetadata) {
    if (!m_database || !m_database->saveFuseMetadata(updatedMetadata)) {
        return false;
    }

    if (updatedMetadata.isFolder) {
        m_database->updateFuseChildrenPaths(updatedMetadata.fileId, previousMetadata.path,
                                            updatedMetadata.path);
    }

    if (m_metadataCache) {
        if (updatedMetadata.isFolder) {
            m_metadataCache->dropSubtreeFromCache(previousMetadata.path);
        }

        m_metadataCache->setMetadata(toCacheMetadata(updatedMetadata), false);
    }

    return true;
}

void FuseDriver::invalidateFusePaths(const QList<QString>& paths) {
    QSet<QString> uniquePaths;
    for (QString path : paths) {
        if (path.isEmpty()) {
            continue;
        }
        if (!path.startsWith(QLatin1Char('/'))) {
            path.prepend(QLatin1Char('/'));
        }
        uniquePaths.insert(path);
    }

    for (const QString& path : uniquePaths) {
        invalidateFusePath(m_fuse, path);
    }
}

void FuseDriver::handleRemoteChangeProcessed(const QString& displayPath,
                                             const QString& changeType) {
    Q_UNUSED(changeType)

    const QString fusePath = fusePathFromMetadataPath(displayPath);
    if (m_metadataCache) {
        m_metadataCache->invalidateChildren(getParentPath(displayPath));
    }
    invalidateFusePaths({fusePath, getParentPath(fusePath)});
}

bool FuseDriver::stageDirtyFileForUpload(const QString& fileId, const QString& path, int localFd,
                                         const QString& nodeId, const QString& sourcePath) {
    if (fileId.isEmpty() || !m_fileCache) {
        return false;
    }

    if (localFd >= 0) {
        ::fsync(localFd);
    }

    QString resolvedNodeId = nodeId;
    if (resolvedNodeId.isEmpty() && m_database) {
        const QString normalizedPath =
            path.startsWith(QLatin1Char('/')) ? path : QStringLiteral("/") + path;
        resolvedNodeId = m_database->getFuseNodeByPath(normalizedPath).nodeId;
    }

    QString authoritativeSourcePath = sourcePath;
    if (authoritativeSourcePath.isEmpty() && !resolvedNodeId.isEmpty() && m_database) {
        const FuseNodeContentState state = m_database->getFuseNodeContentState(resolvedNodeId);
        authoritativeSourcePath = state.localContentPath;
    }

    QString pendingPath = m_fileCache->moveToDirtyStore(fileId, authoritativeSourcePath);
    if (pendingPath.isEmpty()) {
        return false;
    }

    {
        QMutexLocker locker(&m_openFilesMutex);
        for (auto it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
            FuseOpenFile& openFile = it.value();
            if (openFile.fileId != fileId || openFile.synthetic || openFile.localFd < 0) {
                continue;
            }
            if (openFile.localFd == localFd) {
                continue;
            }

            const int newFd = openLocalHandle(pendingPath, openFile.writable);
            if (newFd < 0) {
                qCritical() << "FuseDriver: failed to retarget open handle for" << fileId
                            << "to pending store";
                continue;
            }

            ::close(openFile.localFd);
            openFile.localFd = newFd;
            openFile.contentPath = pendingPath;
            openFile.size = sizeForOpenHandle(newFd);
        }
    }
    if (!resolvedNodeId.isEmpty()) {
        updateNodeContentPath(resolvedNodeId, pendingPath);
    }

    if (m_database) {
        FuseMetadata meta = m_database->getFuseMetadata(fileId);
        if (!meta.fileId.isEmpty()) {
            meta.size = sizeForExistingPath(pendingPath);
            meta.modifiedTime = QDateTime::currentDateTime();
            meta.lastAccessed = QDateTime::currentDateTime();
            m_database->saveFuseMetadata(meta);
        }
    }

    if (m_dirtySyncWorker) {
        QMetaObject::invokeMethod(m_dirtySyncWorker, "syncNow", Qt::QueuedConnection);
    }

    qDebug() << "FuseDriver: staged dirty file for upload" << path;
    return true;
}

void FuseDriver::stageDirtyFilesForPause() {
    if (!m_fileCache) {
        return;
    }

    const QList<DirtyFileEntry> dirtyFiles = m_fileCache->getDirtyFiles();
    for (const DirtyFileEntry& entry : dirtyFiles) {
        const QString fusePath =
            entry.path.startsWith(QLatin1Char('/')) ? entry.path : QStringLiteral("/") + entry.path;
        stageDirtyFileForUpload(entry.fileId, fusePath, -1);
    }
}

int FuseDriver::truncateWithoutHandle(const QString& fileId, qint64 expectedSize,
                                      const QString& path, off_t size) {
    if (fileId.isEmpty() || !m_fileCache) {
        return -EIO;
    }

    if (!isDriveApiAllowed() && !m_fileCache->hasLocalContent(fileId)) {
        emitDriveOperationBlocked(QStringLiteral("download uncached files"), path);
        return -EHOSTDOWN;
    }

    QString cachePath = m_fileCache->getCachedPath(fileId, expectedSize);
    if (cachePath.isEmpty()) {
        return -EIO;
    }

    const int localFd = openLocalHandle(cachePath, true);
    if (localFd < 0) {
        return -EIO;
    }

    const QString lookupPath = path.startsWith("/") ? path.mid(1) : path;
    int result = 0;

    if (::ftruncate(localFd, size) != 0) {
        result = -EIO;
    } else {
        m_fileCache->markDirty(fileId, lookupPath);
        if (!stageDirtyFileForUpload(fileId, path, localFd, QString(), cachePath)) {
            result = -EIO;
        }
    }

    ::close(localFd);
    return result;
}

void FuseDriver::startBackgroundWorkers() {
    qDebug() << "FuseDriver: Starting background workers";

    if (m_fileCache && m_driveClient && m_database && !m_dirtySyncThread && !m_dirtySyncWorker) {
        m_dirtySyncThread = new QThread(this);
        m_dirtySyncWorker = new DirtySyncWorker(m_fileCache, m_driveClient, m_database);
        m_dirtySyncWorker->moveToThread(m_dirtySyncThread);

        connect(m_dirtySyncThread, &QThread::started, m_dirtySyncWorker, &DirtySyncWorker::start);
        connect(m_dirtySyncThread, &QThread::finished, m_dirtySyncWorker, &QObject::deleteLater);

        // Relay upload activity signals so external consumers (tray icon) can
        // track FUSE upload progress.
        connect(m_dirtySyncWorker, &DirtySyncWorker::uploadStarted, this,
                &FuseDriver::uploadStarted);
        connect(m_dirtySyncWorker, &DirtySyncWorker::uploadCompleted, this,
                &FuseDriver::uploadFinished);
        connect(m_dirtySyncWorker, &DirtySyncWorker::uploadFailed, this,
                [this](const QString& fileId, const QString& path, const QString& error) {
                    emit uploadFinished(fileId, path);
                    emit fuseUploadFailed(path, error);
                });
        connect(m_dirtySyncWorker, &DirtySyncWorker::stateChanged, this,
                [this](DirtySyncWorkerState state) {
                    emit uploadActivityChanged(state == DirtySyncWorkerState::Uploading);
                });

        m_dirtySyncThread->start();
        if (m_backgroundSyncPaused || !isDriveApiAllowed()) {
            QMetaObject::invokeMethod(m_dirtySyncWorker, "pause", Qt::QueuedConnection);
        }
    }

    // Relay file download activity signals from the cache.
    if (m_fileCache) {
        connect(m_fileCache, &FileCache::downloadStarted, this, &FuseDriver::downloadStarted);
        connect(m_fileCache, &FileCache::downloadCompleted, this,
                [this](const QString& fileId, const QString&) {
                    emit downloadFinished(fileId);

                    if (!m_fuse || !m_database) {
                        return;
                    }

                    const FuseMetadata meta = m_database->getFuseMetadata(fileId);
                    if (meta.fileId.isEmpty() || !isNativeDoc(meta)) {
                        return;
                    }

                    const QString path = nativeDocFusePath(meta);
                    const QByteArray encodedPath = QFile::encodeName(path);
                    const int rc = fuse_invalidate_path(m_fuse, encodedPath.constData());
                    if (rc != 0 && rc != -ENOENT) {
                        qWarning() << "FuseDriver: Failed to invalidate native-doc path cache for"
                                   << path << "after export:" << rc;
                    }
                });
        connect(m_fileCache, &FileCache::downloadFailedDetailed, this,
                [this](const QString& fileId, const QString& error, int httpStatus) {
                    if (!m_database || !m_metadataCache ||
                        !isNativeDocExportLimitError(error, httpStatus)) {
                        return;
                    }

                    QString oldPath;
                    const FuseFileMetadata updated = m_metadataCache->applyNativeDocModeOverride(
                        fileId, nativeDocModeToString(NativeDocMode::BrowserShortcut), &oldPath);
                    if (!updated.isValid()) {
                        return;
                    }

                    const QString oldFusePath = fusePathFromMetadataPath(oldPath);
                    const QString newFusePath = fusePathFromMetadataPath(updated.path);
                    const QString parentFusePath = [&]() {
                        const QString rawParent = QFileInfo(newFusePath).path();
                        return rawParent.isEmpty() || rawParent == QStringLiteral(".")
                                   ? QStringLiteral("/")
                                   : rawParent;
                    }();
                    const QString metadataParentPath = parentFusePath == QStringLiteral("/")
                                                           ? QStringLiteral("/")
                                                           : parentFusePath.mid(1);

                    m_metadataCache->invalidateChildren(metadataParentPath);

                    invalidateFusePath(m_fuse, oldFusePath);
                    invalidateFusePath(m_fuse, newFusePath);
                    invalidateFusePath(m_fuse, parentFusePath);
                });
        connect(m_fileCache, &FileCache::downloadFailed, this,
                [this](const QString& fileId, const QString& error) {
                    emit downloadFinished(fileId);

                    if (!m_database || !isLocalNativeDocExportFailure(error)) {
                        return;
                    }

                    const FuseMetadata meta = m_database->getFuseMetadata(fileId);
                    if (meta.fileId.isEmpty() || !isNativeDoc(meta)) {
                        return;
                    }

                    if (meta.nativeDocModeOverride ==
                        nativeDocModeToString(NativeDocMode::BrowserShortcut)) {
                        return;
                    }

                    emit nativeDocExportFailed(nativeDocFusePath(meta), error);
                });
    }

    if (m_metadataCache && m_fileCache && m_database && m_driveClient && !m_metadataRefreshThread &&
        !m_metadataRefreshWorker) {
        m_metadataRefreshThread = new QThread(this);
        m_metadataRefreshWorker =
            new MetadataRefreshWorker(m_metadataCache, m_fileCache, m_database, m_driveClient);
        m_metadataRefreshWorker->moveToThread(m_metadataRefreshThread);

        connect(m_metadataRefreshThread, &QThread::started, m_metadataRefreshWorker,
                &MetadataRefreshWorker::start);
        connect(m_metadataRefreshThread, &QThread::finished, m_metadataRefreshWorker,
                &QObject::deleteLater);

        // Relay path-aware remote change events for UI activity logging.
        connect(m_metadataRefreshWorker, &MetadataRefreshWorker::changeProcessedDetailed, this,
                &FuseDriver::fuseRemoteChange);
        connect(m_metadataRefreshWorker, &MetadataRefreshWorker::changeProcessedDetailed, this,
                &FuseDriver::handleRemoteChangeProcessed);
        connect(m_metadataRefreshWorker, &MetadataRefreshWorker::refreshCompleted, this,
                [this](int) { emit metadataRefreshed(); });
        connect(m_metadataRefreshWorker, &MetadataRefreshWorker::error, this,
                &FuseDriver::metadataRefreshFailed);

        m_metadataRefreshThread->start();
        if (m_backgroundSyncPaused || !isDriveApiAllowed()) {
            QMetaObject::invokeMethod(m_metadataRefreshWorker, "pause", Qt::QueuedConnection);
        }
    }

    if (m_database && m_driveClient && !m_replayThread && !m_replayWorker) {
        m_replayThread = new QThread(this);
        m_replayWorker = new FuseReplayWorker(m_database, m_driveClient);
        m_replayWorker->moveToThread(m_replayThread);

        connect(m_replayThread, &QThread::started, m_replayWorker, &FuseReplayWorker::start);
        connect(m_replayThread, &QThread::finished, m_replayWorker, &QObject::deleteLater);

        m_replayThread->start();
        if (m_backgroundSyncPaused || !isDriveApiAllowed()) {
            QMetaObject::invokeMethod(m_replayWorker, "pause", Qt::QueuedConnection);
        }
    }
}

void FuseDriver::stopBackgroundWorkers() {
    qDebug() << "FuseDriver: Stopping background workers";

    // Stop dirty sync worker
    if (m_dirtySyncWorker && m_dirtySyncThread && m_dirtySyncThread->isRunning()) {
        // Use QueuedConnection + thread->quit() instead of BlockingQueuedConnection
        // to avoid deadlocking if the event loop is already exiting.
        QMetaObject::invokeMethod(m_dirtySyncWorker, "stop", Qt::QueuedConnection);
    }

    if (m_dirtySyncThread) {
        m_dirtySyncThread->quit();
        if (!m_dirtySyncThread->wait(3000)) {
            qWarning() << "FuseDriver: DirtySyncWorker thread did not exit, terminating";
            m_dirtySyncThread->terminate();
            m_dirtySyncThread->wait(1000);
        }
        delete m_dirtySyncThread;
        m_dirtySyncWorker = nullptr;
        m_dirtySyncThread = nullptr;
    }

    // Stop metadata refresh worker
    if (m_metadataRefreshWorker && m_metadataRefreshThread &&
        m_metadataRefreshThread->isRunning()) {
        QMetaObject::invokeMethod(m_metadataRefreshWorker, "stop", Qt::QueuedConnection);
    }

    if (m_metadataRefreshThread) {
        m_metadataRefreshThread->quit();
        if (!m_metadataRefreshThread->wait(3000)) {
            qWarning() << "FuseDriver: MetadataRefreshWorker thread did not exit, terminating";
            m_metadataRefreshThread->terminate();
            m_metadataRefreshThread->wait(1000);
        }
        delete m_metadataRefreshThread;
        m_metadataRefreshWorker = nullptr;
        m_metadataRefreshThread = nullptr;
    }

    if (m_replayWorker && m_replayThread && m_replayThread->isRunning()) {
        QMetaObject::invokeMethod(m_replayWorker, "stop", Qt::QueuedConnection);
    }

    if (m_replayThread) {
        m_replayThread->quit();
        if (!m_replayThread->wait(3000)) {
            qWarning() << "FuseDriver: FuseReplayWorker thread did not exit, terminating";
            m_replayThread->terminate();
            m_replayThread->wait(1000);
        }
        delete m_replayThread;
        m_replayWorker = nullptr;
        m_replayThread = nullptr;
    }
}

QString FuseDriver::getParentPath(const QString& path) {
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash <= 0) {
        return "/";
    }
    return path.left(lastSlash);
}

QString FuseDriver::getFileName(const QString& path) {
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash < 0) {
        return path;
    }
    return path.mid(lastSlash + 1);
}

QString FuseDriver::normalizePath(const char* fusePath) {
    QString path = QString::fromUtf8(fusePath);
    // Ensure path starts with /
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    return path;
}

uint64_t FuseDriver::registerOpenFile(const FuseOpenFile& openFile) {
    uint64_t handle;
    {
        QMutexLocker locker(&m_openFilesMutex);
        handle = m_nextFileHandle++;
        m_openFiles[handle] = openFile;
    }
    // Fix 2: Tell FileCache that this file has an active FUSE handle so it
    // will not be evicted or invalidated while the handle is open.
    const QString contentId = contentIdentityForOpenFile(openFile);
    if (m_fileCache && !contentId.isEmpty()) {
        m_fileCache->addOpenHandle(contentId, openFile.writable);
    }
    return handle;
}

std::optional<FuseOpenFile> FuseDriver::getOpenFile(uint64_t fh) {
    QMutexLocker locker(&m_openFilesMutex);
    if (m_openFiles.contains(fh)) {
        return m_openFiles[fh];
    }
    return std::nullopt;
}

bool FuseDriver::markOpenFileDirty(uint64_t fh) {
    QMutexLocker locker(&m_openFilesMutex);
    if (m_openFiles.contains(fh)) {
        m_openFiles[fh].dirty = true;
        return true;
    }
    return false;
}

bool FuseDriver::markOpenFileClean(uint64_t fh) {
    QMutexLocker locker(&m_openFilesMutex);
    if (m_openFiles.contains(fh)) {
        m_openFiles[fh].dirty = false;
        return true;
    }
    return false;
}

void FuseDriver::unregisterOpenFile(uint64_t fh) {
    QString contentId;
    int localFd = -1;
    bool writable = false;
    {
        QMutexLocker locker(&m_openFilesMutex);
        auto it = m_openFiles.find(fh);
        if (it != m_openFiles.end()) {
            contentId = contentIdentityForOpenFile(*it);
            localFd = it->localFd;
            writable = it->writable;
            m_openFiles.erase(it);
        }
    }
    if (localFd >= 0) {
        ::close(localFd);
    }
    // Fix 2: Decrement the open-handle count so the file becomes eligible
    // for eviction and invalidation once all handles are closed.
    if (m_fileCache && !contentId.isEmpty()) {
        m_fileCache->removeOpenHandle(contentId, writable);
    }
}
