/**
 * @file TestAutostartManager.cpp
 * @brief Focused tests for runtime desktop-integration asset staging
 */

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#define private public
#include "utils/AutostartManager.h"
#undef private

class TestAutostartManager : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();
    void testInstallThemeIcons_SourceBuildStagesAppAndMimeIcons();

   private:
    QByteArray m_originalAppDir;
    QByteArray m_originalAppImage;
    QByteArray m_originalXdgDataHome;
};

void TestAutostartManager::init() {
    m_originalAppDir = qgetenv("APPDIR");
    m_originalAppImage = qgetenv("APPIMAGE");
    m_originalXdgDataHome = qgetenv("XDG_DATA_HOME");
}

void TestAutostartManager::cleanup() {
    if (m_originalAppDir.isNull()) {
        qunsetenv("APPDIR");
    } else {
        qputenv("APPDIR", m_originalAppDir);
    }

    if (m_originalAppImage.isNull()) {
        qunsetenv("APPIMAGE");
    } else {
        qputenv("APPIMAGE", m_originalAppImage);
    }

    if (m_originalXdgDataHome.isNull()) {
        qunsetenv("XDG_DATA_HOME");
    } else {
        qputenv("XDG_DATA_HOME", m_originalXdgDataHome);
    }
}

void TestAutostartManager::testInstallThemeIcons_SourceBuildStagesAppAndMimeIcons() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    qunsetenv("APPDIR");
    qunsetenv("APPIMAGE");
    qputenv("XDG_DATA_HOME", tempDir.path().toUtf8());

    QVERIFY(AutostartManager::installThemeIcons());

    const QString iconsRoot = tempDir.path() + QStringLiteral("/icons/hicolor/scalable");
    QVERIFY(QFile::exists(iconsRoot + QStringLiteral("/apps/via.svg")));

    for (const QString& iconName : nativeDocDesktopIconNames()) {
        const QString path =
            iconsRoot + QStringLiteral("/mimetypes/") + iconName + QStringLiteral(".svg");
        QVERIFY2(QFile::exists(path), qPrintable(QStringLiteral("Missing staged icon: ") + path));
    }

    QVERIFY(!AutostartManager::installThemeIcons());
}

QTEST_MAIN(TestAutostartManager)
#include "TestAutostartManager.moc"