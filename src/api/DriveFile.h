/**
 * @file DriveFile.h
 * @brief Data structure for Google Drive file metadata
 */

#ifndef DRIVEFILE_H
#define DRIVEFILE_H

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

/**
 * @struct DriveFile
 * @brief Represents a Google Drive file or folder
 *
 * Contains metadata about a file or folder in Google Drive.
 */
struct DriveFile {
    QString id;                      ///< Unique file ID
    QString name;                    ///< File name
    QString mimeType;                ///< MIME type
    qint64 size = 0;                 ///< File size in bytes
    QDateTime createdTime;           ///< Creation timestamp
    QDateTime modifiedTime;          ///< Last modification timestamp
    QString md5Checksum;             ///< MD5 checksum (not available for folders)
    QStringList parents;             ///< Parent folder IDs
    bool trashed = false;            ///< Whether file is in trash
    bool starred = false;            ///< Whether file is starred
    bool shared = false;             ///< Whether file is shared
    bool ownedByMe = true;           ///< Whether file is owned by authenticated user
    bool isFolder = false;           ///< Whether this is a folder
    bool isShortcut = false;         ///< Whether this is a shortcut to another file
    QString shortcutTargetId;        ///< Target file ID if this is a shortcut
    QString shortcutTargetMimeType;  ///< Target MIME type if this is a shortcut
    QString webViewLink;             ///< Web link to view file
    QString webContentLink;          ///< Web link to download file content
    QString iconLink;                ///< Link to file icon

    /**
     * @brief Check if the file is valid (has required fields)
     * @return true if the file has an ID and name
     */
    bool isValid() const { return !id.isEmpty() && !name.isEmpty(); }

    /**
     * @brief Check if this is a Google Docs file
     * @return true if the MIME type is a Google Docs type
     */
    bool isGoogleDoc() const {
        return mimeType.startsWith("application/vnd.google-apps.") &&
               !mimeType.endsWith("folder");  // folders are not docs, and should be synced!
    }

    /**
     * @brief Get the parent folder ID
     * @return First parent ID or empty string
     */
    QString parentId() const { return parents.isEmpty() ? QString() : parents.first(); }

    /**
     * @brief Compare files for equality
     * @param other Other file to compare
     * @return true if files have the same ID
     */
    bool operator==(const DriveFile& other) const { return id == other.id; }

    /**
     * @brief Compare files for inequality
     * @param other Other file to compare
     * @return true if files have different IDs
     */
    bool operator!=(const DriveFile& other) const { return id != other.id; }
};

// Register type for use in signals/slots
Q_DECLARE_METATYPE(DriveFile)

#endif  // DRIVEFILE_H
