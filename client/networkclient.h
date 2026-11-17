#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

struct NetworkOptions {
  int connectTimeoutMs = 5000, requestTimeoutMs = 10000,
      heartbeatIntervalMs = 15000, heartbeatTimeoutSeconds = 60;
  std::vector<int> retryDelaysMs{1000, 2000, 4000, 8000, 15000};
};
class NetworkClient : public QObject {
  Q_OBJECT
public:
  explicit NetworkClient(QObject *parent = nullptr,
                         NetworkOptions options = {});
  ~NetworkClient() override;
  void setEndpoint(QString host, quint16 port);
  void login(QString username, QString password);
  QString registerAccount(QString username, QString displayName,
                          QString password);
  QString request(QString type, QJsonObject data = {});
  void logout();
  QString state() const;
signals:
  void stateChanged(QString state);
  void authenticated(QJsonObject user);
  void response(QString requestId, QJsonObject reply);
  void eventReceived(QJsonObject event);
  void sessionEnded(QString message);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
