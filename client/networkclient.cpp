#include "networkclient.h"
#include "common/frame.h"
#include "common/idle_timeout.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace {
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject localError(const QString &type, const QString &id,
                       const QString &code, const QString &message) {
  return {{"version", 1},
          {"type", type},
          {"request_id", id},
          {"ok", false},
          {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
} // namespace
struct NetworkClient::Impl {
  struct Pending {
    QString type;
    qint64 deadline;
    bool internal = false;
  };
  NetworkClient *owner;
  NetworkOptions options;
  QTcpSocket socket;
  QTimer reconnect, connectTimeout, maintenance;
  QElapsedTimer clock;
  qtchat::FrameDecoder decoder;
  std::unique_ptr<qtchat::IdleTimeout> idle;
  qtchat::IdleTimeout::Handle watch;
  QString host = "127.0.0.1", username, password, currentState = "disconnected",
          registrationId;
  quint16 port = 9527;
  QByteArray output;
  QHash<QString, Pending> pending;
  QJsonObject registration;
  bool wantReconnect = false, registering = false, failing = false;
  std::size_t retryIndex = 0;
  quint64 epoch = 0;
  qint64 lastTick = 0, lastPing = 0;
  Impl(NetworkClient *value, NetworkOptions config)
      : owner(value), options(std::move(config)), socket(value),
        reconnect(value), connectTimeout(value), maintenance(value) {
    if (options.retryDelaysMs.empty() || options.connectTimeoutMs <= 0 ||
        options.requestTimeoutMs <= 0 || options.heartbeatIntervalMs <= 0 ||
        options.heartbeatTimeoutSeconds <= 0)
      throw std::invalid_argument("invalid network timing options");
    clock.start();
    reconnect.setSingleShot(true);
    connectTimeout.setSingleShot(true);
    QObject::connect(&reconnect, &QTimer::timeout, owner,
                     [this] { connectNow(); });
    QObject::connect(&connectTimeout, &QTimer::timeout, owner, [this] {
      failure("TIMEOUT", QStringLiteral("连接服务器超时"));
    });
    QObject::connect(&socket, &QTcpSocket::connected, owner,
                     [this] { connected(); });
    QObject::connect(&socket, &QTcpSocket::readyRead, owner,
                     [this] { receive(); });
    QObject::connect(&socket, &QTcpSocket::bytesWritten, owner,
                     [this](qint64) { pump(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, owner, [this] {
      failure("DISCONNECTED", QStringLiteral("连接已断开"));
    });
    QObject::connect(&socket, &QTcpSocket::errorOccurred, owner,
                     [this](QAbstractSocket::SocketError) {
                       failure("DISCONNECTED",
                               QStringLiteral("无法连接服务器"));
                     });
    QObject::connect(&maintenance, &QTimer::timeout, owner, [this] { tick(); });
    maintenance.start(std::clamp(options.heartbeatIntervalMs / 4, 10, 250));
  }
  ~Impl() {
    wantReconnect = false;
    registering = false;
    reconnect.stop();
    connectTimeout.stop();
    maintenance.stop();
    QObject::disconnect(&socket, nullptr, owner, nullptr);
    if (idle)
      idle->cancel(watch);
    socket.abort();
  }
  void state(QString value) {
    if (currentState != value) {
      currentState = std::move(value);
      emit owner->stateChanged(currentState);
    }
  }
  void deferredError(const QString &type, const QString &id,
                     const QString &code, const QString &message) {
    auto reply = localError(type, id, code, message);
    QTimer::singleShot(0, owner,
                       [this, id, reply] { emit owner->response(id, reply); });
  }
  void failPending(const QString &code, const QString &message) {
    auto old = pending;
    pending.clear();
    for (auto it = old.cbegin(); it != old.cend(); ++it)
      if (!it->internal)
        emit owner->response(it.key(),
                             localError(it->type, it.key(), code, message));
  }
  void cancelConnection() {
    failing = true;
    reconnect.stop();
    connectTimeout.stop();
    if (idle)
      idle->cancel(watch);
    socket.abort();
    output.clear();
    decoder.reset();
    failing = false;
  }
  void connectNow() {
    ++epoch;
    decoder.reset();
    output.clear();
    state("connecting");
    socket.connectToHost(host, port);
    connectTimeout.start(options.connectTimeoutMs);
  }
  QString send(QString type, QJsonObject data, bool internal = false,
               QString id = {}) {
    if (id.isEmpty())
      id = newId();
    auto bytes = QJsonDocument(QJsonObject{{"version", 1},
                                           {"type", type},
                                           {"request_id", id},
                                           {"data", data}})
                     .toJson(QJsonDocument::Compact);
    if (bytes.isEmpty() ||
        bytes.size() > static_cast<qsizetype>(qtchat::MaxFrameSize)) {
      if (!internal)
        deferredError(type, id, "INVALID_REQUEST",
                      QStringLiteral("请求内容过长"));
      return id;
    }
    auto packet =
        qtchat::encodeFrame(std::string_view(bytes.constData(), bytes.size()));
    if (output.size() + socket.bytesToWrite() +
            static_cast<qint64>(packet.size()) >
        1024 * 1024) {
      if (!internal)
        deferredError(type, id, "BUSY", QStringLiteral("发送队列已满"));
      return id;
    }
    pending.insert(
        id, {type, clock.elapsed() + options.requestTimeoutMs, internal});
    output.append(packet.data(), packet.size());
    pump();
    return id;
  }
  void pump() {
    if (socket.state() != QAbstractSocket::ConnectedState)
      return;
    while (!output.isEmpty()) {
      auto size = socket.write(output);
      if (size < 0) {
        failure("DISCONNECTED", QStringLiteral("发送失败"));
        return;
      }
      if (size == 0)
        return;
      output.remove(0, size);
    }
  }
  void connected() {
    connectTimeout.stop();
    idle =
        std::make_unique<qtchat::IdleTimeout>(options.heartbeatTimeoutSeconds);
    auto generation = epoch;
    watch = idle->watch([this, generation] {
      QTimer::singleShot(0, owner, [this, generation] {
        if (generation == epoch)
          failure("DISCONNECTED", QStringLiteral("服务器心跳超时"));
      });
    });
    lastTick = lastPing = clock.elapsed();
    if (registering) {
      state("authenticating");
      send("register", registration, false, registrationId);
      registration = {};
    } else if (wantReconnect) {
      state("authenticating");
      send("login", {{"username", username}, {"password", password}}, true);
    } else {
      cancelConnection();
      state("disconnected");
    }
  }
  void failure(const QString &code, const QString &message) {
    if (failing || reconnect.isActive())
      return;
    const bool wasRegistering = registering;
    const auto waitingId = registrationId;
    const bool unsentRegistration = registering && !registration.isEmpty();
    cancelConnection();
    failPending(code, message);
    if (unsentRegistration)
      deferredError("register", waitingId, code, message);
    registering = false;
    registration = {};
    registrationId.clear();
    if (wantReconnect && !username.isEmpty()) {
      state("reconnecting");
      auto index = std::min(retryIndex, options.retryDelaysMs.size() - 1);
      ++retryIndex;
      reconnect.start(options.retryDelaysMs[index]);
    } else {
      state("disconnected");
      if (!wasRegistering && code == "INVALID_RESPONSE")
        emit owner->sessionEnded(message);
    }
  }
  void endSession(const QString &message) {
    wantReconnect = false;
    registering = false;
    username.clear();
    password.clear();
    registration = {};
    registrationId.clear();
    ++epoch;
    cancelConnection();
    failPending("DISCONNECTED", message);
    state("disconnected");
    emit owner->sessionEnded(message);
  }
  void receive() {
    try {
      qint64 budget = 65536;
      while (socket.bytesAvailable() > 0 && budget > 0) {
        auto bytes = socket.read(std::min<qint64>(16384, budget));
        budget -= bytes.size();
        for (const auto &payload :
             decoder.feed(std::string_view(bytes.constData(), bytes.size()))) {
          QJsonParseError error;
          auto document = QJsonDocument::fromJson(
              QByteArray(payload.data(), payload.size()), &error);
          if (error.error != QJsonParseError::NoError || !document.isObject())
            throw qtchat::ProtocolError("invalid response JSON");
          const auto object = document.object();
          const auto type = object["type"].toString();
          if (object["version"].toInt() != 1 || type.isEmpty())
            throw qtchat::ProtocolError("invalid response envelope");
          if (object["event"].isBool() && object["event"].toBool()) {
            static const QSet<QString> events = {
                "friend_request", "friends_changed", "message", "presence",
                "session_replaced"};
            if (!events.contains(type) || !object["data"].isObject())
              throw qtchat::ProtocolError("invalid event");
            idle->touch(watch);
            if (type == "session_replaced") {
              endSession(object["data"].toObject()["message"].toString());
              return;
            }
            emit owner->eventReceived(object);
            continue;
          }
          auto id = object["request_id"].toString();
          if (id.isEmpty() || !object["ok"].isBool() ||
              (object["ok"].toBool() ? !object["data"].isObject()
                                     : !object["error"].isObject()))
            throw qtchat::ProtocolError("invalid response fields");
          if (!pending.contains(id))
            continue;
          auto task = pending.value(id);
          if (type != task.type)
            throw qtchat::ProtocolError("response type mismatch");
          pending.remove(id);
          idle->touch(watch);
          if (type == "login" && task.internal) {
            if (!object["ok"].toBool()) {
              endSession(object["error"].toObject()["message"].toString());
              return;
            }
            auto user = object["data"].toObject()["user"].toObject();
            if (user["id"].toInteger() <= 0 ||
                user["username"].toString().isEmpty())
              throw qtchat::ProtocolError("invalid authenticated user");
            retryIndex = 0;
            state("online");
            emit owner->authenticated(user);
          } else if (!task.internal) {
            const bool finishRegistration = type == "register" && registering;
            emit owner->response(id, object);
            if (finishRegistration) {
              registering = false;
              registration = {};
              registrationId.clear();
              cancelConnection();
              state("disconnected");
              return;
            }
          }
        }
      }
      if (socket.bytesAvailable() > 0)
        QTimer::singleShot(0, owner, [this] { receive(); });
    } catch (const std::exception &error) {
      qWarning() << "QtChat protocol error:" << error.what();
      failure("INVALID_RESPONSE", QStringLiteral("服务器返回的数据格式不正确"));
    }
  }
  void tick() {
    const auto now = clock.elapsed();
    QStringList expired;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
      if (it->deadline <= now)
        expired.push_back(it.key());
    for (const auto &id : expired) {
      if (!pending.contains(id))
        continue;
      auto task = pending.take(id);
      if (!task.internal)
        emit owner->response(id,
                             localError(task.type, id, "TIMEOUT",
                                        QStringLiteral("请求超时，请重试")));
      if (task.type == "login" || (task.type == "register" && registering)) {
        failure("TIMEOUT", QStringLiteral("认证请求超时"));
        return;
      }
    }
    if (socket.state() != QAbstractSocket::ConnectedState || !idle || !watch)
      return;
    auto elapsed = (now - lastTick) / 1000;
    if (elapsed > 0) {
      idle->advance(static_cast<unsigned>(
          std::min<qint64>(elapsed, options.heartbeatTimeoutSeconds)));
      lastTick += elapsed * 1000;
    }
    if (wantReconnect && now - lastPing >= options.heartbeatIntervalMs) {
      bool waiting = false;
      for (const auto &task : pending)
        if (task.type == "ping")
          waiting = true;
      if (!waiting)
        send("ping", {}, true);
      lastPing = now;
    }
  }
};

NetworkClient::NetworkClient(QObject *parent, NetworkOptions options)
    : QObject(parent), impl_(std::make_unique<Impl>(this, std::move(options))) {
}
NetworkClient::~NetworkClient() = default;
void NetworkClient::setEndpoint(QString host, quint16 port) {
  if (host.trimmed().isEmpty() || !port)
    return;
  if (host != impl_->host || port != impl_->port)
    logout();
  impl_->host = host.trimmed();
  impl_->port = port;
}
void NetworkClient::login(QString username, QString password) {
  logout();
  impl_->username = std::move(username);
  impl_->password = std::move(password);
  impl_->wantReconnect = true;
  impl_->retryIndex = 0;
  impl_->connectNow();
}
QString NetworkClient::registerAccount(QString username, QString displayName,
                                       QString password) {
  logout();
  impl_->registering = true;
  impl_->registrationId = newId();
  auto id = impl_->registrationId;
  impl_->registration = {{"username", username},
                         {"display_name", displayName},
                         {"password", password}};
  impl_->connectNow();
  return id;
}
QString NetworkClient::request(QString type, QJsonObject data) {
  if (impl_->currentState != "online") {
    auto id = newId();
    impl_->deferredError(type, id, "OFFLINE",
                         QStringLiteral("尚未连接，请等待登录或重连"));
    return id;
  }
  return impl_->send(std::move(type), std::move(data));
}
void NetworkClient::logout() {
  impl_->wantReconnect = false;
  impl_->registering = false;
  ++impl_->epoch;
  impl_->username.clear();
  impl_->password.clear();
  impl_->registration = {};
  impl_->registrationId.clear();
  impl_->cancelConnection();
  impl_->failPending("DISCONNECTED", QStringLiteral("连接已结束"));
  impl_->state("disconnected");
}
QString NetworkClient::state() const { return impl_->currentState; }
