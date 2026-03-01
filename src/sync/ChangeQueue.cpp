/**
 * @file ChangeQueue.cpp
 * @brief Implementation of the thread-safe change queue
 */

#include "ChangeQueue.h"

#include <QDebug>
#include <QMutexLocker>

// Static registration for QMetaType
// These registrations are needed for signal/slot connections with these types
static const int changeQueueItemTypeId = qRegisterMetaType<ChangeQueueItem>("ChangeQueueItem");
static const int changeTypeTypeId = qRegisterMetaType<ChangeType>("ChangeType");
static const int changeOriginTypeId = qRegisterMetaType<ChangeOrigin>("ChangeOrigin");

ChangeQueue::ChangeQueue(QObject* parent) : QObject(parent) {
    // Suppress unused variable warnings - the registrations have side effects
    Q_UNUSED(changeQueueItemTypeId);
    Q_UNUSED(changeTypeTypeId);
    Q_UNUSED(changeOriginTypeId);
}

ChangeQueue::~ChangeQueue() {
    // Wake any waiting threads before destruction
    wakeAll();
}

void ChangeQueue::enqueue(const ChangeQueueItem& item) {
    if (!item.isValid()) {
        qWarning() << "Attempted to enqueue invalid change item";
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

    qDebug() << "Change enqueued:" << static_cast<int>(item.changeType)
             << "origin:" << static_cast<int>(item.origin) << "path:" << item.localPath;
}

ChangeQueueItem ChangeQueue::dequeue() {
    bool shouldEmitEmpty = false;
    ChangeQueueItem item;

    {
        QMutexLocker locker(&m_mutex);

        if (m_queue.isEmpty()) {
            return ChangeQueueItem();
        }

        item = m_queue.dequeue();
        shouldEmitEmpty = m_queue.isEmpty();
    }

    if (shouldEmitEmpty) {
        emit queueEmpty();
    }

    return item;
}

ChangeQueueItem ChangeQueue::peek() const {
    QMutexLocker locker(&m_mutex);

    if (m_queue.isEmpty()) {
        return ChangeQueueItem();
    }

    return m_queue.head();
}

bool ChangeQueue::waitForItems(int timeoutMs) {
    QMutexLocker locker(&m_mutex);

    if (!m_queue.isEmpty()) {
        return true;
    }

    // Wait for items with timeout
    return m_condition.wait(&m_mutex, timeoutMs);
}

bool ChangeQueue::isEmpty() const {
    QMutexLocker locker(&m_mutex);
    return m_queue.isEmpty();
}

int ChangeQueue::count() const {
    QMutexLocker locker(&m_mutex);
    return m_queue.count();
}

void ChangeQueue::clear() {
    {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
    }

    emit queueEmpty();
}

int ChangeQueue::removeByPath(const QString& localPath) {
    int removed = 0;
    bool shouldEmitEmpty = false;

    {
        QMutexLocker locker(&m_mutex);

        QQueue<ChangeQueueItem> newQueue;

        while (!m_queue.isEmpty()) {
            ChangeQueueItem item = m_queue.dequeue();
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

int ChangeQueue::removeByFileId(const QString& fileId) {
    int removed = 0;
    bool shouldEmitEmpty = false;

    {
        QMutexLocker locker(&m_mutex);

        QQueue<ChangeQueueItem> newQueue;

        while (!m_queue.isEmpty()) {
            ChangeQueueItem item = m_queue.dequeue();
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

void ChangeQueue::wakeAll() { m_condition.wakeAll(); }
