#include "mainwindow.h"
#include "client/chatpage.h"
#include "client/icons.h"
#include "client/loginpage.h"
#include "client/networkclient.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWindow>

namespace {
class WindowFrame : public QWidget {
public:
  using QWidget::QWidget;

protected:
  void mousePressEvent(QMouseEvent *event) override {
    Qt::Edges edges;
    auto point = event->position();
    if (point.x() < 6)
      edges |= Qt::LeftEdge;
    if (point.x() > width() - 6)
      edges |= Qt::RightEdge;
    if (point.y() < 6)
      edges |= Qt::TopEdge;
    if (point.y() > height() - 6)
      edges |= Qt::BottomEdge;
    if (event->button() == Qt::LeftButton && edges && window()->windowHandle())
      window()->windowHandle()->startSystemResize(edges);
    else
      QWidget::mousePressEvent(event);
  }
};
class TitleBar : public QWidget {
public:
  explicit TitleBar(QMainWindow *window) : QWidget(window) {
    setObjectName("titleBar");
    setFixedHeight(52);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(18, 4, 8, 4);
    row->setSpacing(10);
    auto *logo = new QLabel(this);
    logo->setPixmap(chatLogo(32));
    row->addWidget(logo);
    auto *brand = new QLabel("QtChat", this);
    brand->setObjectName("windowBrand");
    row->addWidget(brand);
    row->addStretch();
    auto control = [&](QString text, const char *name) {
      auto *button = new QPushButton(text, this);
      button->setObjectName(name);
      button->setFixedSize(40, 34);
      button->setFocusPolicy(Qt::NoFocus);
      button->setStyleSheet(
          "QPushButton{border:0;background:transparent;font-size:21px;padding:"
          "0;}QPushButton:hover{background:#e1e1f4;}");
      row->addWidget(button);
      return button;
    };
    auto *minimize = control(QStringLiteral("−"), "windowMinimize");
    auto *maximize = control(QStringLiteral("□"), "windowMaximize");
    auto *close = control(QStringLiteral("×"), "windowClose");
    close->setStyleSheet(
        "QPushButton{border:0;background:transparent;font-size:24px;padding:0;}"
        "QPushButton:hover{background:#ed6472;color:white;}");
    connect(minimize, &QPushButton::clicked, window, &QWidget::showMinimized);
    connect(maximize, &QPushButton::clicked, window, [window] {
      window->isMaximized() ? window->showNormal() : window->showMaximized();
    });
    connect(close, &QPushButton::clicked, window, &QWidget::close);
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && window()->windowHandle())
      window()->windowHandle()->startSystemMove();
    else
      QWidget::mousePressEvent(event);
  }
  void mouseDoubleClickEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      window()->isMaximized() ? window()->showNormal()
                              : window()->showMaximized();
      event->accept();
    }
  }
};
} // namespace
struct MainWindow::Impl {
  MainWindow *q;
  NetworkClient *network;
  LoginPage *login;
  ChatPage *chat;
  QStackedWidget *pages;
  QHash<QString, QString> tokens;
  QString registrationId, registrationName, lastUsername;
  QSettings settings;
  QString host;
  quint16 port;
  explicit Impl(MainWindow *window)
      : q(window), settings(QStandardPaths::writableLocation(
                                QStandardPaths::AppConfigLocation) +
                                "/client.ini",
                            QSettings::IniFormat) {
    q->setWindowTitle("QtChat");
    q->setWindowIcon(QIcon(chatLogo(64)));
    q->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    q->resize(1280, 820);
    q->setMinimumSize(1000, 680);
    network = new NetworkClient(q);
    host = settings.value("server/host", "127.0.0.1").toString();
    port = settings.value("server/port", 9527).value<quint16>();
    if (!port)
      port = 9527;
    network->setEndpoint(host, port);
    auto *frame = new WindowFrame(q);
    frame->setObjectName("windowFrame");
    auto *root = new QVBoxLayout(frame);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(0);
    root->addWidget(new TitleBar(q));
    pages = new QStackedWidget(frame);
    login = new LoginPage(pages);
    chat = new ChatPage(pages);
    pages->addWidget(login);
    pages->addWidget(chat);
    root->addWidget(pages, 1);
    q->setCentralWidget(frame);
    QObject::connect(login, &LoginPage::loginRequested, q,
                     [this](QString username, QString password) {
                       lastUsername = username;
                       network->login(username, password);
                     });
    QObject::connect(
        login, &LoginPage::registerRequested, q,
        [this](QString username, QString display, QString password) {
          registrationName = username;
          registrationId =
              network->registerAccount(username, display, password);
        });
    QObject::connect(login, &LoginPage::cancelRequested, q, [this] {
      network->logout();
      login->setBusy(false);
      login->showError({});
    });
    QObject::connect(login, &LoginPage::settingsRequested, q,
                     [this] { showSettings(); });
    QObject::connect(chat, &ChatPage::settingsRequested, q,
                     [this] { showSettings(); });
    QObject::connect(chat, &ChatPage::logoutRequested, q, [this] {
      tokens.clear();
      network->logout();
      showLogin();
    });
    QObject::connect(chat, &ChatPage::businessRequested, q,
                     [this](QString type, QJsonObject data, QString token) {
                       auto id = network->request(type, data);
                       tokens.insert(id, token);
                     });
    QObject::connect(
        network, &NetworkClient::response, q,
        [this](QString id, QJsonObject reply) {
          if (id == registrationId) {
            registrationId.clear();
            login->setBusy(false);
            if (reply["ok"].toBool()) {
              lastUsername = registrationName;
              login->showLogin(registrationName);
              login->showError(QStringLiteral("注册成功，请登录"), false);
            } else
              login->showError(reply["error"].toObject()["message"].toString());
            return;
          }
          auto token = tokens.take(id);
          if (!token.isEmpty())
            chat->resolve(token, reply);
        });
    QObject::connect(
        network, &NetworkClient::stateChanged, q, [this](QString state) {
          login->setBusy(state == "connecting" || state == "authenticating" ||
                         state == "reconnecting");
          chat->setOnline(state == "online");
          if (state == "disconnected" && pages->currentWidget() == chat)
            showLogin();
          if (pages->currentWidget() == login && state == "reconnecting")
            login->showError(QStringLiteral(
                "无法连接，正在自动重试；可取消或修改服务器地址"));
        });
    QObject::connect(network, &NetworkClient::authenticated, q,
                     [this](QJsonObject user) {
                       pages->setCurrentWidget(chat);
                       login->setBusy(false);
                       chat->setSession(user);
                     });
    QObject::connect(network, &NetworkClient::eventReceived, chat,
                     &ChatPage::handleEvent);
    QObject::connect(network, &NetworkClient::sessionEnded, q,
                     [this](QString text) {
                       showLogin();
                       login->showError(text);
                     });
  }
  void showLogin() {
    tokens.clear();
    chat->resetSession();
    pages->setCurrentWidget(login);
    login->showLogin(lastUsername);
  }
  void showSettings() {
    QDialog dialog(q);
    dialog.setWindowTitle(QStringLiteral("服务器设置"));
    auto *layout = new QFormLayout(&dialog);
    auto *address = new QLineEdit(host, &dialog);
    auto *number = new QSpinBox(&dialog);
    number->setRange(1, 65535);
    number->setValue(port);
    layout->addRow(QStringLiteral("服务器地址"), address);
    layout->addRow(QStringLiteral("端口"), number);
    auto *note = new QLabel(
        QStringLiteral("默认连接本机Linux服务端。修改地址后需要重新登录。"),
        &dialog);
    note->setWordWrap(true);
    layout->addRow(note);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted ||
        address->text().trimmed().isEmpty())
      return;
    auto newHost = address->text().trimmed();
    auto newPort = static_cast<quint16>(number->value());
    if (newHost == host && newPort == port)
      return;
    tokens.clear();
    network->logout();
    host = newHost;
    port = newPort;
    network->setEndpoint(host, port);
    settings.setValue("server/host", host);
    settings.setValue("server/port", port);
    showLogin();
    login->showError(QStringLiteral("服务器地址已更新，请登录"), false);
  }
};
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  Q_INIT_RESOURCE(resources);
  QFile style(":/qtchat/style.qss");
  if (style.open(QIODevice::ReadOnly))
    setStyleSheet(QString::fromUtf8(style.readAll()));
  impl_ = std::make_unique<Impl>(this);
}
MainWindow::~MainWindow() = default;
