/**
 * @file TestPathUtils.cpp
 * @brief Unit tests for path safety helpers.
 */

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "utils/PathUtils.h"

namespace {

void writeFile(const QString& path, const QByteArray& contents = QByteArray("x")) {
    QVERIFY(QDir().mkpath(QFileInfo(path).path()));

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(contents), contents.size());
    file.close();
}

}  // namespace

class TestPathUtils : public QObject {
    Q_OBJECT

   private slots:
    void testIsSymlink();
    void testIsPathWithinRootBoundary();
    void testTryGetRelativePathWithinRoot();
    void testIsCanonicalPathWithinRootRejectsExternalTarget();
    void testIsCanonicalPathWithinRootAllowsInternalTarget();
    void testClassifyRecursiveRootRemovalAllowsRecursiveDelete();
    void testClassifyRecursiveRootRemovalRefusesDangerousPath();
    void testClassifyRecursiveRootRemovalUnlinksSymlinkRoot();
    void testClassifyRecursiveRootRemovalRefusesAncestorSymlinkEscape();
};

void TestPathUtils::testIsSymlink() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString targetPath = tempDir.path() + "/target.txt";
    const QString linkPath = tempDir.path() + "/link.txt";
    writeFile(targetPath, QByteArray("target"));

    if (!QFile::link(targetPath, linkPath)) {
        QSKIP("Symlink creation not supported");
    }

    QVERIFY(PathUtils::isSymlink(linkPath));
    QVERIFY(PathUtils::isSymlink(QFileInfo(linkPath)));
    QVERIFY(!PathUtils::isSymlink(targetPath));
    QVERIFY(!PathUtils::isSymlink(QFileInfo(targetPath)));
}

void TestPathUtils::testIsPathWithinRootBoundary() {
    const QString root = QStringLiteral("/tmp/sync");

    QVERIFY(PathUtils::isPathWithinRootBoundary(root, root));
    QVERIFY(PathUtils::isPathWithinRootBoundary(root + "/child/file.txt", root));
    QVERIFY(!PathUtils::isPathWithinRootBoundary(root + ".Trash-1000/file.txt", root));
    QVERIFY(!PathUtils::isPathWithinRootBoundary(root + "-backup/file.txt", root));
}

void TestPathUtils::testTryGetRelativePathWithinRoot() {
    const QString root = QStringLiteral("/tmp/sync");
    QString relativePath = QStringLiteral("unchanged");

    QVERIFY(PathUtils::tryGetRelativePathWithinRoot(root + "/child/file.txt", root, &relativePath));
    QCOMPARE(relativePath, QStringLiteral("child/file.txt"));

    QVERIFY(PathUtils::tryGetRelativePathWithinRoot(root, root, &relativePath));
    QCOMPARE(relativePath, QString());

    QVERIFY(
        !PathUtils::tryGetRelativePathWithinRoot(root + "backup/file.txt", root, &relativePath));
    QCOMPARE(relativePath, QString());
}

void TestPathUtils::testIsCanonicalPathWithinRootRejectsExternalTarget() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString syncRoot = tempDir.path() + "/sync-root";
    const QString externalRoot = tempDir.path() + "/external-root";
    QVERIFY(QDir().mkpath(syncRoot));
    QVERIFY(QDir().mkpath(externalRoot));

    const QString externalTarget = externalRoot + "/outside.txt";
    const QString linkPath = syncRoot + "/outside-link.txt";
    writeFile(externalTarget, QByteArray("outside"));

    if (!QFile::link(externalTarget, linkPath)) {
        QSKIP("Symlink creation not supported");
    }

    QVERIFY(!PathUtils::isCanonicalPathWithinRoot(linkPath, syncRoot));
}

void TestPathUtils::testIsCanonicalPathWithinRootAllowsInternalTarget() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString syncRoot = tempDir.path() + "/sync-root";
    QVERIFY(QDir().mkpath(syncRoot));

    const QString internalTarget = syncRoot + "/nested/inside.txt";
    const QString linkPath = syncRoot + "/inside-link.txt";
    writeFile(internalTarget, QByteArray("inside"));

    if (!QFile::link(internalTarget, linkPath)) {
        QSKIP("Symlink creation not supported");
    }

    QVERIFY(PathUtils::isCanonicalPathWithinRoot(linkPath, syncRoot));
}

void TestPathUtils::testClassifyRecursiveRootRemovalAllowsRecursiveDelete() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString rootPath = tempDir.path() + "/safe/root";
    QVERIFY(QDir().mkpath(rootPath));

    const PathUtils::RecursiveRootRemovalDecision decision =
        PathUtils::classifyRecursiveRootRemoval(rootPath);

    QCOMPARE(decision.action, PathUtils::RecursiveRootRemovalAction::RemoveRecursively);
    QCOMPARE(decision.absolutePath, QDir::cleanPath(rootPath));
    QCOMPARE(decision.canonicalPath, QFileInfo(rootPath).canonicalFilePath());
}

void TestPathUtils::testClassifyRecursiveRootRemovalRefusesDangerousPath() {
    const PathUtils::RecursiveRootRemovalDecision decision =
        PathUtils::classifyRecursiveRootRemoval(QStringLiteral("/tmp"));

    QCOMPARE(decision.action, PathUtils::RecursiveRootRemovalAction::Refuse);
    QCOMPARE(decision.absolutePath, QStringLiteral("/tmp"));
}

void TestPathUtils::testClassifyRecursiveRootRemovalUnlinksSymlinkRoot() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString externalRoot = tempDir.path() + "/external-root";
    const QString linkRoot = tempDir.path() + "/link-root";
    QVERIFY(QDir().mkpath(externalRoot));

    if (!QFile::link(externalRoot, linkRoot)) {
        QSKIP("Symlink creation not supported");
    }

    const PathUtils::RecursiveRootRemovalDecision decision =
        PathUtils::classifyRecursiveRootRemoval(linkRoot);

    QCOMPARE(decision.action, PathUtils::RecursiveRootRemovalAction::RemoveSymlinkOnly);
    QCOMPARE(decision.absolutePath, QDir::cleanPath(linkRoot));
    QCOMPARE(decision.canonicalPath, QFileInfo(linkRoot).canonicalFilePath());
}

void TestPathUtils::testClassifyRecursiveRootRemovalRefusesAncestorSymlinkEscape() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString externalParent = tempDir.path() + "/external-parent";
    const QString externalRoot = externalParent + "/sync-root";
    const QString aliasParent = tempDir.path() + "/alias-parent";
    const QString aliasedRoot = aliasParent + "/sync-root";
    QVERIFY(QDir().mkpath(externalRoot));

    if (!QFile::link(externalParent, aliasParent)) {
        QSKIP("Symlink creation not supported");
    }

    const PathUtils::RecursiveRootRemovalDecision decision =
        PathUtils::classifyRecursiveRootRemoval(aliasedRoot);

    QCOMPARE(decision.action, PathUtils::RecursiveRootRemovalAction::Refuse);
    QCOMPARE(decision.absolutePath, QDir::cleanPath(aliasedRoot));
    QCOMPARE(decision.canonicalPath, QFileInfo(aliasedRoot).canonicalFilePath());
}

QTEST_MAIN(TestPathUtils)
#include "TestPathUtils.moc"