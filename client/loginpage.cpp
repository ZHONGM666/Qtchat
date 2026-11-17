#include "loginpage.h"
#include "icons.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
QLineEdit *input(QWidget *parent, const char *name, const QString &placeholder,
                 const QString &icon, bool password = false) {
  auto *edit = new QLineEdit(parent);
  edit->setObjectName(name);
  edit->setPlaceholderText(placeholder);
  edit->setMinimumHeight(48);
  edit->addAction(lineIcon(icon, QColor("#818493")),
                  QLineEdit::LeadingPosition);
  if (password)
    edit->setEchoMode(QLineEdit::Password);
  return edit;
}
QLabel *errorLabel(const char *name, QWidget *parent) {
  auto *label = new QLabel(parent);
  label->setObjectName(name);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumHeight(22);
  return label;
}
bool valid(const QString &user, const QString &password) {
  return QRegularExpression("^[A-Za-z0-9_]{3,32}$").match(user).hasMatch() &&
         password.toUtf8().size() >= 8 && password.toUtf8().size() <= 128;
}
} // namespace
LoginPage::LoginPage(QWidget *parent) : QWidget(parent) {
  setObjectName("loginPage");
  setAttribute(Qt::WA_StyledBackground, true);
  auto *root = new QVBoxLayout(this);
  root->setSpacing(0);
  root->setContentsMargins(32, 12, 32, 20);
  root->addStretch();
  auto *logo = new QLabel(this);
  logo->setObjectName("brandLogo");
  logo->setPixmap(chatLogo(104));
  logo->setAlignment(Qt::AlignCenter);
  root->addWidget(logo);
  auto *title = new QLabel("QtChat", this);
  title->setObjectName("brandTitle");
  title->setAlignment(Qt::AlignCenter);
  root->addWidget(title);
  auto *subtitle = new QLabel(QStringLiteral("即时通信系统"), this);
  subtitle->setObjectName("brandSubtitle");
  subtitle->setAlignment(Qt::AlignCenter);
  root->addWidget(subtitle);
  root->addSpacing(28);
  forms_ = new QStackedWidget(this);
  forms_->setFixedWidth(520);
  forms_->setFixedHeight(258);
  auto *loginForm = new QWidget(forms_);
  auto *l = new QVBoxLayout(loginForm);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(14);
  username_ =
      input(loginForm, "loginUsername", QStringLiteral("用户名"), "user");
  password_ =
      input(loginForm, "loginPassword", QStringLiteral("密码"), "lock", true);
  l->addWidget(username_);
  l->addWidget(password_);
  loginError_ = errorLabel("loginError", loginForm);
  l->addWidget(loginError_);
  login_ = new QPushButton(QStringLiteral("登 录"), loginForm);
  login_->setObjectName("loginButton");
  login_->setProperty("primary", true);
  login_->setMinimumHeight(50);
  l->addWidget(login_);
  auto *links = new QHBoxLayout;
  auto *regLink = new QPushButton(QStringLiteral("注册账号"), loginForm);
  regLink->setObjectName("registerLink");
  regLink->setProperty("link", true);
  auto *settings = new QPushButton(QStringLiteral("服务器设置"), loginForm);
  settings->setProperty("link", true);
  links->addWidget(regLink);
  links->addStretch();
  links->addWidget(settings);
  l->addLayout(links);
  forms_->addWidget(loginForm);
  auto *regForm = new QWidget(forms_);
  auto *r = new QVBoxLayout(regForm);
  r->setContentsMargins(0, 0, 0, 0);
  r->setSpacing(10);
  display_ = input(regForm, "registrationDisplay",
                   QStringLiteral("昵称（支持中文）"), "user");
  regUsername_ =
      input(regForm, "registrationUsername",
            QStringLiteral("用户名：3–32位字母、数字或下划线"), "user");
  regPassword_ = input(regForm, "registrationPassword",
                       QStringLiteral("密码：8–128字节"), "lock", true);
  confirm_ = input(regForm, "registrationConfirm",
                   QStringLiteral("再次输入密码"), "lock", true);
  for (auto *edit : {display_, regUsername_, regPassword_, confirm_})
    r->addWidget(edit);
  registrationError_ = errorLabel("registrationError", regForm);
  r->addWidget(registrationError_);
  register_ = new QPushButton(QStringLiteral("注 册"), regForm);
  register_->setObjectName("registerButton");
  register_->setProperty("primary", true);
  register_->setMinimumHeight(48);
  r->addWidget(register_);
  auto *back = new QPushButton(QStringLiteral("返回登录"), regForm);
  back->setObjectName("backToLogin");
  back->setProperty("link", true);
  r->addWidget(back);
  forms_->addWidget(regForm);
  root->addWidget(forms_, 0, Qt::AlignHCenter);
  cancel_ = new QPushButton(QStringLiteral("取消连接"), this);
  cancel_->setProperty("link", true);
  cancel_->hide();
  root->addWidget(cancel_, 0, Qt::AlignHCenter);
  root->addStretch();
  connect(regLink, &QPushButton::clicked, this, [this, logo, title, subtitle] {
    emit cancelRequested();
    logo->hide();
    title->hide();
    subtitle->hide();
    forms_->setCurrentIndex(1);
    forms_->setFixedHeight(380);
    registrationError_->clear();
    regUsername_->setFocus();
  });
  connect(back, &QPushButton::clicked, this, [this] {
    emit cancelRequested();
    showLogin();
  });
  connect(settings, &QPushButton::clicked, this, &LoginPage::settingsRequested);
  connect(cancel_, &QPushButton::clicked, this, &LoginPage::cancelRequested);
  connect(login_, &QPushButton::clicked, this, [this] {
    auto name = username_->text().trimmed();
    if (!valid(name, password_->text())) {
      showError(QStringLiteral("请输入有效用户名和8–128字节的密码"));
      return;
    }
    showError({});
    emit loginRequested(name, password_->text());
  });
  connect(password_, &QLineEdit::returnPressed, login_, &QPushButton::click);
  connect(register_, &QPushButton::clicked, this, [this] {
    auto name = regUsername_->text().trimmed(),
         display = display_->text().trimmed(), password = regPassword_->text();
    if (password != confirm_->text()) {
      showError(QStringLiteral("两次输入的密码不一致"));
      return;
    }
    if (!valid(name, password) || display.isEmpty() ||
        display.toUcs4().size() > 32) {
      showError(QStringLiteral("请检查用户名、昵称和密码长度"));
      return;
    }
    showError({});
    emit registerRequested(name, display, password);
  });
  username_->setFocus();
}
void LoginPage::showLogin(QString username) {
  forms_->setCurrentIndex(0);
  forms_->setFixedHeight(258);
  for (const char *name : {"brandLogo", "brandTitle", "brandSubtitle"})
    if (auto *label = findChild<QLabel *>(name))
      label->show();
  if (!username.isEmpty())
    username_->setText(username);
  password_->clear();
  loginError_->clear();
  setBusy(false);
}
void LoginPage::showError(QString text, bool error) {
  auto *label = forms_->currentIndex() ? registrationError_ : loginError_;
  label->setStyleSheet(error ? "color:#d15a65;" : "color:#3e9276;");
  label->setText(std::move(text));
}
void LoginPage::setBusy(bool busy) {
  login_->setEnabled(!busy);
  register_->setEnabled(!busy);
  for (auto *edit :
       {username_, password_, regUsername_, display_, regPassword_, confirm_})
    edit->setEnabled(!busy);
  cancel_->setVisible(busy);
}
