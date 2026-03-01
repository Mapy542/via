# Via Unit Tests

This directory contains unit tests for the Via sync subsystem using **Qt Test** framework with **CTest** integration.

## Directory Structure

```
tests/
└── sync/                          # Sync subsystem tests
    ├── TestChangeQueue.cpp        # Queue operations and thread safety
    ├── TestChangeProcessor.cpp    # Change classification and conflicts
    ├── TestSyncDatabase.cpp       # Database CRUD and migrations
    ├── TestSyncActionQueue.cpp    # Action queue management
    ├── TestSyncActionThread.cpp   # Action thread wake/execution
    ├── TestLocalChangeWatcher.cpp # Filesystem event detection
    ├── TestRemoteChangeWatcher.cpp# Drive API change polling
    ├── TestInitialSync.cpp        # Full sync process
    └── TestFileMetadataHelper.cpp # Metadata extraction and comparison
```

## Setup

### Prerequisites

- Qt 6 with Test module (`Qt6::Test`)
- CMake 3.20+
- GCC/Clang with C++20 support

### Building Tests

```bash
# From project root
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build build --parallel
```

To disable tests (e.g., for release builds):

```bash
cmake -B build -DBUILD_TESTS=OFF
```

## Running Tests

### Run All Tests

```bash
cd build
ctest --output-on-failure
```

### Run a Specific Test

```bash
# By test name
ctest -R test_ChangeQueue --output-on-failure

# Or run the executable directly for verbose output
./build/test_SyncDatabase -v2
```

### Run Tests Matching a Pattern

```bash
ctest -R "test_.*Watcher" --output-on-failure
```

### List Available Tests

```bash
ctest -N
```

## Writing Tests

### Test File Template

Each test file follows the Qt Test pattern:

```cpp
#include <QtTest/QtTest>
#include "sync/YourComponent.h"

class TestYourComponent : public QObject {
    Q_OBJECT

private slots:
    void init();      // Runs before EACH test
    void cleanup();   // Runs after EACH test

    void testFeatureA();
    void testFeatureB();
};

void TestYourComponent::init() {
    // Set up test fixtures
}

void TestYourComponent::cleanup() {
    // Tear down
}

void TestYourComponent::testFeatureA() {
    QVERIFY(condition);
    QCOMPARE(actual, expected);
}

void TestYourComponent::testFeatureB() {
    QSKIP("Not implemented yet");  // Skip placeholder tests
}

QTEST_MAIN(TestYourComponent)
#include "TestYourComponent.moc"
```

### Adding a New Test File

1. Create `tests/sync/TestNewComponent.cpp`
2. Add to `CMakeLists.txt`:
    ```cmake
    add_qt_test(test_NewComponent tests/sync/TestNewComponent.cpp)
    ```
3. Rebuild: `cmake --build build`

### Useful Qt Test Macros

| Macro                                  | Purpose                          |
| -------------------------------------- | -------------------------------- |
| `QVERIFY(condition)`                   | Assert condition is true         |
| `QCOMPARE(actual, expected)`           | Assert equality with diff output |
| `QSKIP("reason")`                      | Skip test with message           |
| `QFAIL("message")`                     | Fail immediately                 |
| `QEXPECT_FAIL("", "reason", Continue)` | Mark expected failure            |
| `QBENCHMARK { ... }`                   | Benchmark code block             |
| `QSignalSpy spy(obj, &Class::signal)`  | Capture signal emissions         |

### Testing with Temporary Files

```cpp
#include <QTemporaryDir>

void TestExample::testWithTempDir() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QString testFile = tempDir.filePath("test.txt");
    // ... use testFile
    // Directory auto-deleted when tempDir goes out of scope
}
```

### Testing Signals

```cpp
void TestExample::testSignalEmitted() {
    QSignalSpy spy(&myObject, &MyClass::someSignal);

    myObject.triggerAction();

    QCOMPARE(spy.count(), 1);
    QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), "expected");
}
```

## Implementing Placeholder Tests

Current tests use `QSKIP("Not implemented yet")`. To implement:

1. Remove the `QSKIP` line
2. Set up necessary mocks/fixtures in `init()`
3. Write test logic using `QVERIFY`/`QCOMPARE`
4. Clean up in `cleanup()`

### Example: Implementing a SyncDatabase Test

```cpp
void TestSyncDatabase::testInsertFileMetadata() {
    // QSKIP("Not implemented yet");  // Remove this

    SyncDatabase db(m_tempDir->filePath("test.db"));
    QVERIFY(db.open());

    FileMetadata meta;
    meta.driveId = "abc123";
    meta.localPath = "/home/user/test.txt";
    meta.mimeType = "text/plain";

    QVERIFY(db.insertFile(meta));

    auto retrieved = db.getFileByDriveId("abc123");
    QVERIFY(retrieved.has_value());
    QCOMPARE(retrieved->localPath, meta.localPath);
}
```

## Test Categories

Tests are organized by sync subsystem component. Each maps to TODOs in the codebase:

| Test File               | Related TODOs                     |
| ----------------------- | --------------------------------- |
| TestChangeProcessor     | ChangeProcessor.cpp:293, 306, 393 |
| TestSyncDatabase        | SyncDatabase.h:130, 175, 212, 379 |
| TestLocalChangeWatcher  | LocalChangeWatcher.h:94           |
| TestRemoteChangeWatcher | RemoteChangeWatcher.cpp:272, 298  |
| TestInitialSync         | InitialSync.cpp:23, 437, 481, 484 |

## Future: CI/CD Integration

The test suite is designed for future GitHub Actions integration:

```yaml
# .github/workflows/test.yml (future)
name: Tests
on: [push, pull_request]
jobs:
    test:
        runs-on: ubuntu-latest
        steps:
            - uses: actions/checkout@v4
            - name: Install Qt
              uses: jurplel/install-qt-action@v3
              with:
                  version: '6.6.*'
                  modules: 'qtnetworkauth'
            - name: Configure
              run: cmake -B build -DBUILD_TESTS=ON
            - name: Build
              run: cmake --build build --parallel
            - name: Test
              run: ctest --test-dir build --output-on-failure
```

## Troubleshooting

### "No tests found"

Ensure tests are enabled: `cmake -DBUILD_TESTS=ON`

### Qt platform plugin errors

Tests run with `QT_QPA_PLATFORM=offscreen` (set in CMakeLists.txt). If issues persist:

```bash
QT_QPA_PLATFORM=offscreen ./build/test_SyncDatabase
```

### MOC errors

Ensure `CMAKE_AUTOMOC` is ON and the `.moc` include matches the filename:

```cpp
// In TestFoo.cpp:
#include "TestFoo.moc"  // Must match filename
```

### Linking errors

If tests fail to link, check that all dependencies are in `via_testable` library in CMakeLists.txt.
