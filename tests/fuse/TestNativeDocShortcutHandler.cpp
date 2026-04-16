/**
 * @file TestNativeDocShortcutHandler.cpp
 * @brief Unit tests for Via native-doc shortcut integration helpers
 */

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "utils/NativeDocShortcutHandler.h"

class TestNativeDocShortcutHandler : public QObject {
    Q_OBJECT

   private slots:
    void testMimeTypeField_ContainsAllRegisteredTypes();
    void testParseShortcutText_ValidShortcut();
    void testParseShortcutFile_InvalidHeader();
};

void TestNativeDocShortcutHandler::testMimeTypeField_ContainsAllRegisteredTypes() {
    const QString field = nativeDocDesktopMimeTypesField();

    QVERIFY(field.contains(QStringLiteral("application/x-via-gdoc;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gsheet;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gslides;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gdraw;")));
    QVERIFY(field.contains(QStringLiteral("application/x-via-gdrive;")));
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

QTEST_MAIN(TestNativeDocShortcutHandler)
#include "TestNativeDocShortcutHandler.moc"