/**
 * @file DriveFile.cpp
 * @brief Implementation of DriveFile registration
 */

#include "DriveFile.h"

// Static registration for QMetaType
static int driveFileTypeId = qRegisterMetaType<DriveFile>("DriveFile");
