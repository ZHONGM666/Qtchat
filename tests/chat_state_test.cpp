#include "client/chatpage.h"
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QtTest>
#include <functional>

static QJsonObject user(qint64 id) {
  return {{"id", id},
          {"username", QString("user%1").arg(id)},
          {"display_name", QString("User %1").arg(id)}};
}
static QJsonObject message(qint64 id) {
  return {{"id", id},
          {"sender_id", 2},
          {"receiver_id", 1},
          {"text", QString("record-%1").arg(id)},
          {"is_read", true}};
}
struct Model {
  ChatPage page;
  qint64 first = 1, last = 10;
  bool interleave = false;
  int summaries = 0;
  Model() {
    page.resize(1200, 800);
    QObject::connect(
        &page, &ChatPage::businessRequested, &page,
        [this](QString type, QJsonObject data, QString token) {
          QTimer::singleShot(0, &page, [this, type, data, token] {
            QJsonObject result;
            if (type == "friend_list")
              result = {{"friends", QJsonArray{user(2), user(3)}},
                        {"requests", QJsonArray{}}};
            if (type == "conversations") {
              ++summaries;
              const bool next = data["before_id"].toInteger() != 0;
              result = {{"conversations",
                         QJsonArray{QJsonObject{
                             {"peer", user(next ? 3 : 2)},
                             {"last_message", message(next ? 1 : 10)},
                             {"is_friend", true}}}},
                        {"has_more", !next},
                        {"next_before_id", next ? 0 : 10}};
            }
            if (type == "history") {
              QJsonArray rows;
              bool more = false;
              auto peer = data["peer_id"].toInteger();
              if (peer == 2) {
                const auto after = data["after_id"].toInteger();
                const auto before = data["before_id"].toInteger();
                qint64 lo = first, hi = last;
                if (data.contains("after_id")) {
                  lo = qMax(lo, after + 1);
                  hi = qMin(last, lo + 49);
                  more = hi < last;
                  if (interleave) {
                    interleave = false;
                    page.handleEvent(
                        {{"type", "message"},
                         {"data", QJsonObject{{"message", message(100)}}}});
                  }
                } else {
                  if (before)
                    hi = qMin(hi, before - 1);
                  lo = qMax(lo, hi - 49);
                  more = lo > first;
                }
                for (qint64 id = lo; id <= hi; ++id)
                  rows.append(message(id));
              }
              result = {{"peer", user(peer)},
                        {"is_friend", true},
                        {"messages", rows},
                        {"has_more", more}};
            }
            page.resolve(token, {{"ok", true}, {"data", result}});
          });
        });
    page.show();
    page.setSession(user(1));
    QTest::qWait(50);
    page.findChild<QPushButton *>("navFriends")->click();
  }
  bool open(qint64 peer) {
    auto *list = page.findChild<QListWidget *>("conversationList");
    for (int i = 0; i < list->count(); ++i) {
      auto *item = list->item(i);
      if (item->data(Qt::UserRole).toLongLong() == peer) {
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(item).center());
        return true;
      }
    }
    return false;
  }
  bool contains(qint64 id) {
    for (auto *label : page.findChildren<QLabel *>("messageText"))
      if (label->text() == QString("record-%1").arg(id))
        return true;
    return false;
  }
};
class ChatStateTest : public QObject {
  Q_OBJECT
private slots:
  void draftsBelongToTheirRecipient() {
    Model model;
    QVERIFY(model.open(2));
    auto *editor = model.page.findChild<MessageEditor *>("messageEditor");
    editor->setPlainText("private for user2");
    QVERIFY(model.open(3));
    QCOMPARE(editor->toPlainText(), QString());
    editor->setPlainText("private for user3");
    QVERIFY(model.open(2));
    QCOMPARE(editor->toPlainText(), QString("private for user2"));
    model.page.setOnline(false);
    model.page.setSession(user(1));
    QCOMPARE(editor->toPlainText(), QString("private for user2"));
  }
  void liveMessageDoesNotAdvanceHistoryCursor() {
    Model model;
    QVERIFY(model.open(2));
    QTRY_VERIFY(model.contains(10));
    model.last = 100;
    model.interleave = true;
    model.page.setOnline(false);
    model.page.setSession(user(1));
    QTRY_VERIFY(model.contains(61));
    for (int i = 1; i <= 100; ++i)
      QVERIFY(model.contains(i));
  }
  void reopeningInactiveConversationFillsMiddleGap() {
    Model model;
    model.last = 50;
    QVERIFY(model.open(2));
    QTRY_VERIFY(model.contains(1));
    QVERIFY(model.open(3));
    QTest::qWait(20);
    model.last = 150;
    QVERIFY(model.open(2));
    QTRY_VERIFY(model.contains(150));
    if (!model.contains(51)) {
      for (auto *button : model.page.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("加载更早的消息"))
          button->click();
    }
    QTRY_VERIFY(model.contains(51));
    for (int i = 1; i <= 150; ++i)
      QVERIFY(model.contains(i));
  }
  void conversationPagesAreAllConsumed() {
    Model model;
    QTRY_COMPARE(model.summaries, 2);
  }
};
QTEST_MAIN(ChatStateTest)
#include "chat_state_test.moc"
