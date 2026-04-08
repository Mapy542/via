/**
 * @file FuseTestHarness.cpp
 * @brief Headless test harness for mounting Via's FUSE filesystem
 *
 * Mounts the FUSE filesystem against a real Google Drive account using
 * tokens provided via environment variables. Designed for E2E testing
 * in CI (GitHub Actions) or locally.
 *
 * Required environment variables:
 *   VIA_CLIENT_ID       - OAuth 2.0 client ID
 *   VIA_CLIENT_SECRET   - OAuth 2.0 client secret
 *   VIA_REFRESH_TOKEN   - OAuth 2.0 refresh token
 *   VIA_MOUNT_POINT     - Where to mount the FUSE filesystem (default: /tmp/via-fuse-test)
 *   VIA_CACHE_SIZE_MB   - Max cache size in MB (default: 500)
 *
 * Usage:
 *   ./via_fuse_harness
 *   # Mount stays active until SIGTERM/SIGINT is received
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>
#include <csignal>

#include "api/GoogleDriveClient.h"
#include "auth/GoogleAuthManager.h"
#include "auth/TokenStorage.h"
#include "fuse/FuseDriver.h"
#include "sync/SyncDatabase.h"

static FuseDriver* g_fuseDriver = nullptr;

static void signalHandler(int sig) {
    qInfo() << "Received signal" << sig << "— initiating shutdown";
    if (g_fuseDriver && g_fuseDriver->isMounted()) {
        g_fuseDriver->unmount();
    }
    QCoreApplication::quit();
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("Via");
    app.setOrganizationName("Via");

    // ── Read configuration from environment ──────────────────────────

    const QString clientId = qEnvironmentVariable("VIA_CLIENT_ID");
    const QString clientSecret = qEnvironmentVariable("VIA_CLIENT_SECRET");
    const QString refreshToken = qEnvironmentVariable("VIA_REFRESH_TOKEN");
    const QString mountPoint = qEnvironmentVariable("VIA_MOUNT_POINT", "/tmp/via-fuse-test");
    const qint64 cacheSizeMb = qEnvironmentVariable("VIA_CACHE_SIZE_MB", "500").toLongLong();

    if (clientId.isEmpty() || clientSecret.isEmpty() || refreshToken.isEmpty()) {
        qCritical() << "Missing required environment variables:";
        qCritical() << "  VIA_CLIENT_ID, VIA_CLIENT_SECRET, VIA_REFRESH_TOKEN";
        return 1;
    }

    qInfo() << "FUSE test harness starting";
    qInfo() << "  Mount point:" << mountPoint;
    qInfo() << "  Cache size:" << cacheSizeMb << "MB";

    // ── Create mount point directory ─────────────────────────────────

    QDir mountDir(mountPoint);
    if (!mountDir.exists() && !mountDir.mkpath(".")) {
        qCritical() << "Failed to create mount point:" << mountPoint;
        return 1;
    }

    // ── Initialize components ────────────────────────────────────────

    TokenStorage tokenStorage;
    tokenStorage.saveCredentials(clientId, clientSecret);
    // Save a dummy access token + real refresh token; the auth manager
    // will refresh on first API call via ensureValidToken().
    tokenStorage.saveTokens(QString(),  // access token (empty — forces refresh)
                            refreshToken,
                            QDateTime::currentDateTimeUtc()  // already expired
    );

    GoogleAuthManager authManager(&tokenStorage);
    authManager.setCredentials(clientId, clientSecret);

    // Kick off a token refresh so we have a valid access token before mount
    qInfo() << "Refreshing access token...";
    if (!authManager.ensureValidToken(30000)) {
        qCritical() << "Failed to obtain a valid access token. Check your credentials.";
        return 1;
    }
    qInfo() << "Access token acquired, expires:" << authManager.tokenExpiry().toString(Qt::ISODate);

    GoogleDriveClient driveClient(&authManager);

    SyncDatabase syncDatabase;
    if (!syncDatabase.initialize()) {
        qCritical() << "Failed to initialize sync database";
        return 1;
    }

    FuseDriver fuseDriver(&driveClient, &syncDatabase);
    fuseDriver.setMountPoint(mountPoint);
    fuseDriver.setMaxCacheSizeBytes(cacheSizeMb * 1024LL * 1024LL);
    g_fuseDriver = &fuseDriver;

    // ── Install signal handlers ──────────────────────────────────────

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── Mount ────────────────────────────────────────────────────────

    qInfo() << "Mounting FUSE filesystem...";
    if (!fuseDriver.mount()) {
        qCritical() << "Failed to mount FUSE filesystem at" << mountPoint;
        return 1;
    }
    qInfo() << "FUSE filesystem mounted at" << mountPoint;

    // Write a sentinel file so the test script can detect mount readiness
    QString readyFile = mountPoint + "/../.via-fuse-ready";
    QFile f(readyFile);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("ready\n");
        f.close();
    }

    // ── Run event loop (keeps token refresh alive) ───────────────────

    int rc = app.exec();

    // ── Cleanup ──────────────────────────────────────────────────────

    if (fuseDriver.isMounted()) {
        qInfo() << "Unmounting FUSE filesystem...";
        fuseDriver.unmount();
    }

    QFile::remove(readyFile);
    qInfo() << "FUSE test harness exiting with code" << rc;
    return rc;
}
