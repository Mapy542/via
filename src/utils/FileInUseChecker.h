/**
 * @file FileInUseChecker.h
 * @brief Utility to detect whether a file is open for writing by another process
 *
 * Uses the Linux /proc filesystem to scan file descriptors across all accessible
 * processes. This is the same mechanism used by fuser(1) and lsof(8).
 */

#ifndef FILEINUSECHECKER_H
#define FILEINUSECHECKER_H

#include <QString>

class FileInUseChecker {
   public:
    /**
     * @brief Check if a file is currently open for writing by another process
     * @param absolutePath Absolute path to the file to check
     * @return true if another process has the file open with write flags
     *
     * Scans /proc file descriptors for each process, checking for write-mode
     * flags (O_WRONLY or O_RDWR). Skips the current process. Requires no
     * elevated privileges for same-user processes.
     *
     * Returns false if the file does not exist or /proc is unavailable.
     */
    static bool isFileOpenForWriting(const QString& absolutePath);

   private:
    FileInUseChecker() = default;
};

#endif  // FILEINUSECHECKER_H
