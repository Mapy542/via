/**
 * @file LogManager.cpp
 * @brief Implementation of debug logging to file
 */

#include "LogManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <iostream>

LogManager* LogManager::s_instance = nullptr;

LogManager& LogManager::instance() {
    if (!s_instance) {
        s_instance = new LogManager();
    }
    return *s_instance;
}

LogManager::LogManager(QObject* parent)
    : QObject(parent),
      m_maxLogSize(10 * 1024 * 1024)  // 10 MB
      ,
      m_maxLogFiles(5),
      m_initialized(false),
      m_previousHandler(nullptr) {}

LogManager::~LogManager() { shutdown(); }

bool LogManager::initialize(const QString& logDir) {
    QMutexLocker locker(&m_mutex);

    if (m_initialized) {
        return true;
    }

    // Determine log directory
    if (logDir.isEmpty()) {
        m_logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
    } else {
        m_logDir = logDir;
    }

    // Create log directory if it doesn't exist
    QDir dir(m_logDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            std::cerr << "LogManager: Failed to create log directory: " << m_logDir.toStdString()
                      << std::endl;
            return false;
        }
    }

    // Create log file with timestamp
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    m_currentLogPath = m_logDir + "/via_" + timestamp + ".log";

    m_logFile.setFileName(m_currentLogPath);
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::cerr << "LogManager: Failed to open log file: " << m_currentLogPath.toStdString()
                  << std::endl;
        return false;
    }

    // Clean up old log files
    cleanOldLogs();

    // Install message handler
    m_previousHandler = qInstallMessageHandler(messageHandler);

    m_initialized = true;

    // Write header directly to avoid mutex deadlock (writeToFile also locks m_mutex).
    // No need to check rotateLogsIfNeeded() here since we just created a fresh log file.
    QString header = QString(
                         "\n========================================\n"
                         "Via Log Started\n"
                         "Time: %1\n"
                         "========================================\n")
                         .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    QTextStream stream(&m_logFile);
    stream << header << "\n";
    m_logFile.flush();

    return true;
}

void LogManager::shutdown() {
    QMutexLocker locker(&m_mutex);

    if (!m_initialized) {
        return;
    }

    // Write footer
    QString footer = QString(
                         "\n========================================\n"
                         "Via Log Ended\n"
                         "Time: %1\n"
                         "========================================\n")
                         .arg(QDateTime::currentDateTime().toString(Qt::ISODate));

    if (m_logFile.isOpen()) {
        QTextStream stream(&m_logFile);
        stream << footer;
        m_logFile.flush();
        m_logFile.close();
    }

    // Restore previous handler
    if (m_previousHandler) {
        qInstallMessageHandler(m_previousHandler);
        m_previousHandler = nullptr;
    }

    m_initialized = false;
}

QString LogManager::currentLogPath() const { return m_currentLogPath; }

QString LogManager::logDirectory() const { return m_logDir; }

void LogManager::flush() {
    QMutexLocker locker(&m_mutex);
    if (m_logFile.isOpen()) {
        m_logFile.flush();
    }
}

void LogManager::setMaxLogSize(qint64 bytes) { m_maxLogSize = bytes; }

void LogManager::setMaxLogFiles(int count) { m_maxLogFiles = count; }

void LogManager::messageHandler(QtMsgType type, const QMessageLogContext& context,
                                const QString& msg) {
    if (!s_instance || !s_instance->m_initialized) {
        // Fallback to stderr
        std::cerr << msg.toStdString() << std::endl;
        return;
    }

    // Format message with timestamp and type
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString typeStr;

    switch (type) {
        case QtDebugMsg:
            typeStr = "DEBUG";
            break;
        case QtInfoMsg:
            typeStr = "INFO";
            break;
        case QtWarningMsg:
            typeStr = "WARN";
            break;
        case QtCriticalMsg:
            typeStr = "ERROR";
            break;
        case QtFatalMsg:
            typeStr = "FATAL";
            break;
    }

    // Include file/line info for debug builds
    QString locationStr;
    if (context.file) {
        QFileInfo fileInfo(context.file);
        locationStr = QString(" [%1:%2]").arg(fileInfo.fileName()).arg(context.line);
    }

    QString formattedMsg =
        QString("[%1] [%2]%3 %4").arg(timestamp).arg(typeStr).arg(locationStr).arg(msg);

    // Write to console
    std::cout << formattedMsg.toStdString() << std::endl;

    // Write to file
    s_instance->writeToFile(formattedMsg);

    // For fatal messages, also write to stderr and abort
    if (type == QtFatalMsg) {
        std::cerr << "FATAL: " << msg.toStdString() << std::endl;
        s_instance->flush();
        abort();
    }
}

void LogManager::writeToFile(const QString& message) {
    QMutexLocker locker(&m_mutex);

    if (!m_logFile.isOpen()) {
        return;
    }

    // Check if rotation is needed
    rotateLogsIfNeeded();

    QTextStream stream(&m_logFile);
    stream << message << "\n";

    // Flush periodically to ensure logs are written
    static int writeCount = 0;
    if (++writeCount % 10 == 0) {
        m_logFile.flush();
    }
}

void LogManager::rotateLogsIfNeeded() {
    if (m_logFile.size() < m_maxLogSize) {
        return;
    }

    // Close current file
    m_logFile.close();

    // Create new log file with new timestamp
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    m_currentLogPath = m_logDir + "/via_" + timestamp + ".log";

    m_logFile.setFileName(m_currentLogPath);
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::cerr << "LogManager: Failed to open new log file: " << m_currentLogPath.toStdString()
                  << std::endl;
        return;
    }

    // Clean up old logs
    cleanOldLogs();

    // Write rotation notice
    QString notice = QString(
                         "\n========================================\n"
                         "Log rotated at %1\n"
                         "========================================\n")
                         .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    QTextStream stream(&m_logFile);
    stream << notice;
}

void LogManager::cleanOldLogs() {
    QDir dir(m_logDir);
    QStringList filters;
    filters << "via_*.log";

    QFileInfoList logFiles = dir.entryInfoList(filters, QDir::Files, QDir::Time);

    // Keep only the newest m_maxLogFiles files
    while (logFiles.size() > m_maxLogFiles) {
        QFileInfo oldest = logFiles.takeLast();
        QFile::remove(oldest.absoluteFilePath());
    }
}
