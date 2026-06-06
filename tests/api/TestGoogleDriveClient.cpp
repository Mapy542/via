/**
 * @file TestGoogleDriveClient.cpp
 * @brief Focused regression tests for GoogleDriveClient blocking helpers and Drive queries
 */

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <QSignalSpy>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest/QtTest>

#define private public
#include "api/GoogleDriveClient.h"
#undef private

#include "auth/GoogleAuthManager.h"

namespace {
class QueuedNetworkReply final : public QNetworkReply {
    Q_OBJECT

   public:
    QueuedNetworkReply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation,
                       QByteArray body, QNetworkReply::NetworkError error, int httpStatus,
                       QObject* parent = nullptr)
        : QNetworkReply(parent), m_body(std::move(body)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, httpStatus);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (error != QNetworkReply::NoError) {
            setError(error, QStringLiteral("Injected network error"));
        }

        QTimer::singleShot(0, this, &QueuedNetworkReply::finishReply);
    }

    void abort() override {
        if (m_finished) {
            return;
        }
        m_aborted = true;
        finishReply();
    }

    qint64 bytesAvailable() const override {
        return m_body.size() - m_offset + QIODevice::bytesAvailable();
    }

    bool isSequential() const override { return true; }

   protected:
    qint64 readData(char* data, qint64 maxSize) override {
        if (m_offset >= m_body.size()) {
            return -1;
        }

        const qint64 bytesToRead = qMin(maxSize, m_body.size() - m_offset);
        memcpy(data, m_body.constData() + m_offset, static_cast<size_t>(bytesToRead));
        m_offset += bytesToRead;
        return bytesToRead;
    }

   private:
    void finishReply() {
        if (m_finished) {
            return;
        }

        m_finished = true;
        setFinished(true);
        if (m_aborted && error() == QNetworkReply::NoError) {
            setError(QNetworkReply::OperationCanceledError, QStringLiteral("Operation canceled"));
        }
        if (!m_body.isEmpty()) {
            emit readyRead();
        }
        emit finished();
    }

    QByteArray m_body;
    qint64 m_offset = 0;
    bool m_finished = false;
    bool m_aborted = false;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager {
    Q_OBJECT

   public:
    struct PlannedResponse {
        QByteArray body;
        QNetworkReply::NetworkError error = QNetworkReply::NoError;
        int httpStatus = 200;
    };

    void enqueueJsonResponse(const QJsonObject& body, int httpStatus = 200,
                             QNetworkReply::NetworkError error = QNetworkReply::NoError) {
        m_responses.enqueue(
            {QJsonDocument(body).toJson(QJsonDocument::Compact), error, httpStatus});
    }

    int requestCount() const { return m_requests.size(); }

    QString queryValue(int requestIndex, const QString& key) const {
        return QUrlQuery(m_requests.at(requestIndex).url()).queryItemValue(key);
    }

    int transferTimeout(int requestIndex) const {
        return m_requests.at(requestIndex).transferTimeout();
    }

   protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoingData) override {
        Q_UNUSED(outgoingData);

        m_requests.append(request);

        PlannedResponse response;
        if (!m_responses.isEmpty()) {
            response = m_responses.dequeue();
        } else {
            response.error = QNetworkReply::UnknownNetworkError;
            response.httpStatus = 500;
        }

        return new QueuedNetworkReply(request, operation, response.body, response.error,
                                      response.httpStatus, this);
    }

   private:
    QQueue<PlannedResponse> m_responses;
    QList<QNetworkRequest> m_requests;
};

class FakeAuthenticatedAuthManager final : public GoogleAuthManager {
   public:
    explicit FakeAuthenticatedAuthManager(QObject* parent = nullptr)
        : GoogleAuthManager(nullptr, parent) {}

    bool isAuthenticated() const override { return true; }
    QString accessToken() const override { return QStringLiteral("test-token"); }

    bool ensureValidToken(int timeoutMs = 15000) override {
        Q_UNUSED(timeoutMs);
        ++ensureValidTokenCalls;
        return true;
    }

    int ensureValidTokenCalls = 0;
};
}  // namespace

class TestGoogleDriveClient : public QObject {
    Q_OBJECT

   private slots:
    void testCreateRequest_AttachesAuthorizationHeaderOnlyForHttps();
    void testGetFolderIdByPath_ReleasesBlockingGuardAndEscapesPathSegment();
    void testListFiles_EscapesFolderIdInQuery();
    void testListFilesBlocking_EscapesFolderIdInQuery();
    void testAsyncMetadataRequests_SetTransferTimeout();
};

void TestGoogleDriveClient::testCreateRequest_AttachesAuthorizationHeaderOnlyForHttps() {
    FakeAuthenticatedAuthManager authManager;
    auto* networkManager = new FakeNetworkAccessManager();
    GoogleDriveClient client(&authManager, networkManager);

    const QNetworkRequest httpsRequest =
        client.createRequest(QUrl(QStringLiteral("https://example.com/resource")));
    QCOMPARE(httpsRequest.rawHeader("Authorization"), QByteArray("Bearer test-token"));
    QCOMPARE(authManager.ensureValidTokenCalls, 1);

    const QNetworkRequest httpRequest =
        client.createRequest(QUrl(QStringLiteral("http://example.com/resource")));
    QVERIFY(!httpRequest.hasRawHeader("Authorization"));
    QCOMPARE(authManager.ensureValidTokenCalls, 1);
}

void TestGoogleDriveClient::testGetFolderIdByPath_ReleasesBlockingGuardAndEscapesPathSegment() {
    auto* networkManager = new FakeNetworkAccessManager();
    networkManager->enqueueJsonResponse(QJsonObject{{QStringLiteral("files"), QJsonArray()}});
    networkManager->enqueueJsonResponse(
        QJsonObject{{QStringLiteral("id"), QStringLiteral("resolved-root-id")}});

    GoogleDriveClient client(nullptr, networkManager);

    QCOMPARE(client.getFolderIdByPath(QStringLiteral("/missing'folder")), QString());
    QCOMPARE(client.getRootFolderId(), QStringLiteral("resolved-root-id"));
    QCOMPARE(networkManager->requestCount(), 2);
    QCOMPARE(networkManager->queryValue(0, QStringLiteral("q")),
             QStringLiteral("name = 'missing\\'folder' and 'root' in parents and mimeType = "
                            "'application/vnd.google-apps.folder' and trashed = false"));
}

void TestGoogleDriveClient::testListFiles_EscapesFolderIdInQuery() {
    auto* networkManager = new FakeNetworkAccessManager();
    networkManager->enqueueJsonResponse(QJsonObject{{QStringLiteral("files"), QJsonArray()}});

    GoogleDriveClient client(nullptr, networkManager);
    QSignalSpy filesListedSpy(&client, &GoogleDriveClient::filesListed);

    client.listFiles(QStringLiteral("folder'id"));

    QTRY_COMPARE(filesListedSpy.count(), 1);
    QCOMPARE(networkManager->requestCount(), 1);
    QCOMPARE(networkManager->queryValue(0, QStringLiteral("q")),
             QStringLiteral("'folder\\'id' in parents and trashed = false"));
}

void TestGoogleDriveClient::testListFilesBlocking_EscapesFolderIdInQuery() {
    auto* networkManager = new FakeNetworkAccessManager();
    networkManager->enqueueJsonResponse(QJsonObject{{QStringLiteral("files"), QJsonArray()}});

    GoogleDriveClient client(nullptr, networkManager);

    const QList<DriveFile> files = client.listFilesBlocking(QStringLiteral("folder'id"));

    QVERIFY(files.isEmpty());
    QCOMPARE(networkManager->requestCount(), 1);
    QCOMPARE(
        networkManager->queryValue(0, QStringLiteral("q")),
        QStringLiteral(
            "'folder\\'id' in parents and trashed = false and (not mimeType contains "
            "'application/vnd.google-apps.' or mimeType = 'application/vnd.google-apps.folder' "
            "or mimeType = 'application/vnd.google-apps.shortcut')"));
}

void TestGoogleDriveClient::testAsyncMetadataRequests_SetTransferTimeout() {
    auto* networkManager = new FakeNetworkAccessManager();
    networkManager->enqueueJsonResponse(QJsonObject{{QStringLiteral("files"), QJsonArray()}});
    networkManager->enqueueJsonResponse(
        QJsonObject{{QStringLiteral("changes"), QJsonArray()},
                    {QStringLiteral("newStartPageToken"), QStringLiteral("next-token")}});
    networkManager->enqueueJsonResponse(
        QJsonObject{{QStringLiteral("startPageToken"), QStringLiteral("start-token")}});

    GoogleDriveClient client(nullptr, networkManager);

    client.listFiles(QStringLiteral("all"));
    client.listChanges(QStringLiteral("known-token"));
    client.getStartPageToken();

    QCOMPARE(networkManager->requestCount(), 3);
    QCOMPARE(networkManager->transferTimeout(0), 30000);
    QCOMPARE(networkManager->transferTimeout(1), 30000);
    QCOMPARE(networkManager->transferTimeout(2), 30000);
}

QTEST_MAIN(TestGoogleDriveClient)

#include "TestGoogleDriveClient.moc"