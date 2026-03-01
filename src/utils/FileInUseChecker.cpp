/**
 * @file FileInUseChecker.cpp
 * @brief Implementation of file-in-use detection via /proc/star/fd scanning
 */

#include "FileInUseChecker.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

bool FileInUseChecker::isFileOpenForWriting(const QString& absolutePath) {
    // Resolve the canonical path so symlinks don't defeat the comparison
    QFileInfo targetInfo(absolutePath);
    if (!targetInfo.exists()) {
        return false;
    }
    QString canonicalPath = targetInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        return false;
    }

    pid_t selfPid = getpid();

    QDir procDir("/proc");
    if (!procDir.exists()) {
        qWarning() << "FileInUseChecker: /proc not available";
        return false;
    }

    // Iterate all numeric (PID) directories in /proc
    const QStringList pidEntries =
        procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);

    for (const QString& pidStr : pidEntries) {
        bool ok = false;
        pid_t pid = pidStr.toLongLong(&ok);
        if (!ok) {
            continue;  // Not a PID directory
        }
        if (pid == selfPid) {
            continue;  // Skip our own process
        }

        // Read all fd symlinks for this PID
        QString fdDirPath = QStringLiteral("/proc/%1/fd").arg(pid);
        QDir fdDir(fdDirPath);
        if (!fdDir.exists()) {
            continue;  // Process may have exited or we lack permission
        }

        const QStringList fdEntries = fdDir.entryList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QString& fdStr : fdEntries) {
            // Read the symlink target
            QString fdPath = fdDirPath + "/" + fdStr;
            QByteArray linkTarget(512, '\0');
            ssize_t len =
                readlink(fdPath.toUtf8().constData(), linkTarget.data(), linkTarget.size() - 1);
            if (len <= 0) {
                continue;
            }
            linkTarget.truncate(len);
            QString resolvedFd = QString::fromUtf8(linkTarget);

            if (resolvedFd != canonicalPath) {
                continue;
            }

            // This FD points to our target file — check if opened for writing.
            // Read /proc/PID/fdinfo/N for the flags field.
            QString fdinfoPath = QStringLiteral("/proc/%1/fdinfo/%2").arg(pid).arg(fdStr);
            QFile fdinfoFile(fdinfoPath);
            if (!fdinfoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                // Can't read fdinfo — conservatively assume not writing
                continue;
            }

            while (!fdinfoFile.atEnd()) {
                QByteArray line = fdinfoFile.readLine().trimmed();
                if (line.startsWith("flags:")) {
                    // Format: "flags:\t0100002" (octal)
                    QByteArray flagsStr = line.mid(6).trimmed();
                    bool flagsOk = false;
                    unsigned long flags = flagsStr.toULong(&flagsOk, 8);
                    if (!flagsOk) {
                        break;
                    }

                    // Check O_WRONLY (01) or O_RDWR (02) in the access mode bits
                    int accessMode = flags & O_ACCMODE;
                    if (accessMode == O_WRONLY || accessMode == O_RDWR) {
                        qDebug() << "FileInUseChecker: File" << canonicalPath
                                 << "is open for writing by PID" << pid << "(fd" << fdStr << ")";
                        return true;
                    }
                    break;
                }
            }
        }
    }

    return false;
}
