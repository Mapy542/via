/**
 * @file SyncActionQueue.cpp
 * @brief Implementation of the thread-safe sync action queue
 */

#include "SyncActionQueue.h"

#include <QDebug>
#include <QDir>
#include <QMutexLocker>

namespace {

QString normalizePathIdentity(const QString& path) {
    if (path.isEmpty()) {
        return QString();
    }
    QString normalized = QDir::cleanPath(path);
    if (normalized == ".") {
        normalized.clear();
    }
    return normalized;
}

bool itemsMatchIdentity(const SyncActionItem& lhs, const SyncActionItem& rhs) {
    if (lhs.actionType != rhs.actionType) {
        return false;
    }

    const QString lhsFileId = lhs.fileId.trimmed();
    const QString rhsFileId = rhs.fileId.trimmed();
    if (!lhsFileId.isEmpty() && !rhsFileId.isEmpty()) {
        if (lhsFileId != rhsFileId) {
            return false;
        }
    } else {
        const QString lhsPath = normalizePathIdentity(lhs.localPath);
        const QString rhsPath = normalizePathIdentity(rhs.localPath);
        if (lhsPath.isEmpty() || rhsPath.isEmpty() || lhsPath != rhsPath) {
            return false;
        }
    }

    if (lhs.actionType == SyncActionType::MoveLocal ||
        lhs.actionType == SyncActionType::MoveRemote) {
        return normalizePathIdentity(lhs.moveDestination) ==
               normalizePathIdentity(rhs.moveDestination);
    }

    if (lhs.actionType == SyncActionType::RenameLocal ||
        lhs.actionType == SyncActionType::RenameRemote) {
        return lhs.renameTo == rhs.renameTo;
    }

    return true;
}

}  // namespace

// Static registration for QMetaType
// These registrations are needed for signal/slot connections with these types
static const int syncActionItemTypeId = qRegisterMetaType<SyncActionItem>("SyncActionItem");
static const int syncActionTypeTypeId = qRegisterMetaType<SyncActionType>("SyncActionType");

SyncActionQueue::SyncActionQueue(QObject* parent) : QObject(parent) {
    // Suppress unused variable warnings - the registrations have side effects
    Q_UNUSED(syncActionItemTypeId);
    Q_UNUSED(syncActionTypeTypeId);
}

SyncActionQueue::~SyncActionQueue() {
    // Wake any waiting threads before destruction
    wakeAll();
}

void SyncActionQueue::enqueue(const SyncActionItem& item) {
    if (!item.isValid()) {
        qWarning() << "Attempted to enqueue invalid sync action item"
                   << "type:" << static_cast<int>(item.actionType) << "path:" << item.localPath
                   << "fileId:" << item.fileId << "moveDestination:" << item.moveDestination
                   << "renameTo:" << item.renameTo;
        return;
    }

    bool wasEmpty = false;
    {
        QMutexLocker locker(&m_mutex);
        wasEmpty = m_queue.isEmpty();
        m_queue.enqueue(item);
    }

    emit itemEnqueued(item);

    // Signal that items are available (the "Jobs Available Wakeup Signal")
    if (wasEmpty) {
        emit itemsAvailable();
    }

    // Wake any threads waiting for items
    m_condition.wakeOne();

    qDebug() << "Sync action enqueued:" << static_cast<int>(item.actionType)
             << "path:" << item.localPath << "fileId:" << item.fileId;
}

bool SyncActionQueue::enqueueIfNotDuplicate(const SyncActionItem& item) {
    if (!item.isValid()) {
        qWarning() << "Attempted to enqueue invalid sync action item"
                   << "type:" << static_cast<int>(item.actionType) << "path:" << item.localPath
                   << "fileId:" << item.fileId << "moveDestination:" << item.moveDestination
                   << "renameTo:" << item.renameTo;
        return false;
    }

    bool wasEmpty = false;
    {
        QMutexLocker locker(&m_mutex);
        for (const SyncActionItem& pending : m_queue) {
            if (itemsMatchIdentity(pending, item)) {
                qDebug() << "Sync action duplicate suppressed:" << static_cast<int>(item.actionType)
                         << "path:" << item.localPath << "fileId:" << item.fileId;
                return false;
            }
        }

        wasEmpty = m_queue.isEmpty();
        m_queue.enqueue(item);
    }

    emit itemEnqueued(item);

    if (wasEmpty) {
        emit itemsAvailable();
    }

    m_condition.wakeOne();

    qDebug() << "Sync action enqueued:" << static_cast<int>(item.actionType)
             << "path:" << item.localPath << "fileId:" << item.fileId;
    return true;
}

bool SyncActionQueue::containsDuplicatePending(const SyncActionItem& item) const {
    if (!item.isValid()) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    for (const SyncActionItem& pending : m_queue) {
        if (itemsMatchIdentity(pending, item)) {
            return true;
        }
    }
    return false;
}

SyncActionItem SyncActionQueue::dequeue() {
    bool shouldEmitEmpty = false;
    SyncActionItem item;

    {
        QMutexLocker locker(&m_mutex);

        if (m_queue.isEmpty()) {
            return SyncActionItem();
        }

        item = m_queue.dequeue();
        shouldEmitEmpty = m_queue.isEmpty();
    }

    if (shouldEmitEmpty) {
        emit queueEmpty();
    }

    return item;
}

SyncActionItem SyncActionQueue::peek() const {
    QMutexLocker locker(&m_mutex);

    if (m_queue.isEmpty()) {
        return SyncActionItem();
    }

    return m_queue.head();
}

bool SyncActionQueue::waitForItems(int timeoutMs) {
    QMutexLocker locker(&m_mutex);

    if (!m_queue.isEmpty()) {
        return true;
    }

    // Wait for items with timeout
    return m_condition.wait(&m_mutex, timeoutMs);
}

bool SyncActionQueue::isEmpty() const {
    QMutexLocker locker(&m_mutex);
    return m_queue.isEmpty();
}

int SyncActionQueue::count() const {
    QMutexLocker locker(&m_mutex);
    return m_queue.count();
}

void SyncActionQueue::clear() {
    {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
    }

    emit queueEmpty();
}

int SyncActionQueue::removeByPath(const QString& localPath) {
    int removed = 0;
    bool shouldEmitEmpty = false;

    {
        QMutexLocker locker(&m_mutex);

        QQueue<SyncActionItem> newQueue;

        while (!m_queue.isEmpty()) {
            SyncActionItem item = m_queue.dequeue();
            if (item.localPath == localPath) {
                removed++;
            } else {
                newQueue.enqueue(item);
            }
        }

        m_queue = newQueue;
        shouldEmitEmpty = m_queue.isEmpty() && removed > 0;
    }

    if (shouldEmitEmpty) {
        emit queueEmpty();
    }

    return removed;
}

int SyncActionQueue::removeByFileId(const QString& fileId) {
    int removed = 0;
    bool shouldEmitEmpty = false;

    {
        QMutexLocker locker(&m_mutex);

        QQueue<SyncActionItem> newQueue;

        while (!m_queue.isEmpty()) {
            SyncActionItem item = m_queue.dequeue();
            if (item.fileId == fileId) {
                removed++;
            } else {
                newQueue.enqueue(item);
            }
        }

        m_queue = newQueue;
        shouldEmitEmpty = m_queue.isEmpty() && removed > 0;
    }

    if (shouldEmitEmpty) {
        emit queueEmpty();
    }

    return removed;
}

void SyncActionQueue::wakeAll() { m_condition.wakeAll(); }
