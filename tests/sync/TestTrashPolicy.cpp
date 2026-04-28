/**
 * @file TestTrashPolicy.cpp
 * @brief Unit tests for TrashPolicy path predicates
 *
 * Validates all FreeDesktop trash path classification logic used by
 * LocalChangeWatcher, FullSync, and ChangeProcessor to correctly
 * identify trash subtrees and classify move/rename operations as
 * trash or restore intent.
 */

#include <QtTest/QtTest>

#include "sync/TrashPolicy.h"

class TestTrashPolicy : public QObject {
    Q_OBJECT

   private slots:
    // =========================================================================
    // isTrashRelativePath
    // =========================================================================
    void testRelative_TrashRootUid1000();
    void testRelative_TrashRootUid0();
    void testRelative_TrashRootUidLarge();
    void testRelative_TrashFilesSubdir();
    void testRelative_TrashInfoSubdir();
    void testRelative_NestedFileInTrash();
    void testRelative_EmptyPath();
    void testRelative_NormalFile();
    void testRelative_NormalFolder();
    void testRelative_DotTrashNoUid();
    void testRelative_DotTrashNonNumericUid();
    void testRelative_NestedTrashNotMatched();
    void testRelative_TrailingSlash();

    // =========================================================================
    // isTrashPath (absolute)
    // =========================================================================
    void testAbsolute_TrashDirUnderSyncRoot();
    void testAbsolute_TrashFileUnderSyncRoot();
    void testAbsolute_NormalFileUnderSyncRoot();
    void testAbsolute_TrashOutsideSyncRoot();
    void testAbsolute_SyncRootWithTrailingSlash();
    void testAbsolute_PathCleaningDoubleSlash();
    void testAbsolute_PathCleaningDotSegment();
    void testAbsolute_SyncRootItself();
    void testAbsolute_SiblingPrefixOutsideSyncRoot();

    // =========================================================================
    // isMoveToTrash
    // =========================================================================
    void testMoveToTrash_LiveToTrash();
    void testMoveToTrash_TrashToTrash();
    void testMoveToTrash_LiveToLive();
    void testMoveToTrash_TrashToLive();

    // =========================================================================
    // isRestoreFromTrash
    // =========================================================================
    void testRestoreFromTrash_TrashToLive();
    void testRestoreFromTrash_LiveToTrash();
    void testRestoreFromTrash_TrashToTrash();
    void testRestoreFromTrash_LiveToLive();
};

// ===========================================================================
//  isTrashRelativePath
// ===========================================================================

void TestTrashPolicy::testRelative_TrashRootUid1000() {
    QVERIFY(TrashPolicy::isTrashRelativePath(".Trash-1000"));
}

void TestTrashPolicy::testRelative_TrashRootUid0() {
    QVERIFY(TrashPolicy::isTrashRelativePath(".Trash-0"));
}

void TestTrashPolicy::testRelative_TrashRootUidLarge() {
    QVERIFY(TrashPolicy::isTrashRelativePath(".Trash-65534"));
}

void TestTrashPolicy::testRelative_TrashFilesSubdir() {
    QVERIFY(TrashPolicy::isTrashRelativePath(".Trash-1000/files"));
}

void TestTrashPolicy::testRelative_TrashInfoSubdir() {
    QVERIFY(TrashPolicy::isTrashRelativePath(".Trash-1000/info"));
}

void TestTrashPolicy::testRelative_NestedFileInTrash() {
    QVERIFY(TrashPolicy::isTrashRelativePath(".Trash-1000/files/document.pdf"));
}

void TestTrashPolicy::testRelative_EmptyPath() { QVERIFY(!TrashPolicy::isTrashRelativePath("")); }

void TestTrashPolicy::testRelative_NormalFile() {
    QVERIFY(!TrashPolicy::isTrashRelativePath("documents/report.pdf"));
}

void TestTrashPolicy::testRelative_NormalFolder() {
    QVERIFY(!TrashPolicy::isTrashRelativePath("photos/vacation"));
}

void TestTrashPolicy::testRelative_DotTrashNoUid() {
    QVERIFY(!TrashPolicy::isTrashRelativePath(".Trash"));
}

void TestTrashPolicy::testRelative_DotTrashNonNumericUid() {
    QVERIFY(!TrashPolicy::isTrashRelativePath(".Trash-abc"));
}

void TestTrashPolicy::testRelative_NestedTrashNotMatched() {
    // .Trash-1000 must be the FIRST path component
    QVERIFY(!TrashPolicy::isTrashRelativePath("subdir/.Trash-1000/files/foo.txt"));
}

void TestTrashPolicy::testRelative_TrailingSlash() {
    QVERIFY(TrashPolicy::isTrashRelativePath(".Trash-1000/"));
}

// ===========================================================================
//  isTrashPath (absolute)
// ===========================================================================

void TestTrashPolicy::testAbsolute_TrashDirUnderSyncRoot() {
    QVERIFY(TrashPolicy::isTrashPath("/home/user/sync/.Trash-1000", "/home/user/sync"));
}

void TestTrashPolicy::testAbsolute_TrashFileUnderSyncRoot() {
    QVERIFY(TrashPolicy::isTrashPath("/home/user/sync/.Trash-1000/files/document.pdf",
                                     "/home/user/sync"));
}

void TestTrashPolicy::testAbsolute_NormalFileUnderSyncRoot() {
    QVERIFY(!TrashPolicy::isTrashPath("/home/user/sync/documents/report.pdf", "/home/user/sync"));
}

void TestTrashPolicy::testAbsolute_TrashOutsideSyncRoot() {
    QVERIFY(!TrashPolicy::isTrashPath("/other/path/.Trash-1000", "/home/user/sync"));
}

void TestTrashPolicy::testAbsolute_SyncRootWithTrailingSlash() {
    QVERIFY(TrashPolicy::isTrashPath("/home/user/sync/.Trash-1000", "/home/user/sync/"));
}

void TestTrashPolicy::testAbsolute_PathCleaningDoubleSlash() {
    QVERIFY(TrashPolicy::isTrashPath("/home/user/sync//.Trash-1000", "/home/user/sync"));
}

void TestTrashPolicy::testAbsolute_PathCleaningDotSegment() {
    QVERIFY(TrashPolicy::isTrashPath("/home/user/sync/./subdir/../.Trash-1000", "/home/user/sync"));
}

void TestTrashPolicy::testAbsolute_SyncRootItself() {
    QVERIFY(!TrashPolicy::isTrashPath("/home/user/sync", "/home/user/sync"));
}

void TestTrashPolicy::testAbsolute_SiblingPrefixOutsideSyncRoot() {
    QVERIFY(!TrashPolicy::isTrashPath("/home/user/sync.Trash-1000/files/document.pdf",
                                      "/home/user/sync"));
}

// ===========================================================================
//  isMoveToTrash
// ===========================================================================

void TestTrashPolicy::testMoveToTrash_LiveToTrash() {
    QVERIFY(
        TrashPolicy::isMoveToTrash("/sync/doc.txt", "/sync/.Trash-1000/files/doc.txt", "/sync"));
}

void TestTrashPolicy::testMoveToTrash_TrashToTrash() {
    // Move within trash is not a "move to trash"
    QVERIFY(!TrashPolicy::isMoveToTrash("/sync/.Trash-1000/files/a.txt",
                                        "/sync/.Trash-1000/files/b.txt", "/sync"));
}

void TestTrashPolicy::testMoveToTrash_LiveToLive() {
    QVERIFY(!TrashPolicy::isMoveToTrash("/sync/a.txt", "/sync/b.txt", "/sync"));
}

void TestTrashPolicy::testMoveToTrash_TrashToLive() {
    // This is a restore, not a move to trash
    QVERIFY(!TrashPolicy::isMoveToTrash("/sync/.Trash-1000/files/a.txt", "/sync/a.txt", "/sync"));
}

// ===========================================================================
//  isRestoreFromTrash
// ===========================================================================

void TestTrashPolicy::testRestoreFromTrash_TrashToLive() {
    QVERIFY(TrashPolicy::isRestoreFromTrash("/sync/.Trash-1000/files/doc.txt", "/sync/doc.txt",
                                            "/sync"));
}

void TestTrashPolicy::testRestoreFromTrash_LiveToTrash() {
    QVERIFY(!TrashPolicy::isRestoreFromTrash("/sync/doc.txt", "/sync/.Trash-1000/files/doc.txt",
                                             "/sync"));
}

void TestTrashPolicy::testRestoreFromTrash_TrashToTrash() {
    QVERIFY(!TrashPolicy::isRestoreFromTrash("/sync/.Trash-1000/files/a.txt",
                                             "/sync/.Trash-1000/files/b.txt", "/sync"));
}

void TestTrashPolicy::testRestoreFromTrash_LiveToLive() {
    QVERIFY(!TrashPolicy::isRestoreFromTrash("/sync/a.txt", "/sync/b.txt", "/sync"));
}

QTEST_MAIN(TestTrashPolicy)
#include "TestTrashPolicy.moc"
