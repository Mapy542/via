/**
 * @file TestSyncActionQueue.cpp
 * @brief Unit tests for SyncActionQueue subsystem
 *
 * Tests cover: action enqueueing, queue ordering, concurrency, and edge cases.
 */

#include <QSignalSpy>
#include <QtTest/QtTest>
#include <atomic>
#include <thread>
#include <vector>

#include "sync/SyncActionQueue.h"

class TestSyncActionQueue : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    // Basic operations
    void testEnqueueAction();
    void testDequeueAction();
    void testQueueEmpty();

    // Ordering and removal
    void testQueueOrdering();
    void testRemoveByPath();
    void testRemoveByFileId();
    void testClearQueue();
    void testEnqueueIfNotDuplicate_SameActionDenied();
    void testEnqueueIfNotDuplicate_DifferentActionAllowed();
    void testEnqueueIfNotDuplicate_FileIdPreferredOverPath();

    // Concurrency
    void testConcurrentAccess();

   private:
    SyncActionQueue* m_queue = nullptr;

    static SyncActionItem makeAction(SyncActionType type, const QString& localPath,
                                     const QString& fileId = QString());
};

void TestSyncActionQueue::init() { m_queue = new SyncActionQueue(); }

void TestSyncActionQueue::cleanup() {
    delete m_queue;
    m_queue = nullptr;
}

SyncActionItem TestSyncActionQueue::makeAction(SyncActionType type, const QString& localPath,
                                               const QString& fileId) {
    SyncActionItem item;
    item.actionType = type;
    item.localPath = localPath;
    item.fileId = fileId;
    item.modifiedTime = QDateTime::currentDateTime();
    return item;
}

void TestSyncActionQueue::testEnqueueAction() {
    QSignalSpy itemsAvailableSpy(m_queue, &SyncActionQueue::itemsAvailable);
    QSignalSpy itemEnqueuedSpy(m_queue, &SyncActionQueue::itemEnqueued);

    SyncActionItem item = makeAction(SyncActionType::Upload, "file.txt");
    m_queue->enqueue(item);

    QCOMPARE(m_queue->count(), 1);
    QCOMPARE(itemsAvailableSpy.count(), 1);
    QCOMPARE(itemEnqueuedSpy.count(), 1);
}

void TestSyncActionQueue::testDequeueAction() {
    SyncActionItem item = makeAction(SyncActionType::Upload, "file.txt");
    m_queue->enqueue(item);

    SyncActionItem dequeued = m_queue->dequeue();
    QCOMPARE(dequeued.actionType, SyncActionType::Upload);
    QCOMPARE(dequeued.localPath, QString("file.txt"));
    QVERIFY(m_queue->isEmpty());
}

void TestSyncActionQueue::testQueueEmpty() {
    QVERIFY(m_queue->isEmpty());

    SyncActionItem dequeued = m_queue->dequeue();
    QVERIFY(!dequeued.isValid());

    SyncActionItem peeked = m_queue->peek();
    QVERIFY(!peeked.isValid());

    QSignalSpy itemsAvailableSpy(m_queue, &SyncActionQueue::itemsAvailable);
    SyncActionItem invalidItem;
    invalidItem.actionType = SyncActionType::Upload;
    m_queue->enqueue(invalidItem);
    QCOMPARE(m_queue->count(), 0);
    QCOMPARE(itemsAvailableSpy.count(), 0);
}

void TestSyncActionQueue::testQueueOrdering() {
    m_queue->enqueue(makeAction(SyncActionType::Upload, "a.txt"));
    m_queue->enqueue(makeAction(SyncActionType::Download, "b.txt", "id-b"));
    m_queue->enqueue(makeAction(SyncActionType::DeleteLocal, "c.txt"));

    QCOMPARE(m_queue->dequeue().localPath, QString("a.txt"));
    QCOMPARE(m_queue->dequeue().fileId, QString("id-b"));
    QCOMPARE(m_queue->dequeue().localPath, QString("c.txt"));
    QVERIFY(m_queue->isEmpty());
}

void TestSyncActionQueue::testRemoveByPath() {
    m_queue->enqueue(makeAction(SyncActionType::Upload, "a.txt"));
    m_queue->enqueue(makeAction(SyncActionType::Upload, "b.txt"));
    m_queue->enqueue(makeAction(SyncActionType::Upload, "a.txt"));

    int removed = m_queue->removeByPath("a.txt");
    QCOMPARE(removed, 2);
    QCOMPARE(m_queue->count(), 1);
    QCOMPARE(m_queue->dequeue().localPath, QString("b.txt"));
}

void TestSyncActionQueue::testRemoveByFileId() {
    m_queue->enqueue(makeAction(SyncActionType::Download, "a.txt", "id-a"));
    m_queue->enqueue(makeAction(SyncActionType::Download, "b.txt", "id-b"));
    m_queue->enqueue(makeAction(SyncActionType::Download, "c.txt", "id-a"));

    int removed = m_queue->removeByFileId("id-a");
    QCOMPARE(removed, 2);
    QCOMPARE(m_queue->count(), 1);
    QCOMPARE(m_queue->dequeue().fileId, QString("id-b"));
}

void TestSyncActionQueue::testClearQueue() {
    m_queue->enqueue(makeAction(SyncActionType::Upload, "a.txt"));
    m_queue->enqueue(makeAction(SyncActionType::Upload, "b.txt"));

    QSignalSpy queueEmptySpy(m_queue, &SyncActionQueue::queueEmpty);
    m_queue->clear();

    QVERIFY(m_queue->isEmpty());
    QCOMPARE(queueEmptySpy.count(), 1);
}

void TestSyncActionQueue::testEnqueueIfNotDuplicate_SameActionDenied() {
    SyncActionItem first = makeAction(SyncActionType::Upload, "./folder/../file.txt");
    SyncActionItem duplicate = makeAction(SyncActionType::Upload, "file.txt");

    QVERIFY(m_queue->enqueueIfNotDuplicate(first));
    QVERIFY(!m_queue->enqueueIfNotDuplicate(duplicate));
    QCOMPARE(m_queue->count(), 1);
}

void TestSyncActionQueue::testEnqueueIfNotDuplicate_DifferentActionAllowed() {
    SyncActionItem upload = makeAction(SyncActionType::Upload, "file.txt", "id-a");
    SyncActionItem download = makeAction(SyncActionType::Download, "file.txt", "id-a");

    QVERIFY(m_queue->enqueueIfNotDuplicate(upload));
    QVERIFY(m_queue->enqueueIfNotDuplicate(download));
    QCOMPARE(m_queue->count(), 2);
}

void TestSyncActionQueue::testEnqueueIfNotDuplicate_FileIdPreferredOverPath() {
    SyncActionItem first = makeAction(SyncActionType::Download, "path/a.txt", "same-id");
    SyncActionItem duplicate = makeAction(SyncActionType::Download, "path/b.txt", "same-id");

    QVERIFY(m_queue->enqueueIfNotDuplicate(first));
    QVERIFY(m_queue->containsDuplicatePending(duplicate));
    QVERIFY(!m_queue->enqueueIfNotDuplicate(duplicate));
    QCOMPARE(m_queue->count(), 1);
}

void TestSyncActionQueue::testConcurrentAccess() {
    constexpr int producerCount = 4;
    constexpr int itemsPerProducer = 200;
    const int totalItems = producerCount * itemsPerProducer;

    std::atomic<int> produced{0};
    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (int i = 0; i < producerCount; ++i) {
        producers.emplace_back([this, i, &produced]() {
            for (int j = 0; j < itemsPerProducer; ++j) {
                QString path = QString("p%1/file_%2.txt").arg(i).arg(j);
                m_queue->enqueue(makeAction(SyncActionType::Upload, path));
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : producers) {
        thread.join();
    }

    QCOMPARE(produced.load(), totalItems);
    QCOMPARE(m_queue->count(), totalItems);

    QSet<QString> seen;
    for (int i = 0; i < totalItems; ++i) {
        SyncActionItem item = m_queue->dequeue();
        QVERIFY(item.isValid());
        QVERIFY(!seen.contains(item.localPath));
        seen.insert(item.localPath);
    }

    QCOMPARE(seen.count(), totalItems);
    QVERIFY(m_queue->isEmpty());
}

QTEST_MAIN(TestSyncActionQueue)
#include "TestSyncActionQueue.moc"
