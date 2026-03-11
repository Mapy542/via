/**
 * @file FuseDriver.cpp
 * @brief Implementation of FUSE virtual filesystem driver
 *
 * Implements the FUSE callbacks and coordinates between components as defined
 * in the FUSE Procedure Flow Chart (see src/sync/dataflow.md).
 */

#include "FuseDriver.h"

#include <errno.h>
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
#include <QStandardPaths>
#include <QTimer>
#include <functional>

#include "DirtySyncWorker.h"
#include "FileCache.h"
#include "MetadataCache.h"
#include "MetadataRefreshWorker.h"
#include "api/GoogleDriveClient.h"
#include "sync/SyncDatabase.h"

namespace {

/// Recover the FuseDriver instance from FUSE's per-mount private_data.
static inline FuseDriver* self() { return static_cast<FuseDriver*>(fuse_get_context()->private_data); }

constexpr int FUSE_API_TIMEOUT_MS = 30000;

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
    return lowered.contains(QStringLiteral("authentication")) || lowered.contains(QStringLiteral("auth")) ||
           lowered.contains(QStringLiteral("permission")) || lowered.contains(QStringLiteral("credential")) ||
           lowered.contains(QStringLiteral("unauthorized"));
}

bool waitForFolderCreate(GoogleDriveClient* driveClient, const QString& requestLocalPath,
                         const std::function<bool()>& startRequest, DriveFile* createdFolder, QString* errorOut,
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
                                 [&](const QString& operation, const QString& errorMsg, int httpStatus, const QString&,
                                     const QString& localPath) {
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

bool waitForUpload(GoogleDriveClient* driveClient, const QString& localPath, DriveFile* uploadedFile,
                   const std::function<bool()>& startRequest, QString* errorOut, bool* authFailureOut = nullptr) {
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

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg, int httpStatus, const QString&,
                                     const QString& errorLocalPath) {
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
                   const std::function<bool()>& startRequest, QString* errorOut, bool* authFailureOut = nullptr) {
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

    updatedConn = QObject::connect(driveClient, &GoogleDriveClient::fileUpdated, &loop, [&](const DriveFile& file) {
        if (file.id != fileId) {
            return;
        }
        success = file.isValid();
        result = file;
        loop.quit();
    });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg, int httpStatus,
                                     const QString& errorFileId, const QString&) {
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
                 const std::function<bool()>& startRequest, QString* errorOut, bool* authFailureOut = nullptr) {
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

    movedConn = QObject::connect(driveClient, &GoogleDriveClient::fileMovedDetailed, &loop, [&](const DriveFile& file) {
        if (file.id != fileId) {
            return;
        }
        success = file.isValid();
        result = file;
        loop.quit();
    });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg, int httpStatus,
                                     const QString& errorFileId, const QString&) {
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
                   const std::function<bool()>& startRequest, QString* errorOut, bool* authFailureOut = nullptr) {
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

    renamedConn =
        QObject::connect(driveClient, &GoogleDriveClient::fileRenamedDetailed, &loop, [&](const DriveFile& file) {
            if (file.id != fileId) {
                return;
            }
            success = file.isValid();
            result = file;
            loop.quit();
        });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg, int httpStatus,
                                     const QString& errorFileId, const QString&) {
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

bool waitForMoveAndRename(GoogleDriveClient* driveClient, const QString& fileId, DriveFile* resultFile,
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

    QMetaObject::Connection resultConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    resultConn = QObject::connect(driveClient, &GoogleDriveClient::fileMovedAndRenamedDetailed, &loop,
                                  [&](const DriveFile& file) {
                                      if (file.id != fileId) {
                                          return;
                                      }
                                      success = file.isValid();
                                      result = file;
                                      loop.quit();
                                  });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg, int httpStatus,
                                     const QString& errorFileId, const QString&) {
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

bool waitForDelete(GoogleDriveClient* driveClient, const QString& fileId, const std::function<bool()>& startRequest,
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

    QMetaObject::Connection deletedConn;
    QMetaObject::Connection errorConn;
    QMetaObject::Connection timeoutConn;

    deletedConn = QObject::connect(driveClient, &GoogleDriveClient::fileDeleted, &loop, [&](const QString& deletedId) {
        if (deletedId != fileId) {
            return;
        }
        success = true;
        loop.quit();
    });

    errorConn = QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                                 [&](const QString& operation, const QString& errorMsg, int httpStatus,
                                     const QString& errorFileId, const QString&) {
                                     if (operation != QStringLiteral("deleteFile")) {
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
        error = QStringLiteral("deleteFile timeout");
        loop.quit();
    });

    if (!startRequest || !startRequest()) {
        error = QStringLiteral("Failed to dispatch deleteFile request");
    } else {
        timeout.start(FUSE_API_TIMEOUT_MS);
        loop.exec();
    }

    QObject::disconnect(deletedConn);
    QObject::disconnect(errorConn);
    QObject::disconnect(timeoutConn);

    if (!success && errorOut) {
        *errorOut = error.isEmpty() ? QStringLiteral("deleteFile failed") : error;
    }

    if (authFailureOut) {
        *authFailureOut = isAuthOrPermissionFailure(errorStatus, error);
    }

    return success;
}

bool waitForListFiles(GoogleDriveClient* driveClient, const QString& parentId, QList<DriveFile>* resultFiles,
                      QString* errorOut) {
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

    listedConn = QObject::connect(driveClient, &GoogleDriveClient::filesListed, &loop,
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

    errorConn =
        QObject::connect(driveClient, &GoogleDriveClient::errorDetailed, &loop,
                         [&](const QString& operation, const QString& errorMsg, int, const QString&, const QString&) {
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

    if (!invokeDriveCall(driveClient, [driveClient, parentId]() { driveClient->listFiles(parentId); })) {
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

QString FuseDriver::mountPoint() const { return m_mountPoint; }

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

bool FuseDriver::isMounted() const { return m_mounted; }

FileCache* FuseDriver::fileCache() const { return m_fileCache; }

SyncDatabase* FuseDriver::database() const { return m_database; }

GoogleDriveClient* FuseDriver::driveClient() const { return m_driveClient; }

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
                    qDebug() << "FuseDriver: Found existing mount in /proc/mounts:" << line.trimmed();
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
                    emit mountError("Mount point has an existing FUSE mount that could not be cleaned up: " +
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
                qWarning() << "FuseDriver: Running inside Snap confinement (" << profile
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

    qDebug() << "FuseDriver: Refreshing metadata from remote";

    if (m_metadataRefreshWorker) {
        QMetaObject::invokeMethod(m_metadataRefreshWorker, "checkNow", Qt::QueuedConnection);
    }

    emit metadataRefreshed();
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
        QString cachePath = m_fileCache->getCachePathForFile(entry.fileId);
        if (QFile::exists(cachePath) && m_driveClient) {
            QString error;
            DriveFile updated;
            bool authFailure = false;
            if (waitForUpdate(
                    m_driveClient, entry.fileId, &updated,
                    [&]() {
                        return invokeDriveCall(m_driveClient,
                                               [&]() { m_driveClient->updateFile(entry.fileId, cachePath); });
                    },
                    &error, &authFailure)) {
                m_fileCache->clearDirty(entry.fileId);
                uploadedCount++;
            } else {
                m_fileCache->markUploadFailed(entry.fileId);
                qWarning() << "FuseDriver: Unmount flush upload failed for" << entry.path << ":" << error
                           << "authFailure=" << authFailure;
            }
        }
    }

    qInfo() << "FuseDriver: Flushed" << uploadedCount << "files";
    emit dirtyFilesFlushed(uploadedCount);
}

// ============================================================================
// FUSE Callback Implementations
// ============================================================================

int FuseDriver::fuseGetattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    Q_UNUSED(fi)
    auto* drv = self();

    memset(stbuf, 0, sizeof(struct stat));

    QString qpath = normalizePath(path);

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
                parentId = drv->m_metadataCache ? drv->m_metadataCache->rootFolderId() : QStringLiteral("root");
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

                    for (const DriveFile& file : apiFiles) {
                        if (file.isGoogleDoc() && !file.isFolder && !file.isShortcut) {
                            continue;
                        }

                        FuseMetadata newMeta;
                        newMeta.fileId = file.id;
                        newMeta.name = file.name;
                        newMeta.parentId = file.parentId();
                        newMeta.isFolder = file.isFolder;
                        newMeta.size = file.size;
                        newMeta.mimeType = file.mimeType;
                        newMeta.createdTime = file.createdTime;
                        newMeta.modifiedTime = file.modifiedTime;
                        newMeta.cachedAt = QDateTime::currentDateTime();
                        newMeta.lastAccessed = QDateTime::currentDateTime();

                        if (parentPath.isEmpty()) {
                            newMeta.path = file.name;
                        } else {
                            newMeta.path = parentPath + "/" + file.name;
                        }

                        drv->m_database->saveFuseMetadata(newMeta);

                        if (drv->m_metadataCache) {
                            FuseFileMetadata cacheMeta;
                            cacheMeta.fileId = newMeta.fileId;
                            cacheMeta.path = newMeta.path;
                            cacheMeta.name = newMeta.name;
                            cacheMeta.parentId = newMeta.parentId;
                            cacheMeta.isFolder = newMeta.isFolder;
                            cacheMeta.size = newMeta.size;
                            cacheMeta.mimeType = newMeta.mimeType;
                            cacheMeta.createdTime = newMeta.createdTime;
                            cacheMeta.modifiedTime = newMeta.modifiedTime;
                            cacheMeta.cachedAt = newMeta.cachedAt;
                            cacheMeta.lastAccessed = newMeta.lastAccessed;
                            drv->m_metadataCache->setMetadata(cacheMeta);
                        }
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
            } else {
                stbuf->st_mode = S_IFREG | 0644;
                stbuf->st_nlink = 1;
                stbuf->st_size = meta.size;
            }

            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
            stbuf->st_mtime = meta.modifiedTime.toSecsSinceEpoch();
            stbuf->st_atime = meta.lastAccessed.isValid() ? meta.lastAccessed.toSecsSinceEpoch() : stbuf->st_mtime;
            stbuf->st_ctime = meta.createdTime.isValid() ? meta.createdTime.toSecsSinceEpoch() : stbuf->st_mtime;

            return 0;
        }
    }

    return -ENOENT;
}

int FuseDriver::fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    Q_UNUSED(offset)
    Q_UNUSED(fi)
    Q_UNUSED(flags)
    auto* drv = self();

    QString qpath = normalizePath(path);

    // Add . and .. entries
    filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);

    if (!drv || !drv->m_database || !drv->m_driveClient) {
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
        qDebug() << "FuseDriver::readdir: Serving" << cached.size() << "entries from cache for" << qpath;
        for (const FuseFileMetadata& child : cached) {
            filler(buf, child.name.toUtf8().constData(), nullptr, 0, FUSE_FILL_DIR_PLUS);
        }
        return 0;
    }

    // ── Fallback: fetch from API ─────────────────────────────────────
    // Get parent folder ID
    QString parentId;
    if (qpath.isEmpty() || qpath == "/") {
        parentId = drv->m_metadataCache ? drv->m_metadataCache->rootFolderId() : QStringLiteral("root");
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

    qDebug() << "FuseDriver::readdir: Cache miss — fetching children for" << qpath << "(parentId=" << parentId
             << ") from API...";

    QList<DriveFile> apiFiles;
    QString listError;
    if (!waitForListFiles(drv->m_driveClient, parentId, &apiFiles, &listError)) {
        qWarning() << "FuseDriver::readdir: API fetch failed:" << listError;
        return -EIO;
    }

    qDebug() << "FuseDriver::readdir: Fetched" << apiFiles.size() << "files from API";

    // Build the definitive children list from API results, saving to DB/cache
    QList<FuseMetadata> children;
    for (const DriveFile& file : apiFiles) {
        // Skip Google Docs (non-downloadable) but keep folders
        if (file.isGoogleDoc() && !file.isFolder && !file.isShortcut) {
            continue;
        }

        FuseMetadata meta;
        meta.fileId = file.id;
        meta.name = file.name;
        meta.parentId = file.parentId();
        meta.isFolder = file.isFolder;
        meta.size = file.size;
        meta.mimeType = file.mimeType;
        meta.createdTime = file.createdTime;
        meta.modifiedTime = file.modifiedTime;
        meta.cachedAt = QDateTime::currentDateTime();
        meta.lastAccessed = QDateTime::currentDateTime();

        // Build path: parent path + "/" + name
        if (qpath.isEmpty() || qpath == "/") {
            meta.path = file.name;
        } else {
            QString parentPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;
            meta.path = parentPath + "/" + file.name;
        }

        drv->m_database->saveFuseMetadata(meta);
        children.append(meta);

        // Also update MetadataCache in-memory if available
        if (drv->m_metadataCache) {
            FuseFileMetadata cacheMeta;
            cacheMeta.fileId = meta.fileId;
            cacheMeta.path = meta.path;
            cacheMeta.name = meta.name;
            cacheMeta.parentId = meta.parentId;
            cacheMeta.isFolder = meta.isFolder;
            cacheMeta.size = meta.size;
            cacheMeta.mimeType = meta.mimeType;
            cacheMeta.createdTime = meta.createdTime;
            cacheMeta.modifiedTime = meta.modifiedTime;
            cacheMeta.cachedAt = meta.cachedAt;
            cacheMeta.lastAccessed = meta.lastAccessed;
            drv->m_metadataCache->setMetadata(cacheMeta);
        }
    }

    for (const FuseMetadata& child : children) {
        filler(buf, child.name.toUtf8().constData(), nullptr, 0, FUSE_FILL_DIR_PLUS);
    }

    return 0;
}

int FuseDriver::fuseOpen(const char* path, struct fuse_file_info* fi) {
    auto* drv = self();
    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

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

    // Get cached file path (may trigger download)
    QString cachePath = drv->m_fileCache->getCachedPath(meta.fileId, meta.size);
    if (cachePath.isEmpty()) {
        return -EIO;
    }

    // Create open file handle
    FuseOpenFile openFile;
    openFile.fileId = meta.fileId;
    openFile.path = qpath;
    openFile.cachePath = cachePath;
    openFile.size = meta.size;
    openFile.writable = (fi->flags & O_WRONLY) || (fi->flags & O_RDWR);
    openFile.dirty = false;

    fi->fh = drv->registerOpenFile(openFile);

    if (drv) {
        emit drv->fileAccessed(qpath);
    }

    return 0;
}

int FuseDriver::fuseRead(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
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

    // Update access time
    if (drv->m_fileCache) {
        drv->m_fileCache->updateAccessTime(openFile.fileId);
    }

    // Read from cached file
    QFile file(openFile.cachePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return -EIO;
    }

    if (!file.seek(offset)) {
        return -EIO;
    }

    QByteArray data = file.read(size);
    memcpy(buf, data.constData(), data.size());

    return data.size();
}

int FuseDriver::fuseWrite(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
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

    // Write to cached file
    QFile file(openFile.cachePath);
    if (!file.open(QIODevice::ReadWrite)) {
        return -EIO;
    }

    if (!file.seek(offset)) {
        return -EIO;
    }

    qint64 written = file.write(buf, size);
    file.close();

    if (written < 0) {
        return -EIO;
    }

    // Mark file as dirty (for DirtySyncWorker to upload)
    if (!openFile.dirty) {
        drv->markOpenFileDirty(fi->fh);
        if (drv->m_fileCache) {
            QString lookupPath = openFile.path.startsWith("/") ? openFile.path.mid(1) : openFile.path;
            drv->m_fileCache->markDirty(openFile.fileId, lookupPath);
        }
        emit drv->fileModified(openFile.path);
    }

    return written;
}

int FuseDriver::fuseRelease(const char* path, struct fuse_file_info* fi) {
    Q_UNUSED(path)
    auto* drv = self();

    if (drv) {
        auto openFileOpt = drv->getOpenFile(fi->fh);

        if (openFileOpt && openFileOpt->dirty && !openFileOpt->fileId.isEmpty()) {
            FuseOpenFile openFile = *openFileOpt;

            // Optimistic local metadata update so stat() returns
            // a reasonable size/mtime immediately after close.
            if (drv->m_database) {
                QString lookupPath = openFile.path.startsWith("/") ? openFile.path.mid(1) : openFile.path;
                FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
                if (!meta.fileId.isEmpty()) {
                    meta.size = QFileInfo(openFile.cachePath).size();
                    meta.modifiedTime = QDateTime::currentDateTime();
                    meta.lastAccessed = QDateTime::currentDateTime();
                    drv->m_database->saveFuseMetadata(meta);
                }
            }

            // Kick the DirtySyncWorker so the upload starts promptly
            // instead of waiting for the next timer tick.
            if (drv->m_dirtySyncWorker) {
                QMetaObject::invokeMethod(drv->m_dirtySyncWorker, "syncNow", Qt::QueuedConnection);
            }

            qDebug() << "FuseDriver: release – deferred upload for" << openFile.path;
        }

        drv->unregisterOpenFile(fi->fh);
    }

    return 0;
}

int FuseDriver::fuseFsync(const char* path, int datasync, struct fuse_file_info* fi) {
    Q_UNUSED(path)
    Q_UNUSED(datasync)
    auto* drv = self();

    if (!drv) {
        return -EIO;
    }

    auto openFileOpt = drv->getOpenFile(fi->fh);
    if (!openFileOpt) {
        return -EBADF;
    }

    // Nothing to sync if the file wasn't modified.
    if (!openFileOpt->dirty || openFileOpt->fileId.isEmpty()) {
        return 0;
    }

    if (!drv->m_driveClient) {
        return -EIO;
    }

    FuseOpenFile openFile = *openFileOpt;
    QString error;
    DriveFile updatedFile;
    bool authFailure = false;

    // Synchronous upload – the caller explicitly asked for data persistence.
    if (waitForUpdate(
            drv->m_driveClient, openFile.fileId, &updatedFile,
            [&]() {
                return invokeDriveCall(drv->m_driveClient,
                                       [&]() { drv->m_driveClient->updateFile(openFile.fileId, openFile.cachePath); });
            },
            &error, &authFailure)) {
        // Upload succeeded – clear dirty state.
        if (drv->m_fileCache) {
            drv->m_fileCache->clearDirty(openFile.fileId);
        }
        drv->markOpenFileClean(fi->fh);

        if (drv->m_database) {
            QString lookupPath = openFile.path.startsWith("/") ? openFile.path.mid(1) : openFile.path;
            FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
            if (!meta.fileId.isEmpty()) {
                meta.size = QFileInfo(openFile.cachePath).size();
                if (updatedFile.modifiedTime.isValid()) {
                    meta.modifiedTime = updatedFile.modifiedTime;
                }
                meta.lastAccessed = QDateTime::currentDateTime();
                meta.cachedAt = QDateTime::currentDateTime();
                drv->m_database->saveFuseMetadata(meta);
            }
        }
        return 0;
    }

    qWarning() << "FuseDriver: fsync upload failed for" << openFile.path << ":" << error
               << "authFailure=" << authFailure;
    return -EIO;
}

int FuseDriver::fuseMkdir(const char* path, mode_t mode) {
    Q_UNUSED(mode)
    auto* drv = self();

    QString qpath = normalizePath(path);
    QString parentPath = getParentPath(qpath);
    QString folderName = getFileName(qpath);

    if (!drv || !drv->m_driveClient || !drv->m_database) {
        return -EIO;
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
    newMeta.parentId = parentId;
    newMeta.isFolder = true;
    newMeta.size = 0;
    newMeta.mimeType = createdFolder.mimeType;
    newMeta.createdTime = createdFolder.createdTime;
    newMeta.modifiedTime = createdFolder.modifiedTime;
    newMeta.cachedAt = QDateTime::currentDateTime();
    newMeta.lastAccessed = QDateTime::currentDateTime();

    if (!drv->m_database->saveFuseMetadata(newMeta)) {
        return -EIO;
    }

    return 0;
}

int FuseDriver::fuseRmdir(const char* path) {
    auto* drv = self();
    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (!drv || !drv->m_database || !drv->m_driveClient) {
        return -EIO;
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

    // Delete via API (synchronous — H3 fix)
    QString error;
    bool authFailure = false;
    if (!waitForDelete(
            drv->m_driveClient, meta.fileId,
            [&]() {
                return invokeDriveCall(drv->m_driveClient, [&]() { drv->m_driveClient->deleteFile(meta.fileId); });
            },
            &error, &authFailure)) {
        qWarning() << "FuseDriver: rmdir delete failed for" << lookupPath << ":" << error;
        return authFailure ? -EACCES : -EIO;
    }

    // Remove from metadata only after remote delete confirmed
    drv->m_database->deleteFuseMetadata(meta.fileId);

    return 0;
}

int FuseDriver::fuseUnlink(const char* path) {
    auto* drv = self();
    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (!drv || !drv->m_database || !drv->m_driveClient) {
        return -EIO;
    }

    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

    if (meta.isFolder) {
        return -EISDIR;
    }

    // Delete via API (synchronous — H3 fix)
    QString error;
    bool authFailure = false;
    if (!waitForDelete(
            drv->m_driveClient, meta.fileId,
            [&]() {
                return invokeDriveCall(drv->m_driveClient, [&]() { drv->m_driveClient->deleteFile(meta.fileId); });
            },
            &error, &authFailure)) {
        qWarning() << "FuseDriver: unlink delete failed for" << lookupPath << ":" << error;
        return authFailure ? -EACCES : -EIO;
    }

    // Remove from cache only after remote delete confirmed
    if (drv->m_fileCache) {
        drv->m_fileCache->removeFromCache(meta.fileId);
    }

    // Remove from metadata
    drv->m_database->deleteFuseMetadata(meta.fileId);

    return 0;
}

int FuseDriver::fuseRename(const char* from, const char* to, unsigned int flags) {
    Q_UNUSED(flags)
    auto* drv = self();

    QString fromPath = normalizePath(from);
    QString toPath = normalizePath(to);
    QString fromLookup = fromPath.startsWith("/") ? fromPath.mid(1) : fromPath;
    QString toLookup = toPath.startsWith("/") ? toPath.mid(1) : toPath;

    if (!drv || !drv->m_database || !drv->m_driveClient) {
        return -EIO;
    }

    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(fromLookup);
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

    QString oldName = getFileName(fromPath);
    QString newName = getFileName(toPath);
    QString oldParentPath = getParentPath(fromPath);
    QString newParentPath = getParentPath(toPath);

    bool isRename = (oldName != newName);
    bool isMove = (oldParentPath != newParentPath);

    // LOG-02: When both move and rename are needed, issue a single atomic PATCH request
    if (isMove && isRename) {
        QString newParentId = "root";
        if (!newParentPath.isEmpty() && newParentPath != "/") {
            QString newParentLookup = newParentPath.startsWith("/") ? newParentPath.mid(1) : newParentPath;
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
                        drv->m_driveClient->moveAndRenameFile(meta.fileId, newParentId, oldParentId, newName);
                    });
                },
                &error, &authFailure)) {
            qWarning() << "FuseDriver: move+rename failed" << fromPath << "->" << toPath << ":" << error;
            return authFailure ? -EACCES : -EIO;
        }

        meta.parentId = newParentId;
        if (resultFile.modifiedTime.isValid()) {
            meta.modifiedTime = resultFile.modifiedTime;
        }
    } else if (isMove) {
        QString newParentId = "root";
        if (!newParentPath.isEmpty() && newParentPath != "/") {
            QString newParentLookup = newParentPath.startsWith("/") ? newParentPath.mid(1) : newParentPath;
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
                    return invokeDriveCall(drv->m_driveClient,
                                           [&]() { drv->m_driveClient->renameFile(meta.fileId, newName); });
                },
                &error, &authFailure)) {
            qWarning() << "FuseDriver: rename failed" << fromPath << "->" << toPath << ":" << error;
            return authFailure ? -EACCES : -EIO;
        }

        if (renamedFile.modifiedTime.isValid()) {
            meta.modifiedTime = renamedFile.modifiedTime;
        }
    }

    // Update metadata
    meta.name = newName;
    meta.path = toLookup;
    meta.cachedAt = QDateTime::currentDateTime();
    meta.lastAccessed = QDateTime::currentDateTime();
    drv->m_database->saveFuseMetadata(meta);

    // H2 fix: recursively update paths of all descendants
    if (meta.isFolder) {
        drv->m_database->updateFuseChildrenPaths(meta.fileId, fromLookup, toLookup);
    }

    return 0;
}

int FuseDriver::fuseTruncate(const char* path, off_t size, struct fuse_file_info* fi) {
    Q_UNUSED(fi)
    auto* drv = self();

    QString qpath = normalizePath(path);
    QString lookupPath = qpath.startsWith("/") ? qpath.mid(1) : qpath;

    if (!drv || !drv->m_database || !drv->m_fileCache) {
        return -EIO;
    }

    FuseMetadata meta = drv->m_database->getFuseMetadataByPath(lookupPath);
    if (meta.fileId.isEmpty()) {
        return -ENOENT;
    }

    // M2 fix: Download file if not cached, so truncate works correctly
    QString cachePath = drv->m_fileCache->getCachedPath(meta.fileId, meta.size);
    if (cachePath.isEmpty()) {
        return -EIO;
    }

    // Truncate cached file
    QFile file(cachePath);
    if (file.open(QIODevice::ReadWrite)) {
        file.resize(size);
        file.close();
    } else {
        return -EIO;
    }

    // Mark as dirty for upload
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

    if (!drv || !drv->m_driveClient || !drv->m_fileCache) {
        return -EIO;
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
                return invokeDriveCall(drv->m_driveClient,
                                       [&]() { drv->m_driveClient->uploadFile(tempCachePath, parentId, fileName); });
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

    drv->m_fileCache->recordCacheEntry(uploadedFile.id, canonicalCachePath, QFileInfo(canonicalCachePath).size());

    FuseMetadata newMeta;
    newMeta.fileId = uploadedFile.id;
    newMeta.path = lookupPath;
    newMeta.name = fileName;
    newMeta.parentId = parentId;
    newMeta.isFolder = false;
    newMeta.size = 0;
    newMeta.mimeType = uploadedFile.mimeType;
    newMeta.createdTime = uploadedFile.createdTime;
    newMeta.modifiedTime = uploadedFile.modifiedTime;
    newMeta.cachedAt = QDateTime::currentDateTime();
    newMeta.lastAccessed = QDateTime::currentDateTime();
    if (!drv->m_database->saveFuseMetadata(newMeta)) {
        // ROB-06: Clean up orphaned remote file since metadata save failed
        qWarning() << "FuseDriver: saveFuseMetadata failed for" << newMeta.fileId << "- deleting orphaned remote file";
        QMetaObject::invokeMethod(
            drv->m_driveClient, [fileId = newMeta.fileId, drv]() { drv->m_driveClient->deleteFile(fileId); },
            Qt::QueuedConnection);
        return -EIO;
    }
    FuseOpenFile openFile;
    openFile.fileId = uploadedFile.id;
    openFile.path = qpath;
    openFile.cachePath = canonicalCachePath;
    openFile.size = 0;
    openFile.writable = true;
    openFile.dirty = false;

    fi->fh = drv->registerOpenFile(openFile);

    return 0;
}

// --- M5: Missing FUSE operations ---

int FuseDriver::fuseStatfs(const char* path, struct statvfs* stbuf) {
    Q_UNUSED(path)
    auto* drv = self();
    if (!drv) return -EIO;

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

int FuseDriver::fuseUtimens(const char* path, const struct timespec tv[2], struct fuse_file_info* fi) {
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

void FuseDriver::startBackgroundWorkers() {
    qDebug() << "FuseDriver: Starting background workers";

    if (m_fileCache && m_driveClient && m_database && !m_dirtySyncThread && !m_dirtySyncWorker) {
        m_dirtySyncThread = new QThread(this);
        m_dirtySyncWorker = new DirtySyncWorker(m_fileCache, m_driveClient, m_database);
        m_dirtySyncWorker->moveToThread(m_dirtySyncThread);

        connect(m_dirtySyncThread, &QThread::started, m_dirtySyncWorker, &DirtySyncWorker::start);
        connect(m_dirtySyncThread, &QThread::finished, m_dirtySyncWorker, &QObject::deleteLater);

        m_dirtySyncThread->start();
    }

    if (m_metadataCache && m_fileCache && m_database && m_driveClient && !m_metadataRefreshThread &&
        !m_metadataRefreshWorker) {
        m_metadataRefreshThread = new QThread(this);
        m_metadataRefreshWorker = new MetadataRefreshWorker(m_metadataCache, m_fileCache, m_database, m_driveClient);
        m_metadataRefreshWorker->moveToThread(m_metadataRefreshThread);

        connect(m_metadataRefreshThread, &QThread::started, m_metadataRefreshWorker, &MetadataRefreshWorker::start);
        connect(m_metadataRefreshThread, &QThread::finished, m_metadataRefreshWorker, &QObject::deleteLater);

        m_metadataRefreshThread->start();
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
    if (m_metadataRefreshWorker && m_metadataRefreshThread && m_metadataRefreshThread->isRunning()) {
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
    QMutexLocker locker(&m_openFilesMutex);
    uint64_t handle = m_nextFileHandle++;
    m_openFiles[handle] = openFile;
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
    QMutexLocker locker(&m_openFilesMutex);
    m_openFiles.remove(fh);
}
