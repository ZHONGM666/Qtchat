#include "chatpage.h"
#include "icons.h"
#include <QButtonGroup>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <functional>

void MessageEditor::keyPressEvent(QKeyEvent *event) {
  if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
      !event->modifiers().testFlag(Qt::ShiftModifier)) {
    emit submitted();
    event->accept();
    return;
  }
  QPlainTextEdit::keyPressEvent(event);
}
namespace {
QLabel *plain(QString text, QWidget *parent = nullptr) {
  auto *label = new QLabel(std::move(text), parent);
  label->setTextFormat(Qt::PlainText);
  return label;
}
QString idText(qint64 id) { return QString::number(id); }
QString displayName(const QJsonObject &user) {
  auto text = user["display_name"].toString();
  return text.isEmpty() ? user["username"].toString() : text;
}
QString timeText(const QJsonObject &message) {
  auto date = QDateTime::fromString(message["created_at"].toString(),
                                    Qt::ISODateWithMs);
  return date.isValid() ? date.toLocalTime().toString("HH:mm") : QString();
}
void clearLayout(QLayout *layout) {
  while (auto *item = layout->takeAt(0)) {
    if (item->widget())
      delete item->widget();
    if (item->layout())
      clearLayout(item->layout());
    delete item;
  }
}
} // namespace
struct ChatPage::Impl {
  struct Pending {
    QString id, text, status = QStringLiteral("待确认");
    bool retryable = true;
  };
  struct Conversation {
    QJsonObject peer;
    bool isFriend = false, hasOlder = false, historyKnown = false,
         historyLoading = false;
    QMap<qint64, QJsonObject> messages;
    QMap<QString, Pending> pending;
    QString preview, draft;
    int unread = 0;
    qint64 lastId = 0, summaryId = 0, readSent = 0, syncCursor = 0,
           olderCursor = 0;
  };
  ChatPage *q;
  QJsonObject self;
  bool online = false, friendMode = false;
  qint64 selected = 0;
  QMap<qint64, Conversation> conversations;
  QHash<QString, std::function<void(QJsonObject)>> callbacks;
  QLabel *profileName, *profileAvatar, *connectionStatus, *peerName,
      *peerStatus, *peerAvatar, *notice, *searchResult;
  QListWidget *contacts, *requests;
  QLineEdit *search, *addUsername;
  QPushButton *sendButton, *more, *remove, *addFriend, *searchUser;
  MessageEditor *editor;
  QStackedWidget *details;
  QWidget *chat, *addPanel, *requestPanel, *canvas;
  QScrollArea *scroll;
  QVBoxLayout *bubbles;
  QPushButton *navChats, *navFriends, *navAdd, *navRequests;
  QJsonObject searched;
  explicit Impl(ChatPage *owner) : q(owner) {
    q->setObjectName("chatPage");
    auto *root = new QHBoxLayout(q);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *side = new QWidget(q);
    side->setObjectName("sidebar");
    side->setFixedWidth(188);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(12, 24, 12, 16);
    sideLayout->setSpacing(10);
    auto *profile = new QHBoxLayout;
    profileAvatar = plain({});
    profileName = plain({});
    profileName->setObjectName("profileName");
    auto *names = new QVBoxLayout;
    names->addWidget(profileName);
    names->addWidget(plain(QStringLiteral("我的账号")));
    profile->addWidget(profileAvatar);
    profile->addLayout(names);
    sideLayout->addLayout(profile);
    sideLayout->addSpacing(28);
    auto *group = new QButtonGroup(q);
    group->setExclusive(true);
    auto navigation = [&](QString text, const char *name, QString icon) {
      auto *button = new QPushButton(text, side);
      button->setObjectName(name);
      button->setProperty("nav", true);
      button->setCheckable(true);
      button->setIcon(lineIcon(icon));
      button->setIconSize(QSize(24, 24));
      QObject::connect(
          button, &QPushButton::toggled, q, [button, icon](bool selected) {
            button->setIcon(lineIcon(icon, selected ? QColor(Qt::white)
                                                    : QColor("#545568")));
          });
      button->setMinimumHeight(50);
      group->addButton(button);
      sideLayout->addWidget(button);
      return button;
    };
    navChats = navigation(QStringLiteral("会话"), "navChats", "chat");
    navFriends = navigation(QStringLiteral("好友"), "navFriends", "friends");
    navAdd = navigation(QStringLiteral("添加好友"), "navAdd", "add");
    navRequests = navigation(QStringLiteral("系统消息"), "navRequests", "bell");
    navChats->setChecked(true);
    auto *settings = new QPushButton(QStringLiteral("设置"), side);
    settings->setProperty("nav", true);
    settings->setIcon(lineIcon("settings"));
    settings->setIconSize(QSize(24, 24));
    sideLayout->addWidget(settings);
    sideLayout->addStretch();
    auto *logout = new QPushButton(QStringLiteral("退出登录"), side);
    logout->setObjectName("logoutButton");
    logout->setProperty("link", true);
    sideLayout->addWidget(logout);
    connectionStatus = plain(QStringLiteral("● 未连接"), side);
    connectionStatus->setWordWrap(true);
    sideLayout->addWidget(connectionStatus);
    root->addWidget(side);
    auto *middle = new QWidget(q);
    middle->setObjectName("contactPanel");
    middle->setFixedWidth(280);
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(10, 18, 10, 10);
    middleLayout->setSpacing(12);
    search = new QLineEdit(middle);
    search->setObjectName("contactSearch");
    search->setPlaceholderText(QStringLiteral("搜索联系人"));
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setMinimumHeight(42);
    middleLayout->addWidget(search);
    auto *category = plain(QStringLiteral("联系人与最近会话"), middle);
    category->setProperty("muted", true);
    middleLayout->addWidget(category);
    contacts = new QListWidget(middle);
    contacts->setObjectName("conversationList");
    contacts->setSpacing(2);
    middleLayout->addWidget(contacts, 1);
    root->addWidget(middle);
    auto *right = new QWidget(q);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    notice = plain({}, right);
    notice->setWordWrap(true);
    notice->setStyleSheet("padding:5px 14px;color:#aa6b31;background:#fff5e8;");
    notice->hide();
    rightLayout->addWidget(notice);
    details = new QStackedWidget(right);
    rightLayout->addWidget(details, 1);
    root->addWidget(right, 1);
    chat = new QWidget(details);
    auto *chatLayout = new QVBoxLayout(chat);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);
    auto *header = new QWidget(chat);
    header->setObjectName("chatHeader");
    header->setMinimumHeight(78);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 12, 18, 12);
    peerAvatar = plain({});
    headerLayout->addWidget(peerAvatar);
    auto *peerNames = new QVBoxLayout;
    peerName = plain(QStringLiteral("选择一个好友开始聊天"));
    peerName->setObjectName("peerName");
    peerStatus = plain(QStringLiteral("支持实时文字聊天与历史记录"));
    peerStatus->setProperty("muted", true);
    peerNames->addWidget(peerName);
    peerNames->addWidget(peerStatus);
    headerLayout->addLayout(peerNames, 1);
    remove = new QPushButton(QStringLiteral("删除好友"), header);
    remove->setObjectName("removeFriendButton");
    remove->setProperty("link", true);
    remove->hide();
    headerLayout->addWidget(remove);
    chatLayout->addWidget(header);
    more = new QPushButton(QStringLiteral("加载更早的消息"), chat);
    more->setObjectName("loadHistoryButton");
    more->setProperty("link", true);
    more->hide();
    chatLayout->addWidget(more);
    scroll = new QScrollArea(chat);
    scroll->setObjectName("messageScroll");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    canvas = new QWidget(scroll);
    canvas->setObjectName("messageCanvas");
    bubbles = new QVBoxLayout(canvas);
    bubbles->setContentsMargins(18, 20, 18, 20);
    bubbles->setSpacing(16);
    bubbles->setAlignment(Qt::AlignTop);
    scroll->setWidget(canvas);
    chatLayout->addWidget(scroll, 1);
    auto *composer = new QWidget(chat);
    composer->setObjectName("composer");
    auto *composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(16, 10, 16, 14);
    auto *hint =
        plain(QStringLiteral("Enter 发送   ·   Shift+Enter 换行"), composer);
    hint->setProperty("muted", true);
    composerLayout->addWidget(hint);
    editor = new MessageEditor(composer);
    editor->setObjectName("messageEditor");
    editor->setPlaceholderText(QStringLiteral("输入消息…"));
    editor->setMinimumHeight(90);
    editor->setMaximumHeight(130);
    composerLayout->addWidget(editor);
    auto *sendRow = new QHBoxLayout;
    sendRow->addStretch();
    sendButton = new QPushButton(QStringLiteral("发送"), composer);
    sendButton->setObjectName("sendButton");
    sendButton->setIcon(lineIcon("send", Qt::white));
    sendButton->setMinimumHeight(38);
    sendRow->addWidget(sendButton);
    composerLayout->addLayout(sendRow);
    chatLayout->addWidget(composer);
    details->addWidget(chat);
    addPanel = new QWidget(details);
    auto *addLayout = new QVBoxLayout(addPanel);
    addLayout->setContentsMargins(32, 32, 32, 32);
    auto *addTitle = plain(QStringLiteral("添加好友"));
    addTitle->setStyleSheet("font-size:24px;font-weight:650;");
    addLayout->addWidget(addTitle);
    addLayout->addWidget(
        plain(QStringLiteral("输入完整用户名查找用户并发送申请")));
    addUsername = new QLineEdit(addPanel);
    addUsername->setObjectName("addUsername");
    addUsername->setPlaceholderText(QStringLiteral("用户名"));
    addUsername->setMinimumHeight(46);
    addLayout->addWidget(addUsername);
    searchUser = new QPushButton(QStringLiteral("查找用户"), addPanel);
    searchUser->setObjectName("searchUserButton");
    addLayout->addWidget(searchUser);
    searchResult = plain({}, addPanel);
    searchResult->setWordWrap(true);
    addLayout->addWidget(searchResult);
    addFriend = new QPushButton(QStringLiteral("发送好友申请"), addPanel);
    addFriend->setObjectName("addFriendButton");
    addFriend->setEnabled(false);
    addLayout->addWidget(addFriend);
    addLayout->addStretch();
    details->addWidget(addPanel);
    requestPanel = new QWidget(details);
    auto *requestLayout = new QVBoxLayout(requestPanel);
    requestLayout->setContentsMargins(24, 24, 24, 24);
    auto *requestTitle = plain(QStringLiteral("好友申请"));
    requestTitle->setStyleSheet("font-size:24px;font-weight:650;");
    requestLayout->addWidget(requestTitle);
    requests = new QListWidget(requestPanel);
    requests->setObjectName("friendRequests");
    requests->setStyleSheet("QListWidget{border:0;background:transparent;}");
    requestLayout->addWidget(requests);
    details->addWidget(requestPanel);
    QObject::connect(navChats, &QPushButton::clicked, q, [this] {
      friendMode = false;
      details->setCurrentWidget(chat);
      refreshList();
    });
    QObject::connect(navFriends, &QPushButton::clicked, q, [this] {
      friendMode = true;
      details->setCurrentWidget(chat);
      refreshList();
    });
    QObject::connect(navAdd, &QPushButton::clicked, q,
                     [this] { details->setCurrentWidget(addPanel); });
    QObject::connect(navRequests, &QPushButton::clicked, q, [this] {
      details->setCurrentWidget(requestPanel);
      syncFriends();
    });
    QObject::connect(settings, &QPushButton::clicked, q,
                     &ChatPage::settingsRequested);
    QObject::connect(logout, &QPushButton::clicked, q,
                     &ChatPage::logoutRequested);
    QObject::connect(search, &QLineEdit::textChanged, q,
                     [this] { refreshList(); });
    QObject::connect(contacts, &QListWidget::itemClicked, q,
                     [this](QListWidgetItem *item) {
                       open(item->data(Qt::UserRole).toLongLong());
                     });
    QObject::connect(editor, &QPlainTextEdit::textChanged, q,
                     [this] { updateControls(); });
    QObject::connect(editor, &MessageEditor::submitted, q,
                     [this] { submitMessage(); });
    QObject::connect(sendButton, &QPushButton::clicked, q,
                     [this] { submitMessage(); });
    QObject::connect(more, &QPushButton::clicked, q,
                     [this] { load(selected, true, false); });
    QObject::connect(remove, &QPushButton::clicked, q, [this] {
      if (!selected || !online)
        return;
      if (QMessageBox::question(
              q, QStringLiteral("删除好友"),
              QStringLiteral("解除双方好友关系？聊天记录将保留。"),
              QMessageBox::Yes | QMessageBox::No,
              QMessageBox::No) != QMessageBox::Yes)
        return;
      auto peer = selected;
      post("friend_remove", {{"peer_id", peer}},
           [this, peer](QJsonObject reply) {
             if (!good(reply))
               return;
             conversations[peer].isFriend = false;
             sync();
             updateControls();
           });
    });
    QObject::connect(addUsername, &QLineEdit::textChanged, q, [this] {
      searched = {};
      addFriend->setEnabled(false);
    });
    QObject::connect(searchUser, &QPushButton::clicked, q, [this] {
      searchUser->setEnabled(false);
      post("user_search", {{"username", addUsername->text().trimmed()}},
           [this](QJsonObject reply) {
             searchUser->setEnabled(online);
             if (!good(reply))
               return;
             auto data = reply["data"].toObject();
             searched = data["user"].toObject();
             searchResult->setText(displayName(searched) + "  @" +
                                   searched["username"].toString());
             addFriend->setEnabled(online && !data["is_friend"].toBool() &&
                                   searched["id"].toInteger() !=
                                       self["id"].toInteger());
           });
    });
    QObject::connect(addFriend, &QPushButton::clicked, q, [this] {
      auto id = searched["id"].toInteger();
      if (!id)
        return;
      addFriend->setEnabled(false);
      post("friend_request", {{"peer_id", id}}, [this](QJsonObject reply) {
        if (good(reply))
          note(reply["data"].toObject()["outgoing"].toBool()
                   ? QStringLiteral("申请已发送，等待对方确认")
                   : QStringLiteral("对方已向你申请，请到系统消息处理"));
        syncFriends();
      });
    });
    updateControls();
  }
  void note(QString text) {
    notice->setText(text);
    notice->setVisible(!text.isEmpty());
  }
  bool good(const QJsonObject &reply) {
    if (reply["ok"].toBool())
      return true;
    note(reply["error"].toObject()["message"].toString(
        QStringLiteral("操作失败")));
    return false;
  }
  void post(QString type, QJsonObject data,
            std::function<void(QJsonObject)> callback) {
    auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    callbacks.insert(token, std::move(callback));
    emit q->businessRequested(type, data, token);
  }
  void sync() {
    if (!online)
      return;
    syncFriends();
    syncConversations();
    if (selected)
      load(selected, false, true);
  }
  void syncConversations(qint64 before = 0) {
    post("conversations", {{"before_id", before}},
         [this, before](QJsonObject reply) {
           if (!good(reply))
             return;
           for (const auto &value :
                reply["data"].toObject()["conversations"].toArray()) {
             auto item = value.toObject(), peer = item["peer"].toObject();
             auto id = peer["id"].toInteger();
             auto &c = conversations[id];
             c.peer = peer;
             c.isFriend = item["is_friend"].toBool();
             auto last = item["last_message"].toObject();
             c.summaryId = last["id"].toInteger();
             c.unread = item["unread"].toInt();
             for (const auto &message : c.messages)
               if (message["id"].toInteger() > c.summaryId &&
                   message["receiver_id"].toInteger() ==
                       self["id"].toInteger() &&
                   !message["is_read"].toBool())
                 ++c.unread;
             if (c.summaryId >= c.lastId) {
               c.lastId = c.summaryId;
               c.preview = last["text"].toString();
             }
           }
           refreshList();
           markRead();
           auto data = reply["data"].toObject();
           const auto next = data["next_before_id"].toInteger();
           if (data["has_more"].toBool() && next > 0 &&
               (!before || next < before))
             syncConversations(next);
         });
  }
  void syncFriends() {
    if (!online)
      return;
    post("friend_list", {}, [this](QJsonObject reply) {
      if (!good(reply))
        return;
      auto data = reply["data"].toObject();
      for (auto &c : conversations)
        c.isFriend = false;
      for (const auto &value : data["friends"].toArray()) {
        auto peer = value.toObject();
        auto &c = conversations[peer["id"].toInteger()];
        c.peer = peer;
        c.isFriend = true;
      }
      renderRequests(data["requests"].toArray());
      refreshList();
      updateControls();
    });
  }
  void renderRequests(const QJsonArray &values) {
    requests->clear();
    for (const auto &value : values) {
      auto data = value.toObject();
      auto *item = new QListWidgetItem(requests);
      auto *row = new QWidget(requests);
      auto *layout = new QHBoxLayout(row);
      layout->addWidget(plain(displayName(data["from"].toObject()) +
                              QStringLiteral(" 请求添加好友")),
                        1);
      auto *yes = new QPushButton(QStringLiteral("接受"), row);
      yes->setObjectName("acceptFriendButton");
      auto *no = new QPushButton(QStringLiteral("拒绝"), row);
      layout->addWidget(yes);
      layout->addWidget(no);
      item->setSizeHint(QSize(300, 76));
      requests->setItemWidget(item, row);
      auto respond = [this, data, yes, no](bool accepted) {
        yes->setEnabled(false);
        no->setEnabled(false);
        post("friend_respond",
             {{"request_id", data["id"]}, {"accept", accepted}},
             [this](QJsonObject reply) {
               good(reply);
               sync();
             });
      };
      QObject::connect(yes, &QPushButton::clicked, q,
                       [respond] { respond(true); });
      QObject::connect(no, &QPushButton::clicked, q,
                       [respond] { respond(false); });
    }
  }
  void refreshList() {
    contacts->clear();
    QVector<qint64> ids;
    for (auto it = conversations.cbegin(); it != conversations.cend(); ++it)
      if ((friendMode ? it->isFriend
                      : it->lastId > 0 || !it->pending.empty()) &&
          (displayName(it->peer).contains(search->text(),
                                          Qt::CaseInsensitive) ||
           it->peer["username"].toString().contains(search->text(),
                                                    Qt::CaseInsensitive)))
        ids.push_back(it.key());
    std::sort(ids.begin(), ids.end(), [this](qint64 a, qint64 b) {
      if (friendMode)
        return displayName(conversations[a].peer)
                   .localeAwareCompare(displayName(conversations[b].peer)) < 0;
      return conversations[a].lastId > conversations[b].lastId;
    });
    for (auto id : ids) {
      const auto &c = conversations[id];
      auto *item = new QListWidgetItem(contacts);
      item->setData(Qt::UserRole, id);
      item->setSizeHint(QSize(240, 78));
      auto *row = new QWidget(contacts);
      auto *layout = new QHBoxLayout(row);
      layout->setContentsMargins(8, 8, 8, 8);
      auto *picture = plain({});
      picture->setPixmap(avatar(displayName(c.peer), id, 44));
      layout->addWidget(picture);
      auto *lines = new QVBoxLayout;
      auto *name = plain(displayName(c.peer));
      name->setStyleSheet("font-weight:650;font-size:16px;");
      lines->addWidget(name);
      auto hint = friendMode
                      ? (c.peer["online"].toBool() ? QStringLiteral("● 在线")
                                                   : QStringLiteral("离线"))
                      : c.preview.simplified().left(20);
      auto *preview = plain(hint);
      preview->setProperty("muted", true);
      lines->addWidget(preview);
      layout->addLayout(lines, 1);
      if (c.unread) {
        auto *badge = plain(QString::number(c.unread));
        badge->setStyleSheet("background:#ef626b;color:white;border-radius:"
                             "10px;padding:1px 5px;");
        layout->addWidget(badge);
      }
      contacts->setItemWidget(item, row);
      if (id == selected)
        item->setSelected(true);
    }
  }
  void open(qint64 id) {
    if (!conversations.contains(id))
      return;
    if (selected != id) {
      if (selected)
        conversations[selected].draft = editor->toPlainText();
      selected = id;
      editor->setPlainText(conversations[id].draft);
    }
    details->setCurrentWidget(chat);
    note({});
    render();
    load(id, false, conversations[id].historyKnown);
  }
  void load(qint64 peer, bool older, bool incremental) {
    if (!peer || !online)
      return;
    auto &c = conversations[peer];
    if (c.historyLoading)
      return;
    c.historyLoading = true;
    QJsonObject data{{"peer_id", peer}};
    bool after = incremental && c.historyKnown;
    const auto cursor = after ? c.syncCursor : c.olderCursor;
    if (older && cursor > 0)
      data["before_id"] = cursor;
    else if (after)
      data["after_id"] = cursor;
    post("history", data,
         [this, peer, older, after, cursor](QJsonObject reply) {
           auto &c = conversations[peer];
           c.historyLoading = false;
           if (!good(reply))
             return;
           auto data = reply["data"].toObject();
           c.peer = data["peer"].toObject();
           c.isFriend = data["is_friend"].toBool();
           auto records = data["messages"].toArray();
           for (const auto &value : records) {
             auto record = value.toObject();
             c.messages[record["id"].toInteger()] = record;
             if (record["id"].toInteger() > c.lastId) {
               c.lastId = record["id"].toInteger();
               c.preview = record["text"].toString();
             }
           }
           const auto first =
               records.isEmpty() ? 0
                                 : records.first().toObject()["id"].toInteger();
           const auto last = records.isEmpty()
                                 ? cursor
                                 : records.last().toObject()["id"].toInteger();
           // Only history responses advance cursors. Live events and
           // acknowledgements can be newer than records still missing between
           // two history pages.
           if (after) {
             c.syncCursor = last;
           } else {
             c.olderCursor = first;
             c.hasOlder = data["has_more"].toBool();
             if (!older) {
               c.historyKnown = true;
               c.syncCursor = last;
             }
           }
           if (selected == peer) {
             render(!older);
             markRead();
           }
           refreshList();
           if (after && data["has_more"].toBool() && last > cursor)
             load(peer, false, true);
         });
  }
  void updateControls() {
    bool available = selected && conversations.contains(selected);
    bool canSend = online && available && conversations[selected].isFriend;
    editor->setEnabled(available);
    sendButton->setEnabled(canSend &&
                           !editor->toPlainText().trimmed().isEmpty() &&
                           editor->toPlainText().toUtf8().size() <= 8192);
    more->setVisible(available && conversations[selected].hasOlder);
    more->setEnabled(online);
    remove->setVisible(available && conversations[selected].isFriend);
    remove->setEnabled(online);
    searchUser->setEnabled(online);
    if (available) {
      auto &c = conversations[selected];
      peerName->setText(displayName(c.peer));
      peerAvatar->setPixmap(avatar(displayName(c.peer), selected, 46));
      peerStatus->setText(
          !c.isFriend ? QStringLiteral("已解除好友关系 · 历史记录可查看")
          : !online   ? QStringLiteral("连接中断，正在重连…")
          : c.peer["online"].toBool()
              ? QStringLiteral("● 在线")
              : QStringLiteral("离线 · 消息将在登录后可见"));
      peerStatus->setStyleSheet(online && c.isFriend &&
                                        c.peer["online"].toBool()
                                    ? "color:#29986b;font-size:12px;"
                                    : "color:#7a7d90;font-size:12px;");
    }
  }
  void render(bool bottom = true) {
    int previous = scroll->verticalScrollBar()->value(),
        maximum = scroll->verticalScrollBar()->maximum();
    clearLayout(bubbles);
    if (!selected) {
      bubbles->addWidget(plain(QStringLiteral("添加好友，开始你的第一段会话")));
      updateControls();
      return;
    }
    auto &c = conversations[selected];
    for (const auto &message : c.messages)
      addBubble(message["text"].toString(),
                message["sender_id"].toInteger() == self["id"].toInteger(),
                timeText(message), {});
    for (const auto &pending : c.pending)
      addBubble(pending.text, true, pending.status,
                pending.status == QStringLiteral("发送失败") ? pending.id
                                                             : QString());
    updateControls();
    const auto current = selected;
    QTimer::singleShot(0, q, [this, bottom, previous, maximum, current] {
      if (current != selected)
        return;
      auto *bar = scroll->verticalScrollBar();
      bar->setValue(bottom ? bar->maximum()
                           : previous + bar->maximum() - maximum);
    });
  }
  void addBubble(const QString &text, bool own, const QString &status,
                 const QString &retry) {
    auto *row = new QWidget(canvas);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    if (own)
      layout->addStretch();
    auto *picture = plain({}, row);
    picture->setPixmap(avatar(own ? displayName(self)
                                  : displayName(conversations[selected].peer),
                              own ? self["id"].toInteger() : selected, 38));
    if (!own)
      layout->addWidget(picture, 0, Qt::AlignTop);
    auto *bubble = new QWidget(row);
    bubble->setMaximumWidth(
        std::max(240, std::min(620, scroll->viewport()->width() * 3 / 4)));
    bubble->setStyleSheet(
        own ? "QWidget{background:#7963f1;border-radius:12px;color:white;}"
            : "QWidget{background:white;border:1px solid "
              "#e1e3ef;border-radius:12px;color:#282939;}");
    auto *content = new QVBoxLayout(bubble);
    content->setContentsMargins(13, 10, 13, 8);
    content->setSpacing(5);
    auto *message = plain(text, bubble);
    message->setObjectName("messageText");
    message->setWordWrap(true);
    message->setStyleSheet("border:0;background:transparent;font-size:16px;");
    content->addWidget(message);
    auto *stamp = plain(status, bubble);
    stamp->setStyleSheet(
        own ? "border:0;background:transparent;color:#e8e3ff;font-size:11px;"
            : "border:0;background:transparent;color:#9396a7;font-size:11px;");
    stamp->setAlignment(Qt::AlignRight);
    content->addWidget(stamp);
    if (!retry.isEmpty()) {
      auto *button = new QPushButton(QStringLiteral("重试"), bubble);
      button->setEnabled(online && conversations[selected].isFriend);
      button->setStyleSheet(
          "color:white;border:1px solid #c3b8ff;background:transparent;");
      content->addWidget(button);
      auto peer = selected;
      QObject::connect(button, &QPushButton::clicked, q,
                       [this, peer, retry] { sendPending(peer, retry); });
    }
    layout->addWidget(bubble);
    if (own)
      layout->addWidget(picture, 0, Qt::AlignTop);
    else
      layout->addStretch();
    bubbles->addWidget(row);
  }
  void markRead() {
    if (!online || !selected || details->currentWidget() != chat ||
        !q->isVisible() || q->window()->isMinimized())
      return;
    auto &c = conversations[selected];
    qint64 maximum = 0;
    for (auto &message : c.messages)
      if (message["receiver_id"].toInteger() == self["id"].toInteger()) {
        maximum = std::max(maximum, message["id"].toInteger());
        message["is_read"] = true;
      }
    c.unread = 0;
    if (maximum <= c.readSent)
      return;
    c.readSent = maximum;
    auto peer = selected;
    post("read", {{"peer_id", peer}, {"up_to_id", maximum}},
         [this, peer](QJsonObject reply) {
           if (!reply["ok"].toBool())
             conversations[peer].readSent = 0;
         });
    refreshList();
  }
  void submitMessage() {
    if (!online || !selected || !conversations[selected].isFriend)
      return;
    auto text = editor->toPlainText();
    if (text.trimmed().isEmpty() || text.toUtf8().size() > 8192) {
      note(QStringLiteral("消息不能为空或超过8KiB"));
      return;
    }
    auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    conversations[selected].pending.insert(id, {id, text});
    conversations[selected].preview = text;
    conversations[selected].draft.clear();
    editor->clear();
    render();
    sendPending(selected, id);
    refreshList();
  }
  void sendPending(qint64 peer, const QString &id) {
    auto &c = conversations[peer];
    if (!c.pending.contains(id) || !online)
      return;
    c.pending[id].status = QStringLiteral("待确认");
    auto text = c.pending[id].text;
    post("message_send", {{"peer_id", peer}, {"client_id", id}, {"text", text}},
         [this, peer, id](QJsonObject reply) {
           auto &c = conversations[peer];
           if (!c.pending.contains(id))
             return;
           if (reply["ok"].toBool()) {
             auto message = reply["data"].toObject()["message"].toObject();
             auto serverId = message["id"].toInteger();
             c.messages[serverId] = message;
             c.pending.remove(id);
             if (serverId > c.lastId) {
               c.lastId = serverId;
               c.preview = message["text"].toString();
             }
           } else {
             auto code = reply["error"].toObject()["code"].toString();
             c.pending[id].status = QStringLiteral("发送失败");
             c.pending[id].retryable = code == "DISCONNECTED" ||
                                       code == "TIMEOUT" || code == "OFFLINE" ||
                                       code == "BUSY" || code == "SERVER_ERROR";
             note(reply["error"].toObject()["message"].toString());
           }
           if (selected == peer)
             render();
           refreshList();
         });
  }
};

ChatPage::ChatPage(QWidget *parent)
    : QWidget(parent), impl_(std::make_unique<Impl>(this)) {}
ChatPage::~ChatPage() = default;
void ChatPage::setSession(QJsonObject user) {
  if (impl_->self["id"] != user["id"])
    resetSession();
  impl_->self = user;
  impl_->profileName->setText(displayName(user));
  impl_->profileAvatar->setPixmap(
      avatar(displayName(user), user["id"].toInteger(), 48));
  setOnline(true);
  impl_->sync();
  for (auto it = impl_->conversations.begin(); it != impl_->conversations.end();
       ++it) {
    QStringList ids;
    for (const auto &pending : it->pending)
      if (pending.retryable)
        ids.push_back(pending.id);
    for (const auto &id : ids)
      impl_->sendPending(it.key(), id);
  }
}
void ChatPage::setOnline(bool online) {
  impl_->online = online;
  impl_->connectionStatus->setText(online
                                       ? QStringLiteral("● 服务器已连接")
                                       : QStringLiteral("● 连接中断 / 重连中"));
  impl_->connectionStatus->setStyleSheet(online ? "color:#29986b;"
                                                : "color:#c18b41;");
  impl_->updateControls();
}
void ChatPage::resetSession() {
  impl_->callbacks.clear();
  impl_->conversations.clear();
  impl_->self = {};
  impl_->selected = 0;
  impl_->online = false;
  impl_->contacts->clear();
  impl_->requests->clear();
  impl_->editor->clear();
  impl_->details->setCurrentWidget(impl_->chat);
  impl_->peerName->setText(QStringLiteral("选择一个好友开始聊天"));
  impl_->peerStatus->clear();
  impl_->peerAvatar->clear();
  impl_->render();
}
void ChatPage::resolve(QString token, QJsonObject reply) {
  auto callback = impl_->callbacks.take(token);
  if (callback)
    callback(std::move(reply));
}
void ChatPage::handleEvent(QJsonObject event) {
  auto type = event["type"].toString();
  auto data = event["data"].toObject();
  if (type == "friends_changed" || type == "friend_request") {
    impl_->sync();
    return;
  }
  if (type == "presence") {
    auto id = data["user_id"].toInteger();
    if (impl_->conversations.contains(id)) {
      impl_->conversations[id].peer["online"] = data["online"];
      impl_->refreshList();
      impl_->updateControls();
    }
    return;
  }
  if (type == "message") {
    auto message = data["message"].toObject();
    auto id = message["id"].toInteger(),
         peer = message["sender_id"].toInteger();
    if (id <= 0 || peer <= 0)
      return;
    auto &c = impl_->conversations[peer];
    if (c.messages.contains(id))
      return;
    if (c.peer.isEmpty())
      c.peer = {{"id", peer},
                {"display_name", QStringLiteral("用户 ") + idText(peer)}};
    c.messages[id] = message;
    if (id > c.lastId) {
      c.lastId = id;
      c.preview = message["text"].toString();
    }
    if (id > c.summaryId)
      ++c.unread;
    if (impl_->selected == peer) {
      impl_->render();
      impl_->markRead();
    }
    impl_->refreshList();
  }
}
void ChatPage::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  if (impl_)
    impl_->render(false);
}
