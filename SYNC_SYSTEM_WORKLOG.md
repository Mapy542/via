Sync System TODO Worklog — 2026-01-25

Scope: All TODOs found under src/sync and archive/sync. No code changes made; this worklog identifies issues and proposes solutions.

## CRITICAL BUGS DISCOVERED VIA TESTING

---

RemoteChangeWatcher.cpp

- Validate change processor handling: Ensure `ChangeProcessor` correctly interprets remote change events, including deletions, renames, and MIME-type conversions. Solution: Add targeted unit tests for each change type, and instrument `ChangeProcessor` to emit debug metrics; fix discrepancies where event ordering differs from Drive API.
- Docs/local format conversion: Implement Google Docs export mapping using MIME types (e.g., application/vnd.google-apps.document → .docx/.odt via Drive export endpoint). Solution: Add a conversion strategy layer with configurable default formats; fall back to on-demand export when local cache misses; avoid KIOGDrive dependency unless necessary.

LocalChangeWatcher.h

- Unified start/stop/pause/resume controls: Many components share lifecycle controls. Solution: Define an abstract `LifecycleController` interface (start/stop/pause/resume), implement in `LocalChangeWatcher`, `RemoteChangeWatcher`, `SyncActionThread`, and wire via composition or CRTP macro if consistency is critical; prefer interface over macros for type safety.

ChangeProcessor.cpp

- Folder validation cases: Expand validation (existence, permissions, shared drives, multiple parents). Solution: Add checks for Drive “shortcut” and shared drive constraints; validate local FS permissions before writes; surface actionable errors.
- Re-lookup on local origin (possible bug): Re-querying metadata may cause stale or redundant work. Solution: Cache local changes with a strong key (inode+ctime or content hash), only re-lookup when cache miss or conflicting remote state; profile and remove unnecessary queries.
- Conflict processing optimization: If slow, prefer resolving on remote-change path. Solution: Defer heavy conflict resolution to remote watcher; local changes mark provisional state; on remote delta, finalize resolution using server-authoritative metadata.

SyncActionThread.cpp

- Remove test line: Eliminate leftover debug stubs guarded by `#ifdef DEBUG` or feature flags.
- Validate needed/functional switch-case: Confirm action dispatch reflects actual `SyncAction` types; if dynamic dispatch suits better, refactor to strategy pattern per action type.
- Platform-specific metadata updates: Add POSIX extended attributes handling (mtime, mode, user/group), and ensure cross-platform compatibility via abstraction.
- Error handling for specific actions: Standardize per-action error mapping and retries; extend drive signal with structured result (action, status, error_code, retryable, next_step).

- Instrumentation: Add counters and structured logs for each pipeline stage; surface metrics in a debug UI pane.

Acceptance Criteria

- All referenced TODOs have tracked issues linking to this worklog.
- Duplicated logic consolidated; deletion policies configurable and enforced.
- Export conversion works for Google Docs family with user-selectable formats.
- Event-driven queues with reduced idle churn; error handling standardized.
- Database migrations applied; paths no longer used as UIDs.

Risks & Mitigations

- Format conversion API quotas: Cache exports and rate-limit; allow manual export fallback.
- Migration instability: Provide dry-run mode and backups; block sync on failure with clear remediation.
- Cross-platform metadata: Abstract via platform layer; test on Linux primarily, stub others.

---

Launch Review Findings — 2026-02-07

Security

- Token storage uses reversible XOR obfuscation with a hardcoded key and writes tokens to QSettings, which is easy to recover from disk. Replace with OS keyring (libsecret) or strong encryption with per-user keys. [src/auth/TokenStorage.cpp](src/auth/TokenStorage.cpp#L93-L132)

Logic / Data Integrity

- New uploads are tracked in `m_driveActionsInProgress` by `fileId` even when `fileId` is empty; `onFileUploaded()` looks up by the server-assigned `file.id`, so new uploads will not be completed and remain "in operation". Use a temporary local key (e.g., localPath + op id) or store a pending map keyed by reply pointer. [src/sync/SyncActionThread.cpp](src/sync/SyncActionThread.cpp#L196-L214) [src/sync/SyncActionThread.cpp](src/sync/SyncActionThread.cpp#L761-L783)
- Folder uploads call `createFolder()` and immediately `completeAction()` without waiting for server response or updating the database, so failures or assigned IDs are never recorded. Add a `folderCreated` handler and complete only after success. [src/sync/SyncActionThread.cpp](src/sync/SyncActionThread.cpp#L266-L290)
- Remote change tokens are advanced before changes are processed; if processing fails or the app exits mid-loop, those changes are effectively skipped. Consider persisting the token after successful processing or checkpointing. [src/sync/RemoteChangeWatcher.cpp](src/sync/RemoteChangeWatcher.cpp#L186-L205)
- When deleting remote entries with an empty `fileId`, the action is inserted twice into `m_driveActionsInProgress` (once under the empty key and once under the resolved `fileId`), leaving a stale empty-key entry that inflates the in-progress count. Remove/avoid the empty-key insert for operations that resolve IDs later. [src/sync/SyncActionThread.cpp](src/sync/SyncActionThread.cpp#L196-L214) [src/sync/SyncActionThread.cpp](src/sync/SyncActionThread.cpp#L412-L438)

Performance / UX

- Multiple Google Drive API helpers use blocking `QEventLoop` calls. These are invoked from sync components that live on the main thread, which can freeze the UI and stall timers under slow networks. Move these to a worker thread or fully async APIs with callbacks. [src/api/GoogleDriveClient.cpp](src/api/GoogleDriveClient.cpp#L645-L804)

---
