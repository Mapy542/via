/**
 * @file GoogleDriveClient.h
 * @brief Google Drive REST API client
 *
 * Provides a C++ interface to the Google Drive REST API.
 */

#ifndef GOOGLEDRIVECLIENT_H
#define GOOGLEDRIVECLIENT_H

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QUrl>

#include "DriveChange.h"
#include "DriveFile.h"

class GoogleAuthManager;

/**
 * @class GoogleDriveClient
 * @brief Client for Google Drive REST API
 *
 * Implements the following endpoints:
 * - files.list - List files and folders
 * - files.get - Download files
 * - files.create - Upload new files
 * - files.update - Update existing files
 * - files.delete - Delete files
 * - changes.list - Get remote changes
 */
class GoogleDriveClient : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Construct the Drive client
     * @param authManager Pointer to the authentication manager
     * @param parent Parent object
     */
    explicit GoogleDriveClient(GoogleAuthManager* authManager, QObject* parent = nullptr);

    ~GoogleDriveClient() override;

   public slots:
    /**
     * @brief List files in a folder
     * @param folderId Parent folder ID ("root" for root folder)
     * @param pageToken Token for pagination
     */
    virtual void listFiles(const QString& folderId = "root", const QString& pageToken = QString());

    /**
     * @brief Get file metadata
     * @param fileId File ID
     */
    virtual void getFile(const QString& fileId);

    /**
     * @brief Download a file
     * @param fileId File ID
     * @param localPath Local path to save the file
     */
    virtual void downloadFile(const QString& fileId, const QString& localPath);

    /**
     * @brief Upload a new file
     * @param localPath Local file path
     * @param parentId Parent folder ID
     * @param fileName Name for the file in Drive
     */
    virtual void uploadFile(const QString& localPath, const QString& parentId = "root",
                            const QString& fileName = QString());

    /**
     * @brief Update an existing file (upload new version)
     * @param fileId File ID to update
     * @param localPath Local file path
     */
    virtual void updateFile(const QString& fileId, const QString& localPath);

    /**
     * @brief Move a file to a different folder
     * @param fileId File ID
     * @param newParentId New parent folder ID
     * @param oldParentId Old parent folder ID
     */
    virtual void moveFile(const QString& fileId, const QString& newParentId, const QString& oldParentId);

    /**
     * @brief Rename a file
     * @param fileId File ID
     * @param newName New name for the file
     */
    virtual void renameFile(const QString& fileId, const QString& newName);

    /**
     * @brief Delete a file
     * @param fileId File ID
     */
    virtual void deleteFile(const QString& fileId);

    /**
     * @brief Create a new folder
     * @param name Folder name
     * @param parentId Parent folder ID
     */
    virtual void createFolder(const QString& name, const QString& parentId = "root",
                              const QString& localPath = QString());

    /**
     * @brief Get changes since the last check
     */
    virtual void listChanges(const QString& startPageToken = QString());

    /**
     * @brief Get the start page token for change tracking
     */
    virtual void getStartPageToken();

    /**
     * @brief Get about info (storage quota, etc.)
     */
    virtual void getAboutInfo();

    /**
     * @brief Get parent folder ID by file ID (Blocking LONG NETWORK CALL)
     * @param fileId File ID
     * @return Parent folder ID or empty string
     */
    virtual QJsonArray getParentsByFileId(const QString& fileId);

    /**
     * @brief Get file metadata (Blocking LONG NETWORK CALL)
     * @param fileId File ID
     * @return DriveFile metadata (id, name, parents)
     */
    virtual DriveFile getFileMetadataBlocking(const QString& fileId);

    /**
     * @brief Get folder ID by its relative path (Blocking LONG NETWORK CALL)
     * @param folderPath Folder path (root-relative, e.g. "/folder1"=>"user/root/folder1")
     * @return Folder ID or empty string if not found
     */
    virtual QString getFolderIdByPath(const QString& folderPath);

    /**
     * @brief Get remote modified time of a file by its ID (Blocking LONG NETWORK CALL)
     * @param fileId File ID
     * @return QTime representing the last modified time
     */
    virtual QDateTime getRemoteModifiedTime(const QString& fileId);

    /**
     * @brief Get the root folder ID (Blocking LONG NETWORK CALL)
     * @return Root folder ID
     */
    virtual QString getRootFolderId();

    /**
     * @brief List files in a folder (Blocking LONG NETWORK CALL)
     * @param folderId Parent folder ID ("root" for root folder)
     * @return List of files in the folder
     */
    virtual QList<DriveFile> listFilesBlocking(const QString& folderId = "root");

   signals:
    /**
     * @brief Emitted when file list is received
     * @param files List of files
     * @param nextPageToken Token for next page (empty if no more)
     */
    void filesListed(const QList<DriveFile>& files, const QString& nextPageToken);

    /**
     * @brief Emitted when file metadata is received
     * @param file File metadata
     */
    void fileReceived(const DriveFile& file);

    /**
     * @brief Emitted when file download completes
     * @param fileId File ID
     * @param localPath Local path where file was saved
     */
    void fileDownloaded(const QString& fileId, const QString& localPath);

    /**
     * @brief Emitted when file upload completes
     * @param file Uploaded file metadata
     */
    void fileUploaded(const DriveFile& file);

    /**
     * @brief Emitted when file upload completes with local path context
     * @param file Uploaded file metadata
     * @param localPath Local path that was uploaded
     */
    void fileUploadedDetailed(const DriveFile& file, const QString& localPath);

    /**
     * @brief Emitted when file is updated
     * @param file Updated file metadata
     */
    void fileUpdated(const DriveFile& file);

    /**
     * @brief Emitted when file is moved
     * @param fileId File ID
     */
    void fileMoved(const QString& fileId);

    /**
     * @brief Emitted when file is moved with updated metadata
     * @param file Updated file metadata
     */
    void fileMovedDetailed(const DriveFile& file);

    /**
     * @brief Emitted when file is renamed
     * @param fileId File ID
     */
    void fileRenamed(const QString& fileId);

    /**
     * @brief Emitted when file is renamed with updated metadata
     * @param file Updated file metadata
     */
    void fileRenamedDetailed(const DriveFile& file);

    /**
     * @brief Emitted when file is deleted
     * @param fileId File ID
     */
    void fileDeleted(const QString& fileId);

    /**
     * @brief Emitted when folder is created
     * @param folder Created folder metadata
     */
    void folderCreated(const DriveFile& folder);

    /**
     * @brief Emitted when folder is created with local path context
     * @param folder Created folder metadata
     * @param localPath Local folder path that was created
     */
    void folderCreatedDetailed(const DriveFile& folder, const QString& localPath);

    /**
     * @brief Emitted when changes are received
     * @param changes List of changes
     * @param newStartPageToken Token for next check
     * @param hasMorePages True if there are more pages to fetch
     */
    void changesReceived(const QList<DriveChange>& changes, const QString& newStartPageToken, bool hasMorePages);

    /**
     * @brief Emitted when start page token is received
     * @param token Start page token
     */
    void startPageTokenReceived(const QString& token);

    /**
     * @brief Emitted when about info is received
     * @param storageUsed Bytes used
     * @param storageLimit Bytes available
     */
    void aboutInfoReceived(qint64 storageUsed, qint64 storageLimit);

    /**
     * @brief Emitted when user info is received from About endpoint
     * @param displayName User's display name
     * @param emailAddress User's email address
     */
    void userInfoReceived(const QString& displayName, const QString& emailAddress);

    /**
     * @brief Emitted on download progress
     * @param fileId File ID
     * @param bytesReceived Bytes received
     * @param bytesTotal Total bytes
     */
    void downloadProgress(const QString& fileId, qint64 bytesReceived, qint64 bytesTotal);

    /**
     * @brief Emitted on upload progress
     * @param localPath Local file path
     * @param bytesSent Bytes sent
     * @param bytesTotal Total bytes
     */
    void uploadProgress(const QString& localPath, qint64 bytesSent, qint64 bytesTotal);

    /**
     * @brief Emitted on API error
     * @param operation Operation that failed
     * @param error Error message
     */
    void error(const QString& operation, const QString& error);

    /**
     * @brief Emitted when an error occurs with HTTP status details
     * @param operation Operation name
     * @param errorMsg Error message
     * @param httpStatus HTTP status code (0 if unavailable)
     * @param fileId File ID associated with the request (if known)
     * @param localPath Local path associated with the request (if known)
     */
    void errorDetailed(const QString& operation, const QString& errorMsg, int httpStatus, const QString& fileId,
                       const QString& localPath);

    /**
     * @brief Emitted when an API call fails due to authentication/authorization
     * @param operation Operation name
     * @param httpStatus HTTP status code
     * @param errorMsg Error message
     */
    void authenticationFailure(const QString& operation, int httpStatus, const QString& errorMsg);

   private:
    QNetworkRequest createRequest(const QUrl& url);
    void handleNetworkError(QNetworkReply* reply, const QString& operation);
    DriveFile parseFileJson(const QJsonObject& json) const;
    DriveChange parseChangeJson(const QJsonObject& json) const;

    GoogleAuthManager* m_authManager;
    QNetworkAccessManager* m_networkManager;

    // API base URL
    static const QString API_BASE_URL;
    static const QString UPLOAD_URL;
};

#endif  // GOOGLEDRIVECLIENT_H
