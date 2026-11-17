#include "client/networkclient.h"
#include "common/frame.h"
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

static QByteArray wire(const QJsonObject &object) {
  auto text = QJsonDocument(object).toJson(QJsonDocument::Compact);
  auto bytes =
      qtchat::encodeFrame(std::string_view(text.constData(), text.size()));
  return QByteArray(bytes.data(), bytes.size());
}
class FakeServer : public QObject {
public:
  QTcpServer server;
  QPointer<QTcpSocket> peer;
  bool allowLogin = true, dropPong = false, ignoreHistory = false,
       wrongType = false;
  int logins = 0;
  FakeServer() {
    if (!server.listen(QHostAddress::LocalHost, 0))
      qFatal("fake server cannot listen");
    connect(&server, &QTcpServer::newConnection, this, [this] {
      peer = server.nextPendingConnection();
      auto current = peer;
      auto decoder = std::make_shared<qtchat::FrameDecoder>();
      connect(current, &QTcpSocket::disconnected, current,
              &QObject::deleteLater);
      connect(current, &QTcpSocket::readyRead, this, [this, current, decoder] {
        auto bytes = current->readAll();
        for (const auto &text :
             decoder->feed(std::string_view(bytes.constData(), bytes.size()))) {
          auto request =
              QJsonDocument::fromJson(QByteArray(text.data(), text.size()))
                  .object();
          auto type = request["type"].toString();
          if (type == "login")
            ++logins;
          if ((type == "ping" && dropPong) ||
              (type == "history" && ignoreHistory))
            continue;
          QJsonObject data;
          if (type == "login" || type == "register")
            data = {{"user",
                     QJsonObject{{"id", 1},
                                 {"username", "alice"},
                                 {"display_name", QStringLiteral("爱丽丝")}}}};
          else if (type == "ping")
            data = {{"pong", true}};
          else
            data = {{"friends", QJsonArray{}}, {"requests", QJsonArray{}}};
          QJsonObject reply{{"version", 1},
                            {"type", type},
                            {"request_id", request["request_id"]},
                            {"ok", type != "login" || allowLogin},
                            {"data", data}};
          if (type == "login" && !allowLogin)
            reply["error"] = QJsonObject{{"code", "AUTH_FAILED"},
                                         {"message", "wrong password"}};
          if (type == "friend_list" && wrongType)
            reply["type"] = QStringLiteral("conversations");
          auto packet = wire(reply);
          if (current->property("fragmenting").toBool()) {
            current->setProperty(
                "deferredFrames",
                current->property("deferredFrames").toByteArray() + packet);
          } else if (type == "friend_list") {
            current->setProperty("fragmenting", true);
            current->write(packet.left(2));
            QTimer::singleShot(5, current, [current, packet] {
              current->write(packet.mid(2) + packet +
                             current->property("deferredFrames").toByteArray());
              current->setProperty("deferredFrames", QByteArray());
              current->setProperty("fragmenting", false);
            });
          } else
            current->write(packet);
        }
      });
    });
  }
};
static NetworkOptions options() {
  NetworkOptions o;
  o.connectTimeoutMs = 200;
  o.requestTimeoutMs = 100;
  o.heartbeatIntervalMs = 50;
  o.heartbeatTimeoutSeconds = 1;
  o.retryDelaysMs = {20, 40, 80};
  return o;
}
class NetworkTest : public QObject {
  Q_OBJECT
private slots:
  void invalidResponseStillCompletesTheAffectedRequest() {
    FakeServer fake;
    NetworkClient client(nullptr, options());
    client.setEndpoint("127.0.0.1", fake.server.serverPort());
    QSignalSpy auth(&client, &NetworkClient::authenticated);
    client.login("alice", "Secret-123");
    QTRY_COMPARE_WITH_TIMEOUT(auth.size(), 1, 800);
    fake.wrongType = true;
    QSignalSpy replies(&client, &NetworkClient::response);
    auto id = client.request("friend_list");
    QTRY_VERIFY_WITH_TIMEOUT(!replies.empty(), 800);
    QCOMPARE(replies[0][0].toString(), id);
    QCOMPARE(
        replies[0][1].toJsonObject()["error"].toObject()["code"].toString(),
        "INVALID_RESPONSE");
  }
  void offlineRequestReportsFailureAfterReturningItsId() {
    NetworkClient client;
    QSignalSpy replies(&client, &NetworkClient::response);
    auto id = client.request("friend_list");
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 800);
    QCOMPARE(replies[0][0].toString(), id);
    QCOMPARE(
        replies[0][1].toJsonObject()["error"].toObject()["code"].toString(),
        "OFFLINE");
  }
  void fragmentedAndDuplicateReplyCompletesOnce() {
    FakeServer fake;
    NetworkClient client(nullptr, options());
    client.setEndpoint("127.0.0.1", fake.server.serverPort());
    QSignalSpy auth(&client, &NetworkClient::authenticated);
    client.login("alice", "Secret-123");
    QTRY_COMPARE_WITH_TIMEOUT(auth.size(), 1, 800);
    QCOMPARE(auth[0][0].toJsonObject()["display_name"].toString(),
             QStringLiteral("爱丽丝"));
    QSignalSpy replies(&client, &NetworkClient::response);
    auto id = client.request("friend_list");
    QTRY_VERIFY_WITH_TIMEOUT(!replies.empty(), 800);
    QTest::qWait(40);
    QCOMPARE(replies.size(), 1);
    QCOMPARE(replies[0][0].toString(), id);
  }
  void reconnectAuthenticatesAndReplacementStopsRetry() {
    FakeServer fake;
    NetworkClient client(nullptr, options());
    client.setEndpoint("127.0.0.1", fake.server.serverPort());
    QSignalSpy auth(&client, &NetworkClient::authenticated),
        ended(&client, &NetworkClient::sessionEnded);
    client.login("alice", "Secret-123");
    QTRY_COMPARE_WITH_TIMEOUT(auth.size(), 1, 800);
    fake.peer->abort();
    QTRY_COMPARE_WITH_TIMEOUT(auth.size(), 2, 1000);
    fake.peer->write(wire({{"version", 1},
                           {"event", true},
                           {"type", "session_replaced"},
                           {"data", QJsonObject{{"message", "replaced"}}}}));
    QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 800);
    const int count = fake.logins;
    QTest::qWait(150);
    QCOMPARE(fake.logins, count);
    QCOMPARE(client.state(), "disconnected");
  }
  void wrongPasswordDoesNotReconnect() {
    FakeServer fake;
    fake.allowLogin = false;
    NetworkClient client(nullptr, options());
    client.setEndpoint("127.0.0.1", fake.server.serverPort());
    QSignalSpy ended(&client, &NetworkClient::sessionEnded);
    client.login("alice", "Wrong-123");
    QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 800);
    QTest::qWait(150);
    QCOMPARE(fake.logins, 1);
  }
  void missingResponseTimesOutWithoutLosingConnection() {
    FakeServer fake;
    fake.ignoreHistory = true;
    NetworkClient client(nullptr, options());
    client.setEndpoint("127.0.0.1", fake.server.serverPort());
    QSignalSpy auth(&client, &NetworkClient::authenticated);
    client.login("alice", "Secret-123");
    QTRY_COMPARE_WITH_TIMEOUT(auth.size(), 1, 800);
    QSignalSpy replies(&client, &NetworkClient::response);
    auto id = client.request("history", {{"peer_id", 2}});
    QTRY_VERIFY_WITH_TIMEOUT(!replies.empty(), 800);
    QCOMPARE(replies[0][0].toString(), id);
    QCOMPARE(
        replies[0][1].toJsonObject()["error"].toObject()["code"].toString(),
        "TIMEOUT");
    QCOMPARE(client.state(), "online");
  }
  void missingPongUsesReusedTimeoutAndReconnects() {
    FakeServer fake;
    fake.dropPong = true;
    NetworkClient client(nullptr, options());
    client.setEndpoint("127.0.0.1", fake.server.serverPort());
    QSignalSpy auth(&client, &NetworkClient::authenticated);
    client.login("alice", "Secret-123");
    QTRY_COMPARE_WITH_TIMEOUT(auth.size(), 1, 800);
    QTRY_VERIFY_WITH_TIMEOUT(auth.size() >= 2, 2500);
    client.logout();
    const int count = fake.logins;
    QTest::qWait(120);
    QCOMPARE(fake.logins, count);
  }
  void registrationConnectsWithoutPretendingToLogin() {
    FakeServer fake;
    NetworkClient client(nullptr, options());
    client.setEndpoint("127.0.0.1", fake.server.serverPort());
    QSignalSpy auth(&client, &NetworkClient::authenticated),
        replies(&client, &NetworkClient::response);
    auto id =
        client.registerAccount("alice", QStringLiteral("爱丽丝"), "Secret-123");
    QTRY_VERIFY_WITH_TIMEOUT(!replies.empty(), 800);
    QCOMPARE(replies[0][0].toString(), id);
    QVERIFY(replies[0][1].toJsonObject()["ok"].toBool());
    QCOMPARE(auth.size(), 0);
    QCOMPARE(fake.logins, 0);
  }
};
QTEST_GUILESS_MAIN(NetworkTest)
#include "networkclient_test.moc"
