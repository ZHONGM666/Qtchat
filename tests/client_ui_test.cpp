#include "client/networkclient.h"
#include "mainwindow.h"
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <functional>

static bool waitUntil(const std::function<bool()> &predicate,
                      int milliseconds = 5000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < milliseconds)
    QTest::qWait(20);
  return predicate();
}
static bool setText(QWidget &w, const char *name, const QString &value) {
  auto *edit = w.findChild<QLineEdit *>(name);
  if (!edit)
    return false;
  edit->setText(value);
  return true;
}
static bool click(QWidget &w, const char *name) {
  auto *button = w.findChild<QPushButton *>(name);
  if (!button)
    return false;
  QTest::mouseClick(button, Qt::LeftButton);
  return true;
}
static bool containsMessage(QWidget &w, const QString &text) {
  for (auto *label : w.findChildren<QLabel *>("messageText"))
    if (label->text() == text && label->textFormat() == Qt::PlainText)
      return true;
  return false;
}
static void shot(QWidget &w, const char *name) {
  auto output = qEnvironmentVariable("QTCHAT_OUTPUT_DIR");
  if (output.isEmpty())
    return;
  QDir().mkpath(output);
  w.grab().save(output + "/" + name + ".png");
}
static bool registerAndLogin(MainWindow &w, const QString &username) {
  if (!click(w, "registerLink") ||
      !setText(w, "registrationUsername", username) ||
      !setText(w, "registrationDisplay",
               username == "alice" ? "Alice" : "Bob") ||
      !setText(w, "registrationPassword", "Secret-123") ||
      !setText(w, "registrationConfirm", "Secret-123") ||
      !click(w, "registerButton"))
    return false;
  auto *input = w.findChild<QLineEdit *>("loginUsername");
  if (!waitUntil([&] {
        return input && input->isVisible() && input->text() == username;
      }))
    return false;
  if (!setText(w, "loginPassword", "Secret-123") || !click(w, "loginButton"))
    return false;
  auto *network = w.findChild<NetworkClient *>();
  return waitUntil([&] { return network && network->state() == "online"; });
}
struct LiveServer {
  QTemporaryDir directory;
  QProcess process;
  int port = 0;
  bool start() {
    process.start(
        qEnvironmentVariable("QTCHAT_SERVER"),
        {"--port", QString::number(port), "--db", directory.path() + "/ui.db"});
    if (!process.waitForStarted(3000) || !process.waitForReadyRead(5000))
      return false;
    auto line = process.readLine().trimmed();
    if (!line.startsWith("LISTENING "))
      return false;
    port = line.mid(10).toInt();
    return port > 0;
  }
  void stop() {
    if (process.state() != QProcess::NotRunning) {
      process.terminate();
      if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished();
        QTest::qFail("server graceful shutdown timed out", __FILE__, __LINE__);
      }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
      QTest::qFail(
          qPrintable("server shutdown failed: " +
                     QString::fromUtf8(process.readAllStandardError())),
          __FILE__, __LINE__);
  }
  ~LiveServer() { stop(); }
};
class ClientUiTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
  void loginRejectsEmptyFields() {
    MainWindow w;
    w.show();
    auto *password = w.findChild<QLineEdit *>("loginPassword");
    QVERIFY(password);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    shot(w, "login");
    QVERIFY(click(w, "loginButton"));
    auto *error = w.findChild<QLabel *>("loginError");
    QVERIFY(error);
    QVERIFY(!error->text().isEmpty());
  }
  void registrationRejectsMismatchedPasswords() {
    MainWindow w;
    w.show();
    QVERIFY(click(w, "registerLink"));
    QVERIFY(setText(w, "registrationUsername", "alice"));
    QVERIFY(setText(w, "registrationDisplay", QStringLiteral("爱丽丝")));
    QVERIFY(setText(w, "registrationPassword", "Secret-123"));
    QVERIFY(setText(w, "registrationConfirm", "Different-123"));
    QVERIFY(click(w, "registerButton"));
    auto *error = w.findChild<QLabel *>("registrationError");
    QVERIFY(error);
    QVERIFY(!error->text().isEmpty());
    auto *network = w.findChild<NetworkClient *>();
    QVERIFY(network);
    QCOMPARE(network->state(), "disconnected");
    shot(w, "registration");
  }
  void liveTwoClients() {
    MainWindow a, b;
    a.show();
    b.show();
    auto *na = a.findChild<NetworkClient *>();
    auto *nb = b.findChild<NetworkClient *>();
    QVERIFY(na && nb);
    LiveServer server;
    QVERIFY2(server.start(), qPrintable(QString::fromUtf8(
                                 server.process.readAllStandardError())));
    na->setEndpoint("127.0.0.1", server.port);
    nb->setEndpoint("127.0.0.1", server.port);
    QVERIFY(registerAndLogin(a, "alice"));
    QVERIFY(registerAndLogin(b, "bobby"));
    QVERIFY(click(a, "navAdd"));
    QVERIFY(setText(a, "addUsername", "bobby"));
    QVERIFY(click(a, "searchUserButton"));
    auto *add = a.findChild<QPushButton *>("addFriendButton");
    QVERIFY(add);
    QVERIFY(waitUntil([&] { return add->isEnabled(); }));
    QTest::mouseClick(add, Qt::LeftButton);
    QVERIFY(click(b, "navRequests"));
    QVERIFY(waitUntil([&] {
      return b.findChild<QPushButton *>("acceptFriendButton") != nullptr;
    }));
    QVERIFY(click(b, "acceptFriendButton"));
    auto openFirst = [](MainWindow &w) {
      if (!click(w, "navFriends"))
        return false;
      auto *list = w.findChild<QListWidget *>("conversationList");
      if (!list || !waitUntil([&] { return list->count() == 1; }))
        return false;
      QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                        list->visualItemRect(list->item(0)).center());
      return true;
    };
    QVERIFY(openFirst(a));
    QVERIFY(openFirst(b));
    auto *edit = a.findChild<QPlainTextEdit *>("messageEditor");
    QVERIFY(edit);
    const auto text = QStringLiteral("你好 <img src=x>\n项目联调成功");
    edit->setPlainText(text);
    QTest::keyClick(edit, Qt::Key_Return);
    QVERIFY(waitUntil(
        [&] { return containsMessage(a, text) && containsMessage(b, text); }));
    auto *replyEditor = b.findChild<QPlainTextEdit *>("messageEditor");
    QVERIFY(replyEditor);
    const auto replyText = QStringLiteral("收到！服务端与客户端联调正常。");
    replyEditor->setPlainText(replyText);
    QTest::keyClick(replyEditor, Qt::Key_Return);
    QVERIFY(waitUntil([&] {
      return containsMessage(a, replyText) && containsMessage(b, replyText);
    }));
    edit->setPlainText("line1");
    edit->moveCursor(QTextCursor::End);
    QTest::keyClick(edit, Qt::Key_Return, Qt::ShiftModifier);
    QVERIFY(edit->toPlainText().contains('\n'));
    edit->clear();
    nb->logout();
    QVERIFY(waitUntil([&] { return nb->state() == "disconnected"; }));
    edit->setPlainText(QStringLiteral("这是一条离线消息"));
    QTest::keyClick(edit, Qt::Key_Return);
    QVERIFY(waitUntil([&] {
      return containsMessage(a, QStringLiteral("这是一条离线消息"));
    }));
    QVERIFY(setText(b, "loginPassword", "Secret-123"));
    QVERIFY(click(b, "loginButton"));
    QVERIFY(waitUntil([&] { return nb->state() == "online"; }));
    QVERIFY(openFirst(b));
    QVERIFY(waitUntil([&] {
      return containsMessage(b, QStringLiteral("这是一条离线消息"));
    }));
    server.stop();
    QVERIFY(waitUntil([&] {
      return na->state() == "reconnecting" && nb->state() == "reconnecting";
    }));
    QVERIFY(!a.findChild<QPushButton *>("sendButton")->isEnabled());
    edit->setPlainText(QStringLiteral("重连时保留草稿"));
    QVERIFY(server.start());
    QVERIFY(waitUntil(
        [&] { return na->state() == "online" && nb->state() == "online"; },
        8000));
    QCOMPARE(edit->toPlainText(), QStringLiteral("重连时保留草稿"));
    QVERIFY(waitUntil(
        [&] { return containsMessage(a, text) && containsMessage(b, text); }));
    int count = 0;
    for (auto *label : b.findChildren<QLabel *>("messageText"))
      if (label->text() == text)
        ++count;
    QCOMPARE(count, 1);
    a.resize(1000, 680);
    QTest::qWait(60);
    shot(a, "chat-minimum");
    a.resize(1280, 820);
    QTest::qWait(60);
    shot(a, "chat");
    QTimer::singleShot(0, [] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (auto *box = qobject_cast<QMessageBox *>(widget))
          if (auto *button = box->button(QMessageBox::Yes))
            button->click();
    });
    QVERIFY(click(a, "removeFriendButton"));
    QVERIFY(waitUntil([&] {
      return !a.findChild<QPushButton *>("sendButton")->isEnabled();
    }));
    QVERIFY(containsMessage(a, text));
  }
};
QTEST_MAIN(ClientUiTest)
#include "client_ui_test.moc"
