/**
 * @file TestLogManager.cpp
 * @brief Unit tests for LogManager singleton behavior
 */

#include <latch>
#include <thread>
#include <vector>

#include <QtTest/QtTest>

#include "utils/LogManager.h"

class TestLogManager : public QObject {
    Q_OBJECT

   private slots:
    void testInstanceIsSharedAcrossThreads();
};

void TestLogManager::testInstanceIsSharedAcrossThreads() {
    constexpr int threadCount = 8;

    std::latch ready(threadCount);
    std::latch start(1);
    std::vector<LogManager*> instances(threadCount, nullptr);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int index = 0; index < threadCount; ++index) {
        threads.emplace_back([&, index]() {
            ready.count_down();
            start.wait();
            instances[index] = &LogManager::instance();
        });
    }

    ready.wait();
    start.count_down();

    for (std::thread& thread : threads) {
        thread.join();
    }

    LogManager* expected = instances.front();
    QVERIFY(expected != nullptr);

    for (LogManager* instance : instances) {
        QCOMPARE(instance, expected);
    }
}

QTEST_MAIN(TestLogManager)
#include "TestLogManager.moc"