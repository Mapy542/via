/**
 * @file GoogleDriveClient.cpp
 * @brief Implementation of Google Drive REST API client
 */

#include "GoogleDriveClient.h"

#include <QDebug>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrlQuery>

#include "auth/GoogleAuthManager.h"

const QString GoogleDriveClient::API_BASE_URL = "https://www.googleapis.com/drive/v3";
const QString GoogleDriveClient::UPLOAD_URL = "https://www.googleapis.com/upload/drive/v3";

namespace {
void tagReply(QNetworkReply* reply, const QString& fileId, const QString& localPath) {
    if (!fileId.isEmpty()) {
        reply->setProperty("fileId", fileId);
    }
    if (!localPath.isEmpty()) {
        reply->setProperty("localPath", localPath);
    }
}
}  // namespace

GoogleDriveClient::GoogleDriveClient(GoogleAuthManager* authManager, QObject* parent)
    : QObject(parent),
      m_authManager(authManager),
      m_networkManager(new QNetworkAccessManager(this)) {}

GoogleDriveClient::~GoogleDriveClient() = default;

QNetworkRequest GoogleDriveClient::createRequest(const QUrl& url) const {
    QNetworkRequest request(url);

    if (m_authManager && m_authManager->isAuthenticated()) {
        QString bearer = "Bearer " + m_authManager->accessToken();
        request.setRawHeader("Authorization", bearer.toUtf8());
    }

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    return request;
}

void GoogleDriveClient::handleNetworkError(QNetworkReply* reply, const QString& operation) {
    QString errorMsg;

    if (reply->error() != QNetworkReply::NoError) {
        // Try to parse error from response body
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);

        if (doc.isObject()) {
            QJsonObject error = doc.object()["error"].toObject();
            errorMsg = error["message"].toString();

            if (errorMsg.isEmpty()) {
                errorMsg = reply->errorString();
            }
        } else {
            errorMsg = reply->errorString();
        }
    }

    if (!errorMsg.isEmpty()) {
        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString fileId = reply->property("fileId").toString();
        QString localPath = reply->property("localPath").toString();

        qWarning() << operation << "error:" << errorMsg;
        emit error(operation, errorMsg);
        emit errorDetailed(operation, errorMsg, httpStatus, fileId, localPath);

        const QString lowered = errorMsg.toLower();
        const bool authLike403 =
            (httpStatus == 403 &&
             (lowered.contains("auth") || lowered.contains("token") ||
              lowered.contains("permission") || lowered.contains("credential")));
        if (httpStatus == 401 || authLike403) {
            emit authenticationFailure(operation, httpStatus, errorMsg);
        }
    }
}

DriveFile GoogleDriveClient::parseFileJson(const QJsonObject& json) const {
    DriveFile file;

    file.id = json["id"].toString();
    file.name = json["name"].toString();
    file.mimeType = json["mimeType"].toString();
    file.size = json["size"].toString().toLongLong();
    file.createdTime = QDateTime::fromString(json["createdTime"].toString(), Qt::ISODate);
    file.modifiedTime = QDateTime::fromString(json["modifiedTime"].toString(), Qt::ISODate);
    file.md5Checksum = json["md5Checksum"].toString();
    file.trashed = json["trashed"].toBool();
    file.starred = json["starred"].toBool();
    file.shared = json["shared"].toBool();
    file.ownedByMe = json["ownedByMe"].toBool(true);  // Default to true if not present
    file.webViewLink = json["webViewLink"].toString();
    file.webContentLink = json["webContentLink"].toString();
    file.iconLink = json["iconLink"].toString();

    // Parse parents
    QJsonArray parents = json["parents"].toArray();
    for (const QJsonValue& parent : parents) {
        file.parents.append(parent.toString());
    }

    // Check if folder
    file.isFolder = (file.mimeType == "application/vnd.google-apps.folder");

    // Check if shortcut and parse shortcut details
    file.isShortcut = (file.mimeType == "application/vnd.google-apps.shortcut");
    if (file.isShortcut && json.contains("shortcutDetails")) {
        QJsonObject shortcutDetails = json["shortcutDetails"].toObject();
        file.shortcutTargetId = shortcutDetails["targetId"].toString();
        file.shortcutTargetMimeType = shortcutDetails["targetMimeType"].toString();
    }

    return file;
}

DriveChange GoogleDriveClient::parseChangeJson(const QJsonObject& json) const {
    DriveChange change;

    change.fileId = json["fileId"].toString();
    // Use fileId as changeId since Drive API v3 doesn't provide a separate change ID
    change.changeId = change.fileId;
    change.time = QDateTime::fromString(json["time"].toString(), Qt::ISODate);
    change.removed = json["removed"].toBool();

    if (json.contains("file") && !change.removed) {
        change.file = parseFileJson(json["file"].toObject());
    }

    return change;
}

void GoogleDriveClient::listFiles(const QString& folderId, const QString& pageToken) {
    QUrl url(API_BASE_URL + "/files");
    QUrlQuery query;

    // Query parameters
    if (folderId.isEmpty() || folderId == "all") {
        // List ALL files owned by the current user that are not trashed
        // 'me' in owners ensures we only get files owned by the authenticated user
        // This excludes "Shared with me" items completely
        query.addQueryItem("q", "trashed = false and 'me' in owners");
        query.addQueryItem("corpora", "user");
        qDebug() << "API: Listing owned files only (q=trashed=false and 'me' in owners)";
    } else if (folderId == "myDrive") {
        // Same as "all" - list all files owned by user (exclude shared)
        query.addQueryItem("q", "trashed = false and 'me' in owners");
        query.addQueryItem("corpora", "user");
    } else {
        // List files in specific folder
        query.addQueryItem("q", QString("'%1' in parents and trashed = false").arg(folderId));
    }
    query.addQueryItem(
        "fields",
        "nextPageToken,files(id,name,mimeType,size,createdTime,modifiedTime,md5Checksum,parents,"
        "trashed,starred,shared,ownedByMe,webViewLink,webContentLink,iconLink,shortcutDetails)");
    query.addQueryItem("pageSize", "100");

    if (!pageToken.isEmpty()) {
        query.addQueryItem("pageToken", pageToken);
    }

    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "listFiles");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        QList<DriveFile> files;
        QJsonArray filesArray = obj["files"].toArray();

        for (const QJsonValue& fileValue : filesArray) {
            files.append(parseFileJson(fileValue.toObject()));
        }

        QString nextPageToken = obj["nextPageToken"].toString();

        emit filesListed(files, nextPageToken);
    });
}

void GoogleDriveClient::getFile(const QString& fileId) {
    QUrl url(API_BASE_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("fields",
                       "id,name,mimeType,size,createdTime,modifiedTime,md5Checksum,parents,trashed,"
                       "starred,shared,webViewLink,webContentLink,iconLink");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "getFile");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        DriveFile file = parseFileJson(doc.object());

        emit fileReceived(file);
    });
}

void GoogleDriveClient::downloadFile(const QString& fileId, const QString& localPath) {
    QUrl url(API_BASE_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("alt", "media");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    // Create file for writing
    QFile* file = new QFile(localPath);
    if (!file->open(QIODevice::WriteOnly)) {
        delete file;
        emit error("downloadFile", "Failed to open file for writing: " + localPath);
        reply->abort();
        reply->deleteLater();
        return;
    }

    // Connect progress signal
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, fileId](qint64 received, qint64 total) {
                emit downloadProgress(fileId, received, total);
            });

    // Write data as it arrives
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, file]() { file->write(reply->readAll()); });

    connect(reply, &QNetworkReply::finished, this, [this, reply, file, fileId, localPath]() {
        file->close();
        file->deleteLater();
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "downloadFile");
            QFile::remove(localPath);
            return;
        }

        emit fileDownloaded(fileId, localPath);
    });
}

void GoogleDriveClient::uploadFile(const QString& localPath, const QString& parentId,
                                   const QString& fileName) {
    QFileInfo fileInfo(localPath);
    if (!fileInfo.exists()) {
        emit error("uploadFile", "File does not exist: " + localPath);
        return;
    }

    QString name = fileName.isEmpty() ? fileInfo.fileName() : fileName;

    // Determine MIME type
    QMimeDatabase mimeDb;
    QMimeType mimeType = mimeDb.mimeTypeForFile(localPath);

    // Create metadata
    QJsonObject metadata;
    metadata["name"] = name;
    metadata["parents"] = QJsonArray({parentId});

    // Use resumable upload for files
    QUrl url(UPLOAD_URL + "/files");
    QUrlQuery query;
    query.addQueryItem("uploadType", "multipart");
    query.addQueryItem("fields", "id,name,mimeType,modifiedTime,parents");
    url.setQuery(query);

    // Create multipart request
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::RelatedType);

    // Metadata part
    QHttpPart metadataPart;
    metadataPart.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
    metadataPart.setBody(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    multiPart->append(metadataPart);

    // File part
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType.name());

    QFile* file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        delete multiPart;
        emit error("uploadFile", "Failed to open file for reading: " + localPath);
        return;
    }

    filePart.setBodyDevice(file);
    file->setParent(multiPart);  // Ensure file is deleted with multiPart
    multiPart->append(filePart);

    QNetworkRequest request = createRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "multipart/related; boundary=" + multiPart->boundary());

    QNetworkReply* reply = m_networkManager->post(request, multiPart);
    tagReply(reply, QString(), localPath);
    multiPart->setParent(reply);  // Delete multiPart with reply

    connect(reply, &QNetworkReply::uploadProgress, this,
            [this, localPath](qint64 sent, qint64 total) {
                emit uploadProgress(localPath, sent, total);
            });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "uploadFile");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        DriveFile uploadedFile = parseFileJson(doc.object());
        QString localPath = reply->property("localPath").toString();

        emit fileUploaded(uploadedFile);
        emit fileUploadedDetailed(uploadedFile, localPath);
    });
}

void GoogleDriveClient::updateFile(const QString& fileId, const QString& localPath) {
    QFileInfo fileInfo(localPath);
    if (!fileInfo.exists()) {
        emit error("updateFile", "File does not exist: " + localPath);
        return;
    }

    // Determine MIME type
    QMimeDatabase mimeDb;
    QMimeType mimeType = mimeDb.mimeTypeForFile(localPath);

    // Use simple upload for update
    QUrl url(UPLOAD_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("uploadType", "media");
    query.addQueryItem("fields", "id,name,mimeType,modifiedTime,parents");
    url.setQuery(query);

    QFile* file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        emit error("updateFile", "Failed to open file for reading: " + localPath);
        return;
    }

    QNetworkRequest request = createRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, mimeType.name());

    QNetworkReply* reply = m_networkManager->sendCustomRequest(request, "PATCH", file);
    tagReply(reply, fileId, localPath);
    file->setParent(reply);

    connect(reply, &QNetworkReply::uploadProgress, this,
            [this, localPath](qint64 sent, qint64 total) {
                emit uploadProgress(localPath, sent, total);
            });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "updateFile");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        DriveFile updatedFile = parseFileJson(doc.object());

        emit fileUpdated(updatedFile);
    });
}

void GoogleDriveClient::moveFile(const QString& fileId, const QString& newParentId,
                                 const QString& oldParentId) {
    QUrl url(API_BASE_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("addParents", newParentId);
    query.addQueryItem("removeParents", oldParentId);
    query.addQueryItem("fields", "id,name,mimeType,modifiedTime,parents");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->sendCustomRequest(request, "PATCH");
    tagReply(reply, fileId, QString());

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "moveFile");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        DriveFile movedFile = parseFileJson(doc.object());

        emit fileMoved(fileId);
        if (movedFile.isValid()) {
            emit fileMovedDetailed(movedFile);
        }
    });
}

void GoogleDriveClient::renameFile(const QString& fileId, const QString& newName) {
    QUrl url(API_BASE_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("fields", "id,name,mimeType,modifiedTime,parents");
    url.setQuery(query);

    QJsonObject metadata;
    metadata["name"] = newName;

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->sendCustomRequest(
        request, "PATCH", QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    tagReply(reply, fileId, QString());

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "renameFile");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        DriveFile renamedFile = parseFileJson(doc.object());

        emit fileRenamed(fileId);
        if (renamedFile.isValid()) {
            emit fileRenamedDetailed(renamedFile);
        }
    });
}

void GoogleDriveClient::deleteFile(const QString& fileId) {
    QUrl url(API_BASE_URL + "/files/" + fileId);

    QNetworkRequest request = createRequest(url);

    QNetworkReply* reply = m_networkManager->deleteResource(request);
    tagReply(reply, fileId, QString());

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "deleteFile");
            return;
        }

        emit fileDeleted(fileId);
    });
}

void GoogleDriveClient::createFolder(const QString& name, const QString& parentId,
                                     const QString& localPath) {
    QUrl url(API_BASE_URL + "/files");
    QUrlQuery query;
    query.addQueryItem("fields", "id,name,mimeType,modifiedTime,parents");
    url.setQuery(query);

    QJsonObject metadata;
    metadata["name"] = name;
    metadata["mimeType"] = "application/vnd.google-apps.folder";
    metadata["parents"] = QJsonArray({parentId});

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply =
        m_networkManager->post(request, QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    tagReply(reply, QString(), localPath);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "createFolder");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        DriveFile folder = parseFileJson(doc.object());
        QString replyLocalPath = reply->property("localPath").toString();

        emit folderCreated(folder);
        emit folderCreatedDetailed(folder, replyLocalPath);
    });
}

void GoogleDriveClient::listChanges(const QString& startPageToken) {
    if (startPageToken.isEmpty()) {
        // Need to get start page token first
        getStartPageToken();
        return;
    }

    QUrl url(API_BASE_URL + "/changes");
    QUrlQuery query;
    query.addQueryItem("pageToken", startPageToken);
    query.addQueryItem(
        "fields",
        "nextPageToken,newStartPageToken,changes(fileId,time,removed,file(id,name,mimeType,size,"
        "createdTime,modifiedTime,md5Checksum,parents,trashed,ownedByMe))");
    query.addQueryItem("pageSize", "100");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "listChanges");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        QList<DriveChange> changes;
        QJsonArray changesArray = obj["changes"].toArray();

        for (const QJsonValue& changeValue : changesArray) {
            changes.append(parseChangeJson(changeValue.toObject()));
        }

        QString nextPageToken = obj["nextPageToken"].toString();
        QString newStartPageToken = obj["newStartPageToken"].toString();
        bool hasMorePages = !nextPageToken.isEmpty();

        // Use nextPageToken if there are more pages, otherwise use newStartPageToken
        QString tokenForNext = hasMorePages ? nextPageToken : newStartPageToken;

        emit changesReceived(changes, tokenForNext, hasMorePages);
    });
}

void GoogleDriveClient::getStartPageToken() {
    QUrl url(API_BASE_URL + "/changes/startPageToken");

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "getStartPageToken");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QString token = doc.object()["startPageToken"].toString();

        emit startPageTokenReceived(token);
    });
}

void GoogleDriveClient::getAboutInfo() {
    QUrl url(API_BASE_URL + "/about");
    QUrlQuery query;
    query.addQueryItem("fields", "storageQuota,user");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "getAboutInfo");
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject quota = doc.object()["storageQuota"].toObject();

        qint64 used = quota["usage"].toString().toLongLong();
        qint64 limit = quota["limit"].toString().toLongLong();

        emit aboutInfoReceived(used, limit);

        QJsonObject user = doc.object()["user"].toObject();
        QString displayName = user["displayName"].toString();
        QString emailAddress = user["emailAddress"].toString();
        if (!emailAddress.isEmpty() || !displayName.isEmpty()) {
            emit userInfoReceived(displayName, emailAddress);
        }
    });
}

QJsonArray GoogleDriveClient::getParentsByFileId(const QString& fileId) {
    QUrl url(API_BASE_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("fields", "parents");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    // Use event loop to wait for reply (blocking call)
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QString parentId;

    QJsonArray parents;
    if (reply->error() != QNetworkReply::NoError) {
        handleNetworkError(reply, "getParentByFileId");
    } else {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        parents = obj["parents"].toArray();
    }

    reply->deleteLater();
    return parents;
}

DriveFile GoogleDriveClient::getFileMetadataBlocking(const QString& fileId) {
    QUrl url(API_BASE_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("fields", "id,name,parents,mimeType,modifiedTime,md5Checksum");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    // Use event loop to wait for reply (blocking call)
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    DriveFile file;
    if (reply->error() != QNetworkReply::NoError) {
        handleNetworkError(reply, "getFileMetadataBlocking");
    } else {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        file.id = obj["id"].toString();
        file.name = obj["name"].toString();
        file.mimeType = obj["mimeType"].toString();
        file.modifiedTime = QDateTime::fromString(obj["modifiedTime"].toString(), Qt::ISODate);
        file.md5Checksum = obj["md5Checksum"].toString();
        file.isFolder = file.mimeType == "application/vnd.google-apps.folder";

        QJsonArray parentsArray = obj["parents"].toArray();
        for (const QJsonValue& parentValue : parentsArray) {
            file.parents.append(parentValue.toString());
        }
    }

    reply->deleteLater();
    return file;
}

QString GoogleDriveClient::getFolderIdByPath(const QString& path) {
    if (path.isEmpty() || path == "/") {
        return "root";
    }

    QStringList parts = path.split('/', Qt::SkipEmptyParts);
    QString parentId = "root";

    for (const QString& part : parts) {
        QUrl url(API_BASE_URL + "/files");
        QUrlQuery query;
        query.addQueryItem("q", QString("name = '%1' and '%2' in parents and mimeType = "
                                        "'application/vnd.google-apps.folder' and trashed = false")
                                    .arg(part)
                                    .arg(parentId));
        query.addQueryItem("fields", "files(id,name)");
        url.setQuery(query);

        QNetworkRequest request = createRequest(url);
        QNetworkReply* reply = m_networkManager->get(request);

        // Use event loop to wait for reply (blocking call)
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "getFolderIdByPath");
            reply->deleteLater();
            return QString();
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();
        QJsonArray filesArray = obj["files"].toArray();

        if (filesArray.isEmpty()) {
            // Folder not found
            reply->deleteLater();
            return QString();
        }

        QJsonObject folderObj = filesArray.first().toObject();
        parentId = folderObj["id"].toString();

        reply->deleteLater();
    }

    return parentId;
}

QDateTime GoogleDriveClient::getRemoteModifiedTime(const QString& fileId) {
    QUrl url(API_BASE_URL + "/files/" + fileId);
    QUrlQuery query;
    query.addQueryItem("fields", "modifiedTime");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);

    // Use event loop to wait for reply (blocking call)
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QDateTime modifiedTime;
    if (reply->error() != QNetworkReply::NoError) {
        handleNetworkError(reply, "getRemoteModifiedTime");
    } else {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        modifiedTime = QDateTime::fromString(obj["modifiedTime"].toString(), Qt::ISODate);
    }

    reply->deleteLater();
    return modifiedTime;
}

QString GoogleDriveClient::getRootFolderId() {
    // call files.get with fileId='root' to get the actual root folder ID
    QUrl url(API_BASE_URL + "/files/root");
    QUrlQuery query;
    query.addQueryItem("fields", "id");
    url.setQuery(query);
    QNetworkRequest request = createRequest(url);
    QNetworkReply* reply = m_networkManager->get(request);
    // Use event loop to wait for reply (blocking call)
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QString rootId;
    if (reply->error() != QNetworkReply::NoError) {
        handleNetworkError(reply, "getRootFolderId");
    } else {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();
        rootId = obj["id"].toString();
    }
    reply->deleteLater();
    return rootId;
}

QList<DriveFile> GoogleDriveClient::listFilesBlocking(const QString& folderId) {
    QList<DriveFile> allFiles;
    QString pageToken;

    do {
        QUrl url(API_BASE_URL + "/files");
        QUrlQuery query;

        if (folderId.isEmpty() || folderId == "root") {
            query.addQueryItem("q", QString("'%1' in parents and trashed = false and ("
                                            "not mimeType contains 'application/vnd.google-apps.' "
                                            "or mimeType = 'application/vnd.google-apps.folder' "
                                            "or mimeType = 'application/vnd.google-apps.shortcut')")
                                        .arg(folderId.isEmpty() ? "root" : folderId));
        } else {
            query.addQueryItem("q", QString("'%1' in parents and trashed = false and ("
                                            "not mimeType contains 'application/vnd.google-apps.' "
                                            "or mimeType = 'application/vnd.google-apps.folder' "
                                            "or mimeType = 'application/vnd.google-apps.shortcut')")
                                        .arg(folderId));
        }
        query.addQueryItem(
            "fields",
            "nextPageToken,files(id,name,mimeType,size,createdTime,modifiedTime,md5Checksum,"
            "parents,trashed,starred,shared,ownedByMe,webViewLink,webContentLink,iconLink,"
            "shortcutDetails)");
        query.addQueryItem("pageSize", "1000");

        if (!pageToken.isEmpty()) {
            query.addQueryItem("pageToken", pageToken);
        }

        url.setQuery(query);

        QNetworkRequest request = createRequest(url);
        QNetworkReply* reply = m_networkManager->get(request);

        // Use event loop to wait for reply (blocking call)
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            handleNetworkError(reply, "listFilesBlocking");
            reply->deleteLater();
            break;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        QJsonArray filesArray = obj["files"].toArray();
        for (const QJsonValue& fileValue : filesArray) {
            allFiles.append(parseFileJson(fileValue.toObject()));
        }

        pageToken = obj["nextPageToken"].toString();
        reply->deleteLater();
    } while (!pageToken.isEmpty());

    return allFiles;
}