/**
 * @file LogManager.h
 * @brief Manages debug logging to file
 */

#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>

/**
 * @class LogManager
 * @brief Manages debug logging to file and console
 *
 * This class installs a Qt message handler that writes all debug output
 * to both the console and a log file in the logs directory.
 */
class LogManager : public QObject {
    Q_OBJECT

   public:
    /**
     * @brief Get the singleton instance
     * @return Reference to the LogManager instance
     */
    static LogManager& instance();

    /**
     * @brief Initialize the log manager
     * @param logDir Directory to store log files (default: AppDataLocation/logs)
     * @return true if initialization succeeded
     */
    bool initialize(const QString& logDir = QString());

    /**
     * @brief Shutdown and flush logs
     */
    void shutdown();

    /**
     * @brief Get the current log file path
     * @return Full path to the current log file
     */
    QString currentLogPath() const;

    /**
     * @brief Get the log directory path
     * @return Full path to the log directory
     */
    QString logDirectory() const;

    /**
     * @brief Flush pending log entries to disk
     */
    void flush();

    /**
     * @brief Set maximum log file size before rotation (default: 10MB)
     * @param bytes Maximum size in bytes
     */
    void setMaxLogSize(qint64 bytes);

    /**
     * @brief Set maximum number of log files to keep (default: 5)
     * @param count Maximum number of files
     */
    void setMaxLogFiles(int count);

   private:
    explicit LogManager(QObject* parent = nullptr);
    ~LogManager();

    // Disable copy
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    /**
     * @brief Qt message handler callback
     */
    static void messageHandler(QtMsgType type, const QMessageLogContext& context,
                               const QString& msg);

    /**
     * @brief Rotate log files if current one exceeds max size
     */
    void rotateLogsIfNeeded();

    /**
     * @brief Clean up old log files
     */
    void cleanOldLogs();

    /**
     * @brief Write a message to the log file
     * @param message Formatted log message
     * @param type Message severity level (flushes immediately for warnings and above)
     */
    void writeToFile(const QString& message, QtMsgType type = QtDebugMsg);

    static LogManager* s_instance;

    QFile m_logFile;
    QMutex m_mutex;
    QString m_logDir;
    QString m_currentLogPath;
    qint64 m_maxLogSize;
    int m_maxLogFiles;
    bool m_initialized;
    QtMessageHandler m_previousHandler;
};

#endif  // LOGMANAGER_H
