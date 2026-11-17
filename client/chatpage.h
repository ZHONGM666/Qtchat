#pragma once
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QWidget>
#include <memory>
class MessageEditor : public QPlainTextEdit {
  Q_OBJECT
public:
  using QPlainTextEdit::QPlainTextEdit;
signals:
  void submitted();

protected:
  void keyPressEvent(QKeyEvent *event) override;
};
class ChatPage : public QWidget {
  Q_OBJECT
public:
  explicit ChatPage(QWidget *parent = nullptr);
  ~ChatPage() override;
  void setSession(QJsonObject user);
  void setOnline(bool online);
  void resetSession();
  void resolve(QString token, QJsonObject reply);
  void handleEvent(QJsonObject event);
signals:
  void businessRequested(QString type, QJsonObject data, QString token);
  void settingsRequested();
  void logoutRequested();

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
