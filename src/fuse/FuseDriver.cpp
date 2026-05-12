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
#include <functional>

#include "DirtySyncWorker.h"
#include "FileCache.h"
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

bool waitForMove(GoogleDriveClient* driveClient, const QString& fileId, DriveFile* movedFile,
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

    QMetaObject::Connection movedConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    movedConn = QObject::connect(driveClient, &GoogleDriveClient::fileMovedDetailed, &loop,
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
                                     if (operation != QStringLiteral("moveFile")) {
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
        error = QStringLiteral("moveFile timeout");
        loop.quit();
    });

    if (!startRequest || !startRequest()) {
        error = QStringLiteral("Failed to dispatch moveFile request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(movedConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("moveFile failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    if (success && movedFile) {
        *movedFile = result;
    }

    return success;
}

bool waitForRename(GoogleDriveClient* driveClient, const QString& fileId, DriveFile* renamedFile,
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

    QMetaObject::Connection renamedConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    renamedConn = QObject::connect(driveClient, &GoogleDriveClient::fileRenamedDetailed, &loop,
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
                                     if (operation != QStringLiteral("renameFile")) {
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
        error = QStringLiteral("renameFile timeout");
        loop.quit();
    });

    if (!startRequest || !startRequest()) {
        error = QStringLiteral("Failed to dispatch renameFile request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(renamedConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("renameFile failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    if (success && renamedFile) {
        *renamedFile = result;
    }

    return success;
}

bool waitForMoveAndRename(GoogleDriveClient* driveClient, const QString& fileId,
                          DriveFile* resultFile, const std::function<bool()>& startRequest,
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

    QMetaObject::Connection resultConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    resultConn = QObject::connect(driveClient, &GoogleDriveClient::fileMovedAndRenamedDetailed,
                                  &loop, [&](const DriveFile& file) {
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
                                     if (operation != QStringLiteral("moveAndRenameFile")) {
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
        error = QStringLiteral("moveAndRenameFile timeout");
        loop.quit();
    });

    if (!startRequest || !startRequest()) {
        error = QStringLiteral("Failed to dispatch moveAndRenameFile request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(resultConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("moveAndRenameFile failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    if (success && resultFile) {
        *resultFile = result;
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
      m_dirtySyncWorker(nullptr),
      m_metadataRefreshWorker(nullptr),
      m_pauseController(nullptr),
      m_backgroundSyncPaused(false),
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

void FuseDriver::pauseSync() {
    m_backgroundSyncPaused = true;

    if (m_dirtySyncWorker) {
        QMetaObject::invokeMethod(m_dirtySyncWorker, "pause", Qt::QueuedConnection);
    }
    if (m_metadataRefreshWorker) {
        QMetaObject::invokeMethod(m_metadataRefreshWorker, "pause", Qt::QueuedConnection);
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

void FuseDriver::emitDriveOperationBlocked(const QString& action, const QString& path) {
    const QString message = m_pauseController
                                ? m_pauseController->blockedOperationMessage(action)
                                : QStringLiteral("Drive access is currently unavailable.");

    QMetaObject::invokeMethod(
        this,
        [this, action, path, message]() { emit driveOperationBlocked(action, path, message); },
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

    // ── Cache-first path (LOG-01 fix) ──
    // If MetadataCache has fresh children for this directory, serve them
    // directly without hitting the API.  The MetadataRefreshWorker keeps
    // the cache warm in the background, so this is safe.
    QString cacheLookupPath;
    if (qpath.isEmpty() || qpath == "/") {
        cacheLookupPath = QStringLiteral("/");
    } else {
        cacheLookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;
    }

    if (drv->m_metadataCache && drv->m_metadataCache->hasChildrenCached(cacheLookupPath)) {
        QList<FuseFileMetadata> cached = drv->m_metadataCache->getChildren(cacheLookupPath);
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

    // Get file metadata
    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

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
            openFile.fileId = meta.fileId;
            openFile.cacheKey = meta.fileId + QLatin1Char('|') + repr.outputMimeType;
            openFile.path = qpath;
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

    // Get cached file path (may trigger download)
    if (!drv->isDriveApiAllowed() && !drv->m_fileCache->hasLocalContent(meta.fileId)) {
        drv->emitDriveOperationBlocked(QStringLiteral("download uncached files"), qpath);
        return -EHOSTDOWN;
    }

    QString cachePath = drv->m_fileCache->getCachedPath(meta.fileId, meta.size);
    if (cachePath.isEmpty()) {
        return -EIO;
    }

    // Create open file handle
    FuseOpenFile openFile;
    openFile.fileId = meta.fileId;
    openFile.cacheKey = meta.fileId;
    openFile.path = qpath;
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

    if (openFile.localFd < 0) {
        return -EIO;
    }

    const ssize_t written = ::pwrite(openFile.localFd, buf, size, offset);
    if (written < 0) {
        return -EIO;
    }

    QString lookupPath = openFile.path.startsWith("/") ? openFile.path.mid(1) : openFile.path;

    // Mark file as dirty (for DirtySyncWorker to upload).  We bump the
    // FileCache generation on every successful write so an older upload can
    // never clear newer bytes written through the same handle.
    if (drv->m_fileCache && !openFile.fileId.isEmpty()) {
        drv->m_fileCache->markDirty(openFile.fileId, lookupPath);
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
            drv->stageDirtyFileForUpload(openFile.fileId, openFile.path, openFile.localFd);
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

// TODO: Implement full hardened offline support
// First, “accept dirty files while paused” is reasonable for existing cached files, but full
// offline FUSE mutations are not currently in scope. FuseDriver.cpp:1847 still performs create,
// mkdir, rename, move, trash, and delete as immediate Drive calls. If you want those to work
// offline too, that becomes a separate durable operation-journal feature with replay and conflict
// handling.

int FuseDriver::fuseMkdir(const char* path, mode_t mode) {
    Q_UNUSED(mode)
    auto* drv = self();

    QString qpath = normalizePath(path);
    QString parentPath = getParentPath(qpath);
    QString folderName = getFileName(qpath);

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->mkdirTrashOverlay(qpath);
    }

    if (!drv || !drv->m_driveClient || !drv->m_database) {
        return -EIO;
    }

    if (!drv->isDriveApiAllowed()) {
        drv->emitDriveOperationBlocked(QStringLiteral("create folders"), qpath);
        return drv->pausedMutationErrorCode();
    }

    // Get parent folder ID
    QString parentId = "root";
    if (!parentPath.isEmpty() && parentPath != "/") {
        QString lookupPath = parentPath.startsWith("/") ? parentPath.mid(1) : parentPath;
        FuseMetadata parentMeta = drv->m_database->getFuseMetadataByPath(lookupPath);
        if (!parentMeta.fileId.isEmpty()) {
            parentId = parentMeta.fileId;
        } else {
            return -ENOENT;  // Parent doesn't exist
        }
    }

    // Create folder via API
    QString requestLocalPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;
    QString error;
    DriveFile createdFolder;
    bool authFailure = false;
    if (!waitForFolderCreate(
            drv->m_driveClient, requestLocalPath,
            [&]() {
                return invokeDriveCall(drv->m_driveClient, [&]() {
                    drv->m_driveClient->createFolder(folderName, parentId, requestLocalPath);
                });
            },
            &createdFolder, &error, &authFailure)) {
        qWarning() << "FuseDriver: mkdir failed for" << qpath << ":" << error;
        return authFailure ? -EACCES : -EIO;
    }

    if (!createdFolder.isValid()) {
        return -EIO;
    }

    FuseMetadata newMeta;
    newMeta.fileId = createdFolder.id;
    newMeta.path = requestLocalPath;
    newMeta.name = folderName;
    newMeta.remoteName = folderName;
    newMeta.parentId = parentId;
    newMeta.isFolder = true;
    newMeta.size = 0;
    newMeta.mimeType = createdFolder.mimeType;
    newMeta.createdTime = createdFolder.createdTime;
    newMeta.modifiedTime = createdFolder.modifiedTime;
    newMeta.cachedAt = QDateTime::currentDateTime();
    newMeta.lastAccessed = QDateTime::currentDateTime();

    if (!drv->saveMetadataEntry(newMeta)) {
        return -EIO;
    }

    drv->invalidateFusePaths({parentPath, qpath});

    QMetaObject::invokeMethod(
        drv, [drv, qpath]() { emit drv->fuseFolderCreated(qpath); }, Qt::QueuedConnection);

    return 0;
}

int FuseDriver::fuseRmdir(const char* path) {
    auto* drv = self();
    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->rmdirTrashOverlay(qpath);
    }

    if (!drv || !drv->m_database || !drv->m_driveClient) {
        return -EIO;
    }

    if (!drv->isDriveApiAllowed()) {
        drv->emitDriveOperationBlocked(QStringLiteral("trash folders"), qpath);
        return drv->pausedMutationErrorCode();
    }

    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

    if (!meta.isFolder) {
        return -ENOTDIR;
    }

    // Check if directory is empty
    QList<FuseMetadata> children = drv->m_database->getFuseChildren(meta.fileId);
    if (!children.isEmpty()) {
        return -ENOTEMPTY;
    }

    // Trash via API (synchronous) — moves to Drive trash instead of permanent delete
    QString error;
    bool authFailure = false;
    if (!waitForTrash(
            drv->m_driveClient, meta.fileId,
            [&]() {
                return invokeDriveCall(drv->m_driveClient,
                                       [&]() { drv->m_driveClient->trashFile(meta.fileId); });
            },
            &error, &authFailure)) {
        qWarning() << "FuseDriver: rmdir trash failed for" << lookupPath << ":" << error;
        return authFailure ? -EACCES : -EIO;
    }

    // Remove from metadata only after remote trash confirmed
    if (!drv->m_database->deleteFuseMetadata(meta.fileId)) {
        return -EIO;
    }
    drv->m_database->deleteNativeDocState(meta.fileId);
    drv->removeMetadataEntryFromCache(meta);
    drv->invalidateFusePaths({getParentPath(qpath), qpath});

    QMetaObject::invokeMethod(
        drv, [drv, qpath]() { emit drv->fuseItemTrashed(qpath); }, Qt::QueuedConnection);

    return 0;
}

int FuseDriver::fuseUnlink(const char* path) {
    auto* drv = self();
    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->unlinkTrashOverlay(qpath);
    }

    if (!drv || !drv->m_database || !drv->m_driveClient) {
        return -EIO;
    }

    if (!drv->isDriveApiAllowed()) {
        drv->emitDriveOperationBlocked(QStringLiteral("trash files"), qpath);
        return drv->pausedMutationErrorCode();
    }

    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

    if (meta.isFolder) {
        return -EISDIR;
    }

    // Trash via API (synchronous) — moves to Drive trash instead of permanent delete.
    // Treat "file not found" (404) as success — the file may have already been
    // deleted from Drive by a concurrent operation (e.g. MetadataRefreshWorker
    // processing a remote deletion, or a previous unlink that raced with an
    // orphaned updateFile response).
    QString error;
    bool authFailure = false;
    if (!waitForTrash(
            drv->m_driveClient, meta.fileId,
            [&]() {
                return invokeDriveCall(drv->m_driveClient,
                                       [&]() { drv->m_driveClient->trashFile(meta.fileId); });
            },
            &error, &authFailure)) {
        // Accept "not found" as success — the remote file is already gone.
        bool notFound = error.contains(QLatin1String("not found"), Qt::CaseInsensitive) ||
                        error.contains(QLatin1String("404"));
        if (!notFound) {
            qWarning() << "FuseDriver: unlink trash failed for" << lookupPath << ":" << error;
            return authFailure ? -EACCES : -EIO;
        }
        qDebug() << "FuseDriver: unlink – file already deleted from Drive:" << lookupPath;
    }

    // Remove from cache only after remote delete confirmed
    if (drv->m_fileCache) {
        drv->m_fileCache->removeFromCache(meta.fileId);
    }

    // Remove from metadata
    if (!drv->m_database->deleteFuseMetadata(meta.fileId)) {
        return -EIO;
    }
    drv->m_database->deleteNativeDocState(meta.fileId);
    drv->removeMetadataEntryFromCache(meta);
    drv->invalidateFusePaths({getParentPath(qpath), qpath});

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

    if (!drv || !drv->m_database || !drv->m_driveClient) {
        return -EIO;
    }

    if (fromIsTrash && !toIsTrash) {
        if (!drv->isDriveApiAllowed()) {
            drv->emitDriveOperationBlocked(QStringLiteral("restore items from local trash"),
                                           toPath);
            return drv->pausedMutationErrorCode();
        }

        if (!drv->m_database->getFuseMetadataByPath(toLookup).fileId.isEmpty()) {
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
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

    if (toIsTrash) {
        if (!drv->isDriveApiAllowed()) {
            drv->emitDriveOperationBlocked(QStringLiteral("trash items"), fromPath);
            return drv->pausedMutationErrorCode();
        }

        QString error;
        if (!drv->moveLiveEntryToTrash(meta, fromPath, toPath, &error)) {
            qWarning() << "FuseDriver: move-to-trash failed" << fromPath << "->" << toPath << ":"
                       << error;
            return -EIO;
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

    if (!drv->isDriveApiAllowed()) {
        const QString action = (isMove && isRename) ? QStringLiteral("move or rename items")
                                                    : (isMove ? QStringLiteral("move items")
                                                              : QStringLiteral("rename items"));
        drv->emitDriveOperationBlocked(action, fromPath);
        return drv->pausedMutationErrorCode();
    }

    // LOG-02: When both move and rename are needed, issue a single atomic PATCH request
    if (isMove && isRename) {
        QString newParentId = "root";
        if (!newParentPath.isEmpty() && newParentPath != "/") {
            QString newParentLookup =
                newParentPath.startsWith("/") ? newParentPath.mid(1) : newParentPath;
            FuseMetadata newParentMeta = drv->m_database->getFuseMetadataByPath(newParentLookup);
            if (newParentMeta.fileId.isEmpty() || !newParentMeta.isFolder) {
                return -ENOENT;
            }
            newParentId = newParentMeta.fileId;
        }

        QString oldParentId = meta.parentId;
        if (oldParentId.isEmpty()) {
            oldParentId = "root";
        }

        QString error;
        DriveFile resultFile;
        bool authFailure = false;
        if (!waitForMoveAndRename(
                drv->m_driveClient, meta.fileId, &resultFile,
                [&]() {
                    return invokeDriveCall(drv->m_driveClient, [&]() {
                        drv->m_driveClient->moveAndRenameFile(meta.fileId, newParentId, oldParentId,
                                                              remoteNewName);
                    });
                },
                &error, &authFailure)) {
            qWarning() << "FuseDriver: move+rename failed" << fromPath << "->" << toPath << ":"
                       << error;
            return authFailure ? -EACCES : -EIO;
        }

        meta.parentId = newParentId;
        if (resultFile.modifiedTime.isValid()) {
            meta.modifiedTime = resultFile.modifiedTime;
        }
    } else if (isMove) {
        QString newParentId = "root";
        if (!newParentPath.isEmpty() && newParentPath != "/") {
            QString newParentLookup =
                newParentPath.startsWith("/") ? newParentPath.mid(1) : newParentPath;
            FuseMetadata newParentMeta = drv->m_database->getFuseMetadataByPath(newParentLookup);
            if (newParentMeta.fileId.isEmpty() || !newParentMeta.isFolder) {
                return -ENOENT;
            }
            newParentId = newParentMeta.fileId;
        }

        QString oldParentId = meta.parentId;
        if (oldParentId.isEmpty()) {
            oldParentId = "root";
        }

        QString error;
        DriveFile movedFile;
        bool authFailure = false;
        if (!waitForMove(
                drv->m_driveClient, meta.fileId, &movedFile,
                [&]() {
                    return invokeDriveCall(drv->m_driveClient, [&]() {
                        drv->m_driveClient->moveFile(meta.fileId, newParentId, oldParentId);
                    });
                },
                &error, &authFailure)) {
            qWarning() << "FuseDriver: move failed" << fromPath << "->" << toPath << ":" << error;
            return authFailure ? -EACCES : -EIO;
        }

        meta.parentId = newParentId;
        if (movedFile.modifiedTime.isValid()) {
            meta.modifiedTime = movedFile.modifiedTime;
        }
    } else if (isRename) {
        QString error;
        DriveFile renamedFile;
        bool authFailure = false;
        if (!waitForRename(
                drv->m_driveClient, meta.fileId, &renamedFile,
                [&]() {
                    return invokeDriveCall(drv->m_driveClient, [&]() {
                        drv->m_driveClient->renameFile(meta.fileId, remoteNewName);
                    });
                },
                &error, &authFailure)) {
            qWarning() << "FuseDriver: rename failed" << fromPath << "->" << toPath << ":" << error;
            return authFailure ? -EACCES : -EIO;
        }

        if (renamedFile.modifiedTime.isValid()) {
            meta.modifiedTime = renamedFile.modifiedTime;
        }
    }

    const FuseMetadata previousMeta = meta;

    // Update metadata
    meta.name = newName;
    meta.remoteName = remoteNewName;
    meta.path = toLookup;
    meta.cachedAt = QDateTime::currentDateTime();
    meta.lastAccessed = QDateTime::currentDateTime();
    if (!drv->reconcileMovedMetadata(previousMeta, meta)) {
        return -EIO;
    }

    drv->invalidateFusePaths({fromPath, toPath, oldParentPath, newParentPath});

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

    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

    // Native docs are read-only — reject truncate
    if (isNativeDoc(meta)) {
        return -EACCES;
    }

    bool openedViaHandle = false;
    FuseOpenFile openFile;
    if (fi) {
        auto openFileOpt = drv->getOpenFile(fi->fh);
        if (openFileOpt && openFileOpt->fileId == meta.fileId) {
            openFile = *openFileOpt;
            openedViaHandle = true;
        }
    }

    if (!openedViaHandle) {
        return drv->truncateWithoutHandle(meta.fileId, meta.size, qpath, size);
    }

    if (openFile.localFd < 0 || ::ftruncate(openFile.localFd, size) != 0) {
        return -EIO;
    }

    // Mark as dirty for upload
    drv->markOpenFileDirty(fi->fh);
    drv->m_fileCache->markDirty(meta.fileId, lookupPath);

    return 0;
}

int FuseDriver::fuseCreate(const char* path, mode_t mode, struct fuse_file_info* fi) {
    Q_UNUSED(mode)
    auto* drv = self();

    QString qpath = normalizePath(path);
    QString parentPath = getParentPath(qpath);
    QString fileName = getFileName(qpath);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (drv && drv->isTrashFusePath(qpath)) {
        return drv->createTrashOverlay(qpath, fi);
    }

    if (!drv || !drv->m_driveClient || !drv->m_fileCache) {
        return -EIO;
    }

    if (!drv->isDriveApiAllowed()) {
        drv->emitDriveOperationBlocked(QStringLiteral("create files"), qpath);
        return drv->pausedMutationErrorCode();
    }

    // Get parent folder ID
    QString parentId = "root";
    if (!parentPath.isEmpty() && parentPath != "/") {
        QString parentLookup = parentPath.startsWith("/") ? parentPath.mid(1) : parentPath;
        FuseMetadata parentMeta = drv->m_database->getFuseMetadataByPath(parentLookup);
        if (parentMeta.fileId.isEmpty()) {
            // GPT5.3 #5 fix: return ENOENT instead of silently falling back to root
            return -ENOENT;
        }
        parentId = parentMeta.fileId;
    }

    // Create empty file in cache
    QString tempId = QStringLiteral("temp_create_%1_%2")
                         .arg(QDateTime::currentMSecsSinceEpoch())
                         .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    QString tempCachePath = drv->m_fileCache->getCachePathForFile(tempId);
    QDir().mkpath(QFileInfo(tempCachePath).path());
    QFile tempFile(tempCachePath);
    if (!tempFile.open(QIODevice::WriteOnly)) {
        return -EIO;
    }
    tempFile.close();

    // Create remote file immediately to obtain stable fileId for dirty tracking/retries
    QString error;
    DriveFile uploadedFile;
    bool authFailure = false;
    if (!waitForUpload(
            drv->m_driveClient, tempCachePath, &uploadedFile,
            [&]() {
                return invokeDriveCall(drv->m_driveClient, [&]() {
                    drv->m_driveClient->uploadFile(tempCachePath, parentId, fileName);
                });
            },
            &error, &authFailure)) {
        QFile::remove(tempCachePath);
        qWarning() << "FuseDriver: create upload failed for" << qpath << ":" << error;
        return authFailure ? -EACCES : -EIO;
    }

    if (!uploadedFile.isValid()) {
        QFile::remove(tempCachePath);
        return -EIO;
    }

    // Rename temp cache file to canonical fileId path
    QString canonicalCachePath = drv->m_fileCache->getCachePathForFile(uploadedFile.id);
    QDir().mkpath(QFileInfo(canonicalCachePath).path());
    if (canonicalCachePath != tempCachePath) {
        QFile::remove(canonicalCachePath);
        if (!QFile::rename(tempCachePath, canonicalCachePath)) {
            canonicalCachePath = tempCachePath;
        }
    }

    drv->m_fileCache->recordCacheEntry(uploadedFile.id, canonicalCachePath,
                                       QFileInfo(canonicalCachePath).size());

    FuseMetadata newMeta;
    newMeta.fileId = uploadedFile.id;
    newMeta.path = lookupPath;
    newMeta.name = fileName;
    newMeta.remoteName = fileName;
    newMeta.parentId = parentId;
    newMeta.isFolder = false;
    newMeta.size = 0;
    newMeta.mimeType = uploadedFile.mimeType;
    newMeta.createdTime = uploadedFile.createdTime;
    newMeta.modifiedTime = uploadedFile.modifiedTime;
    newMeta.cachedAt = QDateTime::currentDateTime();
    newMeta.lastAccessed = QDateTime::currentDateTime();
    if (!drv->saveMetadataEntry(newMeta)) {
        // ROB-06: Clean up orphaned remote file since metadata save failed
        qWarning() << "FuseDriver: saveFuseMetadata failed for" << newMeta.fileId
                   << "- deleting orphaned remote file";
        QMetaObject::invokeMethod(
            drv->m_driveClient,
            [fileId = newMeta.fileId, drv]() { drv->m_driveClient->deleteFile(fileId); },
            Qt::QueuedConnection);
        return -EIO;
    }
    drv->invalidateFusePaths({parentPath, qpath});

    FuseOpenFile openFile;
    openFile.fileId = uploadedFile.id;
    openFile.path = qpath;
    openFile.size = 0;
    openFile.writable = true;
    openFile.dirty = false;
    openFile.synthetic = false;
    openFile.localFd = openLocalHandle(canonicalCachePath, true);
    if (openFile.localFd < 0) {
        return -EIO;
    }

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
        m_metadataCache->setRootFolderId("root");
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
    if (!m_driveClient || !m_database || !m_fileCache) {
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

bool FuseDriver::stageDirtyFileForUpload(const QString& fileId, const QString& path, int localFd) {
    if (fileId.isEmpty() || !m_fileCache) {
        return false;
    }

    if (localFd >= 0) {
        ::fsync(localFd);
    }

    QString pendingPath = m_fileCache->moveToDirtyStore(fileId);
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
            openFile.size = sizeForOpenHandle(newFd);
        }
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
        if (!stageDirtyFileForUpload(fileId, path, localFd)) {
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
        connect(m_metadataRefreshWorker, &MetadataRefreshWorker::refreshCompleted, this,
                [this](int) { emit metadataRefreshed(); });
        connect(m_metadataRefreshWorker, &MetadataRefreshWorker::error, this,
                &FuseDriver::metadataRefreshFailed);

        m_metadataRefreshThread->start();
        if (m_backgroundSyncPaused || !isDriveApiAllowed()) {
            QMetaObject::invokeMethod(m_metadataRefreshWorker, "pause", Qt::QueuedConnection);
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
    if (m_fileCache && !openFile.fileId.isEmpty()) {
        m_fileCache->addOpenHandle(openFile.fileId, openFile.writable);
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
    QString fileId;
    int localFd = -1;
    bool writable = false;
    {
        QMutexLocker locker(&m_openFilesMutex);
        auto it = m_openFiles.find(fh);
        if (it != m_openFiles.end()) {
            fileId = it->fileId;
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
    if (m_fileCache && !fileId.isEmpty()) {
        m_fileCache->removeOpenHandle(fileId, writable);
    }
}
