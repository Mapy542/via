/**
 * @file TestNativeDocShortcutHandler.cpp
 * @brief Unit tests for Via native-doc shortcut integration helpers
 */

#include <QMimeDatabase>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "api/GoogleDriveClient.h"
#include "utils/NativeDocShortcutHandler.h"

class TestNativeDocShortcutHandler : public QObject {
    Q_OBJECT

   private slots:
    void testMimeTypeField_ContainsAllRegisteredTypes();
    void testShortcutArgument_LocalFileUrlResolvesToPath();
    void testShortcutArgument_MissingNativeDocStillDetected();
    void testParseShortcutText_ValidShortcut();
    void testParseShortcutFile_InvalidHeader();
    void testExtensionsAndMimeTypes_SameCount();
    void testMimePackageXml_ContainsAllExtensions();
    void testMimePackageXml_ContainsAllMimeTypes();
    void testDesktopMimeTypes_AreDistinct();
    void testDesktopMimeTypes_MatchExpectedPrefix();
    void testDescribeErrorResponse_PlainTextExportLimitMapsToFriendlyMessage();
    void testExportFailureMessage_SizeLimitUsesFriendlyMessage();
    void testExportFailureMessage_403PermissionPreservesOriginal();
    void testExportFailureMessage_403WithoutTextUsesGenericMessage();
};

void TestNativeDocShortcutHandler::testMimeTypeField_ContainsAllRegisteredTypes() {
    const QString field = nativeDocDesktopMimeTypesField();

    QVERIFY(field.contains(QStringLiteral("application/x-via-gdoc;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gsheet;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gslides;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gdraw;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gdrive;")));
}

void TestNativeDocShortcutHandler::testShortcutArgument_LocalFileUrlResolvesToPath() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString path = tempDir.filePath(QStringLiteral("example.gdoc"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("[Via Native Document]\nURL=https://docs.google.com/document/d/example/edit\n");
    file.close();

    const QString argument = QUrl::fromLocalFile(path).toString();
    QString resolvedPath;

    QVERIFY(isNativeDocShortcutArgument(argument, &resolvedPath));
    QCOMPARE(QDir::cleanPath(resolvedPath), QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

void TestNativeDocShortcutHandler::testShortcutArgument_MissingNativeDocStillDetected() {
    QString resolvedPath;

    QVERIFY(
        isNativeDocShortcutArgument(QStringLiteral("/tmp/missing-shortcut.gdoc"), &resolvedPath));
    QVERIFY(resolvedPath.endsWith(QStringLiteral("missing-shortcut.gdoc")));
}

void TestNativeDocShortcutHandler::testParseShortcutText_ValidShortcut() {
    const QString text = QStringLiteral(
        "[Via Native Document]\n"
        "URL=https://docs.google.com/document/d/example/edit\n"
        "MimeType=application/vnd.google-apps.document\n");

    const auto parsed = parseNativeDocShortcutText(text);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->url.toString(),
             QStringLiteral("https://docs.google.com/document/d/example/edit"));
    QCOMPARE(parsed->remoteMimeType, QStringLiteral("application/vnd.google-apps.document"));
}

void TestNativeDocShortcutHandler::testParseShortcutFile_InvalidHeader() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString path = tempDir.filePath(QStringLiteral("broken.gdoc"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("not a Via shortcut\n");
    file.close();

    QString error;
    const auto parsed = parseNativeDocShortcutFile(path, &error);
    QVERIFY(!parsed.has_value());
    QVERIFY(error.contains(QStringLiteral("header")));
}

void TestNativeDocShortcutHandler::testExtensionsAndMimeTypes_SameCount() {
    // The extension list and MIME type list must stay in lock-step.
    const QStringList exts = nativeDocShortcutExtensions();
    const QStringList mimes = nativeDocDesktopMimeTypes();
    QCOMPARE(exts.size(), mimes.size());
}

void TestNativeDocShortcutHandler::testMimePackageXml_ContainsAllExtensions() {
    const QString xml = nativeDocMimePackageXml();
    for (const QString& ext : nativeDocShortcutExtensions()) {
        const QString glob = QStringLiteral("*.") + ext;
        QVERIFY2(xml.contains(glob),
                 qPrintable(QStringLiteral("MIME package XML missing glob for .") + ext));
    }
}

void TestNativeDocShortcutHandler::testMimePackageXml_ContainsAllMimeTypes() {
    const QString xml = nativeDocMimePackageXml();
    for (const QString& mime : nativeDocDesktopMimeTypes()) {
        QVERIFY2(xml.contains(mime),
                 qPrintable(QStringLiteral("MIME package XML missing type ") + mime));
    }
}

void TestNativeDocShortcutHandler::testDesktopMimeTypes_AreDistinct() {
    const QStringList mimes = nativeDocDesktopMimeTypes();
    QSet<QString> unique(mimes.begin(), mimes.end());
    QCOMPARE(unique.size(), mimes.size());
}

void TestNativeDocShortcutHandler::testDesktopMimeTypes_MatchExpectedPrefix() {
    // All custom MIME types must use the application/x-via- prefix
    for (const QString& mime : nativeDocDesktopMimeTypes()) {
        QVERIFY2(mime.startsWith(QStringLiteral("application/x-via-")),
                 qPrintable(QStringLiteral("Unexpected MIME prefix: ") + mime));
    }
}

void TestNativeDocShortcutHandler::
    testDescribeErrorResponse_PlainTextExportLimitMapsToFriendlyMessage() {
    const QString detail = GoogleDriveClient::describeErrorResponse(
        QByteArray("This file is too large to be exported."), 403, QString());

    QVERIFY(detail.contains(QStringLiteral("too large to be exported")));
    QVERIFY(nativeDocExportFailureMessage(detail, 403).contains(QStringLiteral("10 MB")));
}

void TestNativeDocShortcutHandler::testExportFailureMessage_SizeLimitUsesFriendlyMessage() {
    const QString detail = nativeDocExportFailureMessage(
        QStringLiteral("This file is too large to be exported."), 403);

    QVERIFY(detail.contains(QStringLiteral("10 MB")));
}

void TestNativeDocShortcutHandler::testExportFailureMessage_403PermissionPreservesOriginal() {
    const QString detail = nativeDocExportFailureMessage(
        QStringLiteral("The user does not have sufficient permissions for this file."), 403);

    QCOMPARE(detail,
             QStringLiteral("The user does not have sufficient permissions for this file."));
}

void TestNativeDocShortcutHandler::testExportFailureMessage_403WithoutTextUsesGenericMessage() {
    const QString detail = nativeDocExportFailureMessage(QString(), 403);

    QCOMPARE(detail, QStringLiteral("Google rejected the export for this document or account."));
}

QTEST_MAIN(TestNativeDocShortcutHandler)
#include "TestNativeDocShortcutHandler.moc"