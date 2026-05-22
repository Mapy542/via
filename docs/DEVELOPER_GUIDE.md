# Via — Developer Guide

This guide covers the project layout, build system, tooling, architecture, threading model, and conventions for developers contributing to Via.

## Quick Start

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install build-essential cmake pkg-config \
    qt6-base-dev qt6-networkauth-dev libqt6sql6-sqlite \
    libfuse3-dev libsecret-1-dev libdbus-1-dev qtkeychain-qt6-dev

# Configure, build, and test
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Or use the VS Code tasks (Ctrl+Shift+B for the default build).

### Debian Packaging Prerequisites

Local Debian package builds use `scripts/make-deb.sh`, which stages a clean source copy under `build-deb/` and runs `dpkg-buildpackage` without mutating the working tree.

```bash
sudo apt-get install build-essential cmake debhelper desktop-file-utils \
    dpkg-dev lintian pkgconf qt6-base-dev qt6-networkauth-dev \
    libqt6sql6-sqlite libfuse3-dev libgl1-mesa-dev qtkeychain-qt6-dev

./scripts/make-deb.sh
```

The script validates the desktop entry, runs `lintian`, inspects the resulting package with `dpkg-deb`, and writes `.deb`, `.changes`, and `.buildinfo` artifacts to `build-deb/artifacts/`.

---

## Project Layout

```
via/
├── CMakeLists.txt              # Top-level build configuration
├── VERSION                     # Checked-in application/release version
├── CTestCustom.cmake           # CTest output settings (disables truncation)
├── scripts/
│   ├── make-appimage.sh        # Local AppImage builder
│   └── make-deb.sh             # Local Debian package builder
├── src/
│   ├── main.cpp                # Application entry point
│   ├── api/                    # Google Drive REST API layer
│   ├── auth/                   # OAuth 2.0 authentication
│   ├── fuse/                   # FUSE virtual filesystem
│   ├── sync/                   # Bidirectional sync engine
│   ├── ui/                     # Qt Widgets UI
│   └── utils/                  # Cross-cutting utilities
├── tests/
│   ├── sync/                   # Sync subsystem unit tests
│   ├── fuse/                   # FUSE subsystem unit tests
│   ├── ui/                     # UI regression tests
│   └── utils/                  # Utility and startup-policy tests
├── res/                        # Qt resources, icons, .desktop file
├── docs/                       # Documentation
└── .github/workflows/          # CI/CD pipelines
```

---

## Source Modules

### `src/api/` — Google Drive API Client

| File                       | Purpose                                                         |
| -------------------------- | --------------------------------------------------------------- |
| `GoogleDriveClient.h/.cpp` | REST API wrapper — download, upload, move, delete, list changes |
| `DriveFile.h/.cpp`         | Data structure for Google Drive file metadata                   |
| `DriveChange.h/.cpp`       | Data structure for Drive Change notifications                   |

The API client is a `QObject` that uses `QNetworkAccessManager` for HTTP requests. All network calls are asynchronous and signal results via Qt signals.

### `src/auth/` — Authentication

| File                       | Purpose                                                                |
| -------------------------- | ---------------------------------------------------------------------- |
| `GoogleAuthManager.h/.cpp` | OAuth 2.0 authorization code flow using `QOAuth2AuthorizationCodeFlow` |
| `TokenStorage.h/.cpp`      | Secure storage for OAuth tokens and client credentials via QtKeychain with a 0600 file fallback |

**Qt version notes:** `setTokenUrl()` and `serverReportedErrorOccurred()` are only available in Qt 6.9+. These calls are guarded with `#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)`.

On Linux, QtKeychain typically uses libsecret or KWallet. If no supported keyring backend is available, Via falls back to `secure_tokens.json` under `QStandardPaths::AppDataLocation`.

### `src/sync/` — Synchronization Engine

| File                         | Purpose                                                                                                                                                                                       |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `SyncDatabase.h/.cpp`        | SQLite database for tracking file sync state, FUSE metadata, cache entries, dirty files, conflicts, and schema compatibility state. Thread-safe via `QRecursiveMutex`. Uses WAL journal mode. |
| `ChangeQueue.h/.cpp`         | Thread-safe queue for pending changes                                                                                                                                                         |
| `ChangeProcessor.h/.cpp`     | Classifies changes and detects conflicts                                                                                                                                                      |
| `LocalChangeWatcher.h/.cpp`  | Monitors local filesystem for changes via `QFileSystemWatcher`                                                                                                                                |
| `RemoteChangeWatcher.h/.cpp` | Polls Google Drive changes API                                                                                                                                                                |
| `SyncActionQueue.h/.cpp`     | Prioritized queue of sync actions                                                                                                                                                             |
| `SyncActionThread.h/.cpp`    | Worker thread that executes sync actions                                                                                                                                                      |
| `FullSync.h/.cpp`            | Full reconciliation pass (initial sync)                                                                                                                                                       |
| `FileFilter.h/.cpp`          | File/folder ignore rules                                                                                                                                                                      |
| `SyncSettings.h/.cpp`        | Shared runtime sync policy for mirror and FUSE, including sync mode, conflict policy, duplicate naming, native-doc mode, and mirror duty-cycle controls                                                   |

### `src/fuse/` — Virtual Filesystem

| File                           | Purpose                                                                                        |
| ------------------------------ | ---------------------------------------------------------------------------------------------- |
| `FuseDriver.h/.cpp`            | FUSE3 callback implementation — `getattr`, `readdir`, `open`, `read`, `write`, `release`, etc., plus direct sync-mode enforcement for remote mutations |
| `FileCache.h/.cpp`             | LRU disk cache for downloaded files; `getExportedPath()` for native doc exports                |
| `MetadataCache.h/.cpp`         | In-memory + DB-backed metadata cache with `QReadWriteLock` and atomic hit/miss counters        |
| `NativeDocPolicy.h`            | Header-only policy: maps (MIME type, NativeDocMode) → visibility, extension, export MIME type  |
| `DirtySyncWorker.h/.cpp`       | Background worker that uploads locally-modified FUSE files back to Drive                       |
| `MetadataRefreshWorker.h/.cpp` | Background worker that periodically refreshes metadata from Drive                              |

### `src/ui/` — User Interface

| File                       | Purpose                           |
| -------------------------- | --------------------------------- |
| `MainWindow.h/.cpp`        | Main application window and settings-reload wiring into active runtimes |
| `SettingsWindow.h/.cpp`    | Settings dialog with Login, Mirror, Common, Fuse, and Misc tabs |
| `SystemTrayManager.h/.cpp` | System tray icon and context menu |
| `ConflictDialog.h/.cpp`    | Conflict resolution UI            |

### `src/utils/` — Utilities

| File                         | Purpose                                                            |
| ---------------------------- | ------------------------------------------------------------------ |
| `LogManager.h/.cpp`          | Application logging to `~/.local/share/Via/logs/`                  |
| `NotificationManager.h/.cpp` | Desktop notifications via DBus                                     |
| `StartupMaintenance.h/.cpp`  | Startup compatibility policy helpers for sync-state and FUSE cache |
| `FileInUseChecker.h/.cpp`    | Checks if files are open by other processes                        |
| `AutostartManager.h`         | Header-only — manages XDG autostart `.desktop` entries             |
| `UpdateChecker.h`            | Header-only — checks GitHub Releases API for new versions          |
| `ThemeHelper.h`              | Header-only — light/dark theme detection (Qt 6.5+ `colorScheme()`) |

---

## Threading Model

Via uses four primary threads. Understanding which thread owns which data is critical for avoiding races.

```
┌──────────────────────────────────────────────────────────┐
│                      Main / Qt Thread                    │
│  MainWindow, SettingsWindow, SystemTrayManager,          │
│  GoogleAuthManager, LocalChangeWatcher,                  │
│  RemoteChangeWatcher, ChangeProcessor, SyncActionThread  │
└────────────────────────┬─────────────────────────────────┘
                         │ signals/slots
        ┌────────────────┼────────────────────┐
        ▼                ▼                    ▼
┌───────────────┐ ┌──────────────┐ ┌─────────────────────┐
│  FUSE Thread  │ │ DirtySync    │ │ MetadataRefresh      │
│  (QThread)    │ │ Thread       │ │ Thread               │
│               │ │ (QThread)    │ │ (QThread)            │
│ Static FUSE   │ │              │ │                      │
│ callbacks:    │ │ DirtySyncWo- │ │ MetadataRefreshWo-   │
│ fuseRead,     │ │ rker uploads │ │ rker polls Drive     │
│ fuseWrite,    │ │ modified     │ │ for metadata         │
│ fuseGetattr,  │ │ files to     │ │ updates              │
│ fuseRelease,  │ │ Drive        │ │                      │
│ etc.          │ │              │ │                      │
└───────┬───────┘ └──────┬───────┘ └──────────┬───────────┘
        │                │                    │
        ▼                ▼                    ▼
   ┌─────────────────────────────────────────────┐
   │            SyncDatabase (shared)            │
   │  Protected by QRecursiveMutex               │
   │  SQLite with WAL journal mode               │
   └─────────────────────────────────────────────┘
```

### Thread Safety Mechanisms

| Resource                  | Protection                                                                         | Notes                                                                                                       |
| ------------------------- | ---------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `SyncDatabase`            | `QRecursiveMutex` on every public method                                           | Recursive because `updateFuseChildrenPaths()` calls `getFuseChildren()` and `saveFuseMetadata()` internally |
| `FuseDriver::m_openFiles` | `QMutex m_openFilesMutex`                                                          | `getOpenFile()` returns `std::optional<FuseOpenFile>` (copy, not pointer) to prevent dangling references    |
| `MetadataCache`           | `QReadWriteLock` for the cache map; `QAtomicInteger<qint64>` for hit/miss counters | Readers can proceed in parallel; writers get exclusive access                                               |
| `ChangeQueue`             | Internal `QMutex`                                                                  | Thread-safe enqueue/dequeue                                                                                 |
| `SyncActionQueue`         | Internal `QMutex`                                                                  | Thread-safe priority queue                                                                                  |

### Key Rule

**Never hold two locks at once** unless the ordering is strictly defined. Currently `SyncDatabase`'s recursive mutex is the only lock that can be re-entered.

---

## Build System

### CMake Configuration

| Option                 | Default | Effect                                                 |
| ---------------------- | ------- | ------------------------------------------------------ |
| `CMAKE_BUILD_TYPE`     | —       | `Debug` (dev) or `Release` (distribution)              |
| `BUILD_TESTS`          | `ON`    | Builds unit tests and enables CTest                    |
| `VIA_VERSION_OVERRIDE` | empty   | Optional one-off override for the resolved app version |

The build produces:

- `build/via` — main application binary
- `build/test_*` — individual test executables (one per test file)
- `build/compile_commands.json` — for IDE/clangd integration
- `build/via-version.txt` — resolved version used by the binary and packaging

### Version Source

`VERSION` at the repository root is the authoritative checked-in version for runtime metadata, update checks, and release packaging.

- CMake reads `VERSION` before `project(...)` and uses it as `PROJECT_VERSION`
- The resolved version is written to `build/via-version.txt` and injected into the binary via `build/generated/ViaVersion.h`
- Normal local builds need no extra flags
- `-DVIA_VERSION_OVERRIDE=<x.y.z>` is available for one-off source builds and CI experiments, but releases should update `VERSION` in a normal commit instead of overriding it in the workflow
- Release packaging derives the generated Debian changelog body and GitHub release notes from non-merge commit subjects since the previous `v*` tag

### CMake Conventions

- **AUTOMOC / AUTORCC / AUTOUIC** are enabled — no manual `qt_wrap_cpp` calls needed
- Test executables link against `via_testable`, a static library containing all source (excluding `main.cpp`)
- New test files only need one line in CMakeLists.txt: `add_qt_test(test_Name tests/path/TestName.cpp)`

### Dependencies

| Dependency   | Version                                   | Purpose                              |
| ------------ | ----------------------------------------- | ------------------------------------ |
| CMake        | ≥ 3.20                                    | Build system                         |
| C++ compiler | C++20                                     | GCC 10+ or Clang 10+                 |
| Qt 6         | ≥ 6.2 (CI uses 6.7.3, local may use 6.9+) | UI, networking, SQL, auth, threading |
| Qt6Keychain  | `qtkeychain-qt6-dev`                      | OS keyring integration for tokens and credentials |
| FUSE 3       | `libfuse3-dev`                            | Virtual filesystem                   |
| pkg-config   | —                                         | Finding FUSE3                        |

**Qt modules used:** Core, Gui, Widgets, Network, NetworkAuth, Sql, DBus, Concurrent, Test

---

## VS Code Integration

The `.vscode/` directory provides a ready-to-use development environment.

### Tasks (`tasks.json`)

Run via **Terminal → Run Task** or **Ctrl+Shift+B** (build):

| Task                    | Shortcut         | Description                                            |
| ----------------------- | ---------------- | ------------------------------------------------------ |
| CMake: Configure        | —                | Runs `cmake -B build` with Debug + tests               |
| **CMake: Build**        | **Ctrl+Shift+B** | Incremental build (default task, depends on Configure) |
| CMake: Clean            | —                | Cleans build artifacts                                 |
| CMake: Rebuild          | —                | Clean + build                                          |
| Run All Tests           | —                | Builds then runs `ctest`                               |
| Run All Tests (Verbose) | —                | Same with `-V` flag                                    |
| List Available Tests    | —                | Shows registered CTest tests                           |
| Make AppImage           | —                | Builds a local AppImage via `scripts/make-appimage.sh` |
| Make Debian Package     | —                | Builds a local `.deb` via `scripts/make-deb.sh`        |

### Debug Configurations (`launch.json`)

| Configuration    | Debugger | Notes                                           |
| ---------------- | -------- | ----------------------------------------------- |
| Debug Via        | GDB      | Sets `QT_QPA_PLATFORM=xcb`, runs pre-build task |
| Debug Via (lldb) | LLDB     | Same but uses CodeLLDB extension                |

Both pre-launch the build task so you always debug fresh code.

### Extensions (`extensions.json`)

Recommended extensions are listed in `.vscode/extensions.json`. Install them for the best experience.

---

## Testing

### Framework

Tests use **Qt Test** (`QTest`) with **CTest** as the runner. Each test file is a standalone executable with its own `main()` via `QTEST_MAIN()`.

### Test Structure

```
tests/
├── sync/
│   ├── TestChangeQueue.cpp         # Queue thread safety
│   ├── TestChangeProcessor.cpp     # Change classification, conflict detection
│   ├── TestSyncDatabase.cpp        # Database CRUD, compatibility/reset policy, concurrent access
│   ├── TestSyncActionQueue.cpp     # Action queue management
│   ├── TestSyncActionThread.cpp    # Action thread wake/execution
│   ├── TestLocalChangeWatcher.cpp  # Filesystem event detection
│   ├── TestRemoteChangeWatcher.cpp # Drive API change polling
│   └── TestFullSync.cpp            # Full sync reconciliation
├── fuse/
│   ├── TestFileCache.cpp           # LRU cache operations
│   ├── TestDirtySyncWorker.cpp     # Dirty file upload logic
│   └── TestMetadataCache.cpp       # Metadata cache CRUD + persistence
└── utils/
    └── TestStartupMaintenance.cpp  # Startup compatibility policy decisions
```

### Running Tests

```bash
# All tests
ctest --test-dir build --output-on-failure

# Specific test
ctest --test-dir build -R test_SyncDatabase --output-on-failure

# Directly with verbose Qt output
./build/test_SyncDatabase -v2

# Run specific test methods
./build/test_SyncDatabase testConcurrentReadWrite_NoCorruption -v2

# List tests without running
ctest --test-dir build -N
```

All tests run with `QT_QPA_PLATFORM=offscreen` (set automatically in CMakeLists.txt).

Compatibility-policy coverage lives in:

- `tests/sync/TestSyncDatabase.cpp` — current schema adoption, incompatible legacy DB detection, dirty-state reset guard, explicit rebuild path
- `tests/utils/TestStartupMaintenance.cpp` — startup policy decisions so normal app-version changes do not trigger destructive resets while explicit epoch changes do

### Writing a New Test

1. Create `tests/<subsystem>/TestNewComponent.cpp`:

```cpp
#include <QtTest/QtTest>
#include "sync/NewComponent.h"

class TestNewComponent : public QObject {
    Q_OBJECT

private slots:
    void init();                 // Before each test
    void cleanup();              // After each test

    void testBasicOperation();
    void testEdgeCase();

private:
    NewComponent* m_component = nullptr;
};

void TestNewComponent::init() {
    m_component = new NewComponent();
}

void TestNewComponent::cleanup() {
    delete m_component;
    m_component = nullptr;
}

void TestNewComponent::testBasicOperation() {
    QCOMPARE(m_component->doThing(), expectedResult);
}

void TestNewComponent::testEdgeCase() {
    QVERIFY(!m_component->doThing().isEmpty());
}

QTEST_MAIN(TestNewComponent)
#include "TestNewComponent.moc"
```

2. Register it in `CMakeLists.txt`:

```cmake
add_qt_test(test_NewComponent tests/sync/TestNewComponent.cpp)
```

3. Rebuild and run:

```bash
cmake --build build --parallel
ctest --test-dir build -R test_NewComponent --output-on-failure
```

### Concurrency Tests

The `TestSyncDatabase` suite includes stress tests that spawn multiple `QtConcurrent::run` threads performing interleaved reads and writes. These validate the mutex protection on `SyncDatabase`. To write similar tests:

```cpp
#include <QtConcurrent>
#include <QAtomicInt>

void TestMyClass::testConcurrentAccess() {
    QAtomicInt errors(0);
    QList<QFuture<void>> futures;

    for (int t = 0; t < 4; t++) {
        futures.append(QtConcurrent::run([&]() {
            for (int i = 0; i < 50; i++) {
                // Do concurrent operations, check invariants
                if (/* invariant broken */) errors.fetchAndAddRelaxed(1);
            }
        }));
    }

    for (auto& f : futures) f.waitForFinished();
    QCOMPARE(errors.loadRelaxed(), 0);
}
```

---

## CI/CD

### Workflows (`.github/workflows/`)

| Workflow            | Trigger                                 | Purpose                                                                                    |
| ------------------- | --------------------------------------- | ------------------------------------------------------------------------------------------ |
| `build.yml`         | Push to `main`/`develop`, PRs to `main` | Build the project on Ubuntu with Qt 6.7.3                                                  |
| `release.yml`       | Push `v*` tag                           | Validate tag vs `VERSION`, build release binary, package AppImages, publish GitHub Release |
| `todo-to-issue.yml` | —                                       | Converts `TODO` comments to GitHub issues                                                  |

### CI Qt Version

CI uses **Qt 6.7.3** via [`jurplel/install-qt-action@v4`](https://github.com/jurplel/install-qt-action). Local development may use a newer Qt (e.g., 6.9). Any APIs available only in 6.9+ must be guarded:

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    oauth->setTokenUrl(tokenUrl);
#endif
```

Similarly, APIs from 6.5+ (e.g., `QStyleHints::colorScheme()`) need guards:

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Use colorScheme()
#endif
```

---

## AppImage Packaging

### Local Build

```bash
./scripts/make-appimage.sh
```

This script:

1. Downloads `linuxdeploy` + Qt plugin to `build-appimage/tools/` (cached)
2. Runs a Release CMake build into `build-appimage/`
3. Installs to an AppDir structure
4. Runs `linuxdeploy --plugin qt` to bundle all Qt libraries/plugins
5. Reads `build-appimage/via-version.txt` and renames the artifact to `Via-<version>-<arch>.AppImage`

Optional one-off version override:

```bash
VIA_VERSION_OVERRIDE=1.2.3 ./scripts/make-appimage.sh
```

### CI Release Build

The `release.yml` workflow validates the pushed tag (or manual dispatch version) against the checked-in `VERSION` file and fails before publishing if they do not match.

Recommended release flow:

1. Update `VERSION` in a normal commit.
2. Push that commit.
3. Tag the same commit with `v<version>`.
4. Push the tag and let CI verify the tag matches `VERSION`.

Example:

```bash
git commit -am "Bump version to 1.0.0"
git push origin main
git tag v1.0.0
git push origin v1.0.0
```

---

## Database Schema

`SyncDatabase` manages a single SQLite database with WAL mode enabled. Key tables:

| Table                | Purpose                                                         |
| -------------------- | --------------------------------------------------------------- |
| `files`              | Local path ↔ Drive file ID mapping, checksums, sync timestamps  |
| `deleted_files`      | Tracks recently deleted files to avoid re-downloading           |
| `conflicts`          | Conflict records with versions                                  |
| `fuse_metadata`      | FUSE file/folder metadata cache                                 |
| `fuse_cache_entries` | File cache tracking (path, size, last access)                   |
| `fuse_dirty_files`   | Files modified via FUSE pending upload                          |
| `settings`           | Key-value settings (change token, schema epoch, legacy version) |

### Compatibility Policy

Via now separates three different kinds of versioning:

- **Application version** comes from the repository-root `VERSION` file and is used for runtime metadata, update checks, and release packaging.
- **Sync-state schema compatibility** is tracked inside the SQLite `settings` table as `sync_schema_epoch`.
- **FUSE representation/cache compatibility** is tracked in `QSettings` as `advanced/lastAppliedFuseRepresentationEpoch`.

These values are intentionally independent. A normal app update should not wipe sync state just because the binary version changed.

### Current Sync-State Keys

- `settings.sync_schema_epoch` — authoritative compatibility epoch for destructive sync-state decisions. Current value in code: `1`.
- `settings.version` — legacy numeric schema version. Current value in code: `6`.

`settings.version` is still written so current databases can be recognized and older installations can be adopted, but destructive startup decisions should key off `sync_schema_epoch`, not the app version.

### Startup Flow

`SyncDatabase::initialize()` follows this process:

1. Open the DB, enable WAL mode, and ensure the `settings` table exists.
2. Read `settings.sync_schema_epoch`.
3. If the stored epoch matches the code constant, startup proceeds normally.
4. If the epoch is missing, fall back to `settings.version`.
5. If the fallback version is the current baseline (`6`), Via adopts that DB as current and writes `sync_schema_epoch=1` on successful startup.
6. If the stored epoch or fallback legacy version is newer than the code supports, startup fails with a future-schema error.
7. If the stored schema is older than the current compatibility epoch, `initialize()` reports an incompatible schema instead of walking a migration ladder.

### Incompatible Reset Behavior

The pre-1.0 migration ladder was removed. Incompatible legacy schemas are handled through an explicit rebuild flow instead:

- `SyncDatabase` reports a `SchemaCompatibility` state.
- `main.cpp` maps that state through `StartupMaintenance::classifySyncReset()`.
- If the DB is incompatible and there are no pending dirty uploads, Via prompts the user to rebuild local sync metadata.
- If `fuse_dirty_files` still contains pending uploads, Via requires an explicit **Discard And Rebuild** choice and will not silently reset.
- The rebuild path uses `SyncDatabase::recreateCurrentSchema()` rather than `clearAllData()`.
- Rebuilding preserves the local sync folder, recreates the current schema baseline, and then triggers an immediate mirror full sync so local disk state and Google Drive are re-indexed together.

### Sign-Out vs Incompatible Upgrade

These flows are different on purpose:

- `clearAllData()` is for sign-out cleanup. It clears user/sync rows but preserves `settings.version` and `settings.sync_schema_epoch` so sign-out does not look like a schema break.
- `recreateCurrentSchema()` is for incompatible startup recovery. It recreates the database file and writes the current schema metadata from scratch.

### FUSE Representation Epoch

FUSE representation/cache compatibility is tracked separately from sync-state compatibility.

- Code constant: `kCurrentFuseRepresentationEpoch` in `main.cpp`
- Stored value: `advanced/lastAppliedFuseRepresentationEpoch` in `QSettings`

At startup, Via purges FUSE representation state when any of the following are true:

- native-doc mode changed
- `advanced/pendingFuseRepresentationReset` is set
- `advanced/pendingCachePurge` is set
- the stored FUSE representation epoch is older than the current code constant

That purge goes through `CacheMaintenance::purgeFuseRepresentationCache()`, which clears FUSE representation tables and evictable cache files but preserves pending dirty uploads.

### When To Bump An Epoch

- Bump `sync_schema_epoch` only when existing DB contents are no longer safe or meaningful to reuse and Via must rebuild sync state.
- Bump `kCurrentFuseRepresentationEpoch` when cached FUSE metadata/exported representations become incompatible but the core mirror-sync state is still safe to keep.
- Do **not** bump either epoch for ordinary app releases that do not change compatibility.

---

## Coding Conventions

### General

- **C++20** — use `std::optional`, structured bindings, `auto` where clear
- **Qt idioms** — `QObject` ownership, signals/slots, `Q_OBJECT` macro
- `#include` order: Qt headers → project headers → STL headers
- One class per `.h/.cpp` pair (header-only for simple utilities)

### Thread Safety

- Every public method on `SyncDatabase` acquires `QMutexLocker locker(&m_mutex)` as its first statement
- FUSE `getOpenFile()` returns `std::optional<FuseOpenFile>` (value copy), never a pointer into the map
- Use `QAtomicInteger` for simple counters accessed from multiple threads
- Prefer `QReadWriteLock` when reads vastly outnumber writes (e.g., MetadataCache)

### Error Handling

- Database getters return empty/invalid structs on miss (no exceptions)
- Path validation rejects absolute paths where relative paths are expected
- Failing SQLite queries log warnings via `qWarning()` and return gracefully

### Naming

- Classes: `PascalCase` (e.g., `SyncDatabase`, `MetadataCache`)
- Methods: `camelCase` (e.g., `getFileState`, `saveFuseMetadata`)
- Members: `m_camelCase` (e.g., `m_mutex`, `m_openFiles`)
- Test methods: `testDescriptiveName` (e.g., `testConcurrentReadWrite_NoCorruption`)
- Test files: `TestClassName.cpp` matching the class under test

---

## Useful Commands

```bash
# Full rebuild from scratch
cmake --build build --clean-first --parallel

# Run a single test with full output
./build/test_SyncDatabase -v2

# Run tests matching a pattern
ctest --test-dir build -R "test_.*Cache" --output-on-failure

# Build AppImage locally
./scripts/make-appimage.sh

# Build with a one-off version override
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DVIA_VERSION_OVERRIDE=1.2.3

# Check for compile_commands.json (for clangd/IDE)
ls build/compile_commands.json
```

---

## Resources

- [Qt 6 Documentation](https://doc.qt.io/qt-6/)
- [FUSE 3 API](https://libfuse.github.io/doxygen/)
- [Google Drive API v3](https://developers.google.com/drive/api/v3/reference)
- [Qt Test Framework](https://doc.qt.io/qt-6/qtest-overview.html)
- [User Guide](USER_GUIDE.md)
