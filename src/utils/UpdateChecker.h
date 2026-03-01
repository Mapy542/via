/**
 * @file UpdateChecker.h
 * @brief Checks GitHub releases for application updates
 */

#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QApplication>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVersionNumber>

/**
 * @class UpdateChecker
 * @brief Checks the GitHub releases API for new versions of Via
 *
 * On application startup, queries the GitHub API for the latest release.
 * If a newer version is found, shows a dialog offering to open the
 * releases page. The user can dismiss or choose to skip a particular version.
 */
class UpdateChecker : public QObject {
    Q_OBJECT

   public:
    explicit UpdateChecker(QObject* parent = nullptr) : QObject(parent) {
        m_nam = new QNetworkAccessManager(this);
    }

    /**
     * @brief Check for updates asynchronously
     *
     * Queries the GitHub API for the latest release tag. If a newer version
     * is found (and not previously skipped by the user), emits updateAvailable
     * and shows a dialog.
     */
    void checkForUpdates(bool silent = true) {
        QUrl url(QStringLiteral("https://api.github.com/repos/Mapy542/Via/releases/latest"));
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("Via/%1").arg(qApp->applicationVersion()));
        request.setRawHeader("Accept", "application/vnd.github+json");

        QNetworkReply* reply = m_nam->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, silent]() {
            reply->deleteLater();
            handleReply(reply, silent);
        });
    }

   signals:
    /**
     * @brief Emitted when a newer version is available
     * @param version The latest version string (e.g. "1.2.0")
     * @param url The URL of the release page
     */
    void updateAvailable(const QString& version, const QUrl& url);

    /**
     * @brief Emitted when the app is already up to date
     */
    void upToDate();

    /**
     * @brief Emitted when the update check fails
     * @param error Error description
     */
    void checkFailed(const QString& error);

   private:
    void handleReply(QNetworkReply* reply, bool silent) {
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Update check failed:" << reply->errorString();
            emit checkFailed(reply->errorString());
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            qWarning() << "Update check: unexpected response format";
            emit checkFailed("Unexpected response format");
            return;
        }

        QJsonObject obj = doc.object();
        QString tagName = obj.value("tag_name").toString();
        QString htmlUrl = obj.value("html_url").toString();

        if (tagName.isEmpty()) {
            qWarning() << "Update check: no tag_name in response";
            emit checkFailed("No tag_name in response");
            return;
        }

        // Strip leading 'v' from tag if present (e.g. "v1.2.0" -> "1.2.0")
        QString remoteVersion = tagName;
        if (remoteVersion.startsWith('v', Qt::CaseInsensitive)) {
            remoteVersion = remoteVersion.mid(1);
        }

        QString currentVersion = qApp->applicationVersion();
        QVersionNumber remote = QVersionNumber::fromString(remoteVersion);
        QVersionNumber current = QVersionNumber::fromString(currentVersion);

        if (remote.isNull()) {
            qWarning() << "Update check: could not parse remote version:" << tagName;
            emit checkFailed("Could not parse version: " + tagName);
            return;
        }

        if (remote > current) {
            qInfo() << "Update available:" << remoteVersion << "(current:" << currentVersion << ")";

            // Check if user previously skipped this version
            QSettings settings;
            QString skippedVersion = settings.value("updates/skippedVersion", "").toString();
            if (silent && skippedVersion == remoteVersion) {
                qInfo() << "User previously skipped version" << remoteVersion;
                return;
            }

            emit updateAvailable(remoteVersion, QUrl(htmlUrl));
            showUpdateDialog(remoteVersion, QUrl(htmlUrl));
        } else {
            qInfo() << "Up to date (current:" << currentVersion << "latest:" << remoteVersion
                    << ")";
            if (!silent) {
                QMessageBox::information(
                    nullptr, tr("No Updates"),
                    tr("You are running the latest version of Via (%1).").arg(currentVersion));
            }
            emit upToDate();
        }
    }

    void showUpdateDialog(const QString& version, const QUrl& releaseUrl) {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("Update Available"));
        msgBox.setText(tr("A new version of Via is available: <b>%1</b><br>"
                          "You are currently running version <b>%2</b>.")
                           .arg(version, qApp->applicationVersion()));
        msgBox.setInformativeText(tr("Would you like to open the download page?"));
        msgBox.setIcon(QMessageBox::Information);

        QPushButton* downloadBtn = msgBox.addButton(tr("Download"), QMessageBox::AcceptRole);
        QPushButton* skipBtn = msgBox.addButton(tr("Skip This Version"), QMessageBox::RejectRole);
        msgBox.addButton(tr("Remind Me Later"), QMessageBox::DestructiveRole);

        msgBox.setDefaultButton(downloadBtn);
        msgBox.exec();

        QAbstractButton* clicked = msgBox.clickedButton();
        if (clicked == downloadBtn) {
            QDesktopServices::openUrl(releaseUrl);
        } else if (clicked == skipBtn) {
            QSettings settings;
            settings.setValue("updates/skippedVersion", version);
            qInfo() << "User chose to skip version" << version;
        }
        // "Remind Me Later" does nothing — next launch will check again
    }

    QNetworkAccessManager* m_nam;
};

#endif  // UPDATECHECKER_H
