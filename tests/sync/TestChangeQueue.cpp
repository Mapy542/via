/**
 * @file TestChangeQueue.cpp
 * @brief Unit tests for ChangeQueue subsystem
 *
 * Tests cover: queue operations, thread safety, and edge cases.
 */

#include <QSignalSpy>
#include <QtTest/QtTest>
#include <atomic>
#include <thread>
#include <vector>

#include "sync/ChangeQueue.h"

class TestChangeQueue : public QObject {
    Q_OBJECT

   private slots:
    // Setup/teardown run before/after each test
    void init();
    void cleanup();

    // Basic functionality tests
    void testEnqueueDequeue();
    void testQueueEmpty();
    void testQueueOrdering();

    // Thread safety (placeholder for future stress tests)
    void testConcurrentAccess();

    // Edge cases
    void testLargeQueue();
    void testClearQueue();

   private:
    // Test fixtures
    ChangeQueue* m_queue = nullptr;

    static ChangeQueueItem makeItem(const QString& path, const QString& fileId = QString());
};

void TestChangeQueue::init() { m_queue = new ChangeQueue(); }

void TestChangeQueue::cleanup() {
    delete m_queue;
    m_queue = nullptr;
}

ChangeQueueItem TestChangeQueue::makeItem(const QString& path, const QString& fileId) {
    ChangeQueueItem item;
    item.changeType = ChangeType::Modify;
    item.origin = ChangeOrigin::Local;
    item.localPath = path;
    item.fileId = fileId;
    item.detectedTime = QDateTime::currentDateTime();
    item.modifiedTime = QDateTime::currentDateTime();
    return item;
}

void TestChangeQueue::testEnqueueDequeue() {
    ChangeQueueItem item = makeItem("file.txt");
    m_queue->enqueue(item);

    QCOMPARE(m_queue->count(), 1);
    ChangeQueueItem dequeued = m_queue->dequeue();
    QVERIFY(dequeued.isValid());
    QCOMPARE(dequeued.localPath, QString("file.txt"));
    QCOMPARE(m_queue->count(), 0);
}

void TestChangeQueue::testQueueEmpty() {
    QVERIFY(m_queue->isEmpty());

    ChangeQueueItem dequeued = m_queue->dequeue();
    QVERIFY(!dequeued.isValid());

    ChangeQueueItem peeked = m_queue->peek();
    QVERIFY(!peeked.isValid());

    QSignalSpy itemsAvailableSpy(m_queue, &ChangeQueue::itemsAvailable);
    ChangeQueueItem invalidItem;
    invalidItem.origin = ChangeOrigin::Local;
    invalidItem.changeType = ChangeType::Modify;
    m_queue->enqueue(invalidItem);
    QCOMPARE(m_queue->count(), 0);
    QCOMPARE(itemsAvailableSpy.count(), 0);
}

void TestChangeQueue::testQueueOrdering() {
    m_queue->enqueue(makeItem("a.txt"));
    m_queue->enqueue(makeItem("b.txt"));
    m_queue->enqueue(makeItem("c.txt"));

    QCOMPARE(m_queue->dequeue().localPath, QString("a.txt"));
    QCOMPARE(m_queue->dequeue().localPath, QString("b.txt"));
    QCOMPARE(m_queue->dequeue().localPath, QString("c.txt"));
    QVERIFY(m_queue->isEmpty());
}

void TestChangeQueue::testConcurrentAccess() {
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
                m_queue->enqueue(makeItem(path));
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
        ChangeQueueItem item = m_queue->dequeue();
        QVERIFY(item.isValid());
        QVERIFY(!seen.contains(item.localPath));
        seen.insert(item.localPath);
    }

    QCOMPARE(seen.count(), totalItems);
    QVERIFY(m_queue->isEmpty());
}

void TestChangeQueue::testLargeQueue() {
    const int count = 2000;
    for (int i = 0; i < count; ++i) {
        m_queue->enqueue(makeItem(QString("bulk/%1.txt").arg(i)));
    }

    QCOMPARE(m_queue->count(), count);
    QCOMPARE(m_queue->peek().localPath, QString("bulk/0.txt"));
}

void TestChangeQueue::testClearQueue() {
    m_queue->enqueue(makeItem("a.txt"));
    m_queue->enqueue(makeItem("b.txt"));

    QSignalSpy queueEmptySpy(m_queue, &ChangeQueue::queueEmpty);
    m_queue->clear();

    QVERIFY(m_queue->isEmpty());
    QCOMPARE(queueEmptySpy.count(), 1);
}

QTEST_MAIN(TestChangeQueue)
#include "TestChangeQueue.moc"
