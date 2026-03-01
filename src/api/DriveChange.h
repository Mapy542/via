/**
 * @file DriveChange.h
 * @brief Data structure for Google Drive change notifications
 */

#ifndef DRIVECHANGE_H
#define DRIVECHANGE_H

#include <QDateTime>
#include <QMetaType>
#include <QString>

#include "DriveFile.h"

/**
 * @struct DriveChange
 * @brief Represents a change notification from Google Drive
 *
 * Used to track changes to files for synchronization.
 */
struct DriveChange {
    QString changeId;      ///< Unique change ID
    QString fileId;        ///< ID of the changed file
    QDateTime time;        ///< Time of the change
    bool removed = false;  ///< Whether the file was removed
    DriveFile file;        ///< File metadata (if not removed)

    /**
     * @brief Check if the change is valid
     * @return true if the change has required fields
     */
    bool isValid() const { return !changeId.isEmpty() && !fileId.isEmpty(); }

    /**
     * @brief Compare changes for equality
     * @param other Other change to compare
     * @return true if changes have the same ID
     */
    bool operator==(const DriveChange& other) const { return changeId == other.changeId; }
};

// Register type for use in signals/slots
Q_DECLARE_METATYPE(DriveChange)

#endif  // DRIVECHANGE_H
