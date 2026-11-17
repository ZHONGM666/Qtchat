#pragma once
#include <QWidget>
class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class LoginPage : public QWidget {
  Q_OBJECT
public:
  explicit LoginPage(QWidget *parent = nullptr);
  void showLogin(QString username = {});
  void showError(QString text, bool error = true);
  void setBusy(bool busy);
signals:
  void loginRequested(QString username, QString password);
  void registerRequested(QString username, QString displayName,
                         QString password);
  void settingsRequested();
  void cancelRequested();

private:
  QStackedWidget *forms_;
  QLineEdit *username_, *password_, *regUsername_, *display_, *regPassword_,
      *confirm_;
  QLabel *loginError_, *registrationError_;
  QPushButton *login_, *register_, *cancel_;
};
