#include "service.h"
#include "password.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sqlite3.h>
#include <stdexcept>

namespace qtchat {
using Json = nlohmann::json;
namespace {
struct RequestError : std::runtime_error {
  std::string code;
  RequestError(std::string c, std::string message)
      : std::runtime_error(std::move(message)), code(std::move(c)) {}
};
void require(bool condition, const char *code, const char *message) {
  if (!condition)
    throw RequestError(code, message);
}
struct Statement {
  sqlite3 *db;
  sqlite3_stmt *value = nullptr;
  Statement(sqlite3 *connection, const std::string &sql) : db(connection) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &value, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(value); }
  Statement(const Statement &) = delete;
  void number(int i, std::int64_t n) {
    if (sqlite3_bind_int64(value, i, n) != SQLITE_OK)
      throw std::runtime_error("SQL bind failed");
  }
  void text(int i, const std::string &s) {
    if (sqlite3_bind_text(value, i, s.data(), static_cast<int>(s.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
      throw std::runtime_error("SQL bind failed");
  }
  bool row() {
    int rc = sqlite3_step(value);
    if (rc == SQLITE_ROW)
      return true;
    if (rc == SQLITE_DONE)
      return false;
    throw std::runtime_error(sqlite3_errmsg(db));
  }
  void execute() {
    if (row())
      throw std::runtime_error("unexpected SQL row");
  }
  std::int64_t number(int i) const { return sqlite3_column_int64(value, i); }
  std::string text(int i) const {
    const auto *p = sqlite3_column_text(value, i);
    return p ? std::string(reinterpret_cast<const char *>(p),
                           sqlite3_column_bytes(value, i))
             : std::string();
  }
};
struct Transaction {
  sqlite3 *db;
  bool committed = false;
  explicit Transaction(sqlite3 *p) : db(p) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) !=
        SQLITE_OK)
      throw std::runtime_error("cannot begin transaction");
  }
  ~Transaction() {
    if (!committed)
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  void commit() {
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error("cannot commit transaction");
    committed = true;
  }
};
std::string field(const Json &data, const char *key, std::size_t max) {
  require(data.contains(key) && data[key].is_string(), "INVALID_REQUEST",
          "缺少文本参数或类型错误");
  auto s = data[key].get<std::string>();
  require(s.size() <= max, "INVALID_REQUEST", "文本参数过长");
  return s;
}
std::int64_t integer(const Json &data, const char *key, bool zero = false) {
  require(data.contains(key) && data[key].is_number_integer(),
          "INVALID_REQUEST", "缺少整数参数或类型错误");
  if (data[key].is_number_unsigned())
    require(data[key].get<std::uint64_t>() <=
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()),
            "INVALID_REQUEST", "整数超出范围");
  auto n = data[key].get<std::int64_t>();
  require(n >= (zero ? 0 : 1), "INVALID_REQUEST", "整数超出范围");
  return n;
}
bool usernameValid(const std::string &s) {
  return s.size() >= 3 && s.size() <= 32 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
         });
}
bool uuidValid(const std::string &s) {
  if (s.size() != 36)
    return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-')
        return false;
    } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
                 (s[i] >= 'A' && s[i] <= 'F')))
      return false;
  }
  return true;
}
Json user(Statement &q, int first = 0) {
  return {{"id", q.number(first)},
          {"username", q.text(first + 1)},
          {"display_name", q.text(first + 2)}};
}
Json findUser(sqlite3 *db, std::int64_t id) {
  Statement q(db, "SELECT id,username,display_name FROM users WHERE id=?");
  q.number(1, id);
  require(q.row(), "NOT_FOUND", "用户不存在");
  return user(q);
}
bool friends(sqlite3 *db, std::int64_t a, std::int64_t b) {
  Statement q(db, "SELECT 1 FROM friendships WHERE user_low=? AND user_high=?");
  q.number(1, std::min(a, b));
  q.number(2, std::max(a, b));
  return q.row();
}
constexpr const char *columns =
    "id,sender_id,receiver_id,client_id,body,created_at,is_read";
Json message(Statement &q) {
  return {{"id", q.number(0)},          {"sender_id", q.number(1)},
          {"receiver_id", q.number(2)}, {"client_id", q.text(3)},
          {"text", q.text(4)},          {"created_at", q.text(5)},
          {"is_read", q.number(6) != 0}};
}
Json event(const std::string &type, Json data) {
  return {{"version", 1},
          {"type", type},
          {"event", true},
          {"data", std::move(data)}};
}
std::string metadata(const Json &r, const char *key) {
  return r.is_object() && r.contains(key) && r[key].is_string()
             ? r[key].get<std::string>()
             : "";
}
Json reply(const Json &r, Json data) {
  return {{"version", 1},
          {"type", metadata(r, "type")},
          {"request_id", metadata(r, "request_id")},
          {"ok", true},
          {"data", std::move(data)}};
}
Json failure(const Json &r, const std::string &code, const std::string &text) {
  return {{"version", 1},
          {"type", metadata(r, "type")},
          {"request_id", metadata(r, "request_id")},
          {"ok", false},
          {"error", {{"code", code}, {"message", text}}}};
}
const std::map<std::string, std::set<std::string>> fields = {
    {"register", {"username", "display_name", "password"}},
    {"login", {"username", "password"}},
    {"logout", {}},
    {"ping", {}},
    {"user_search", {"username"}},
    {"friend_list", {}},
    {"friend_request", {"peer_id"}},
    {"friend_respond", {"request_id", "accept"}},
    {"friend_remove", {"peer_id"}},
    {"message_send", {"peer_id", "client_id", "text"}},
    {"history", {"peer_id", "before_id", "after_id", "limit"}},
    {"conversations", {"before_id", "limit"}},
    {"read", {"peer_id", "up_to_id"}}};
} // namespace

std::vector<std::int64_t> Service::friendIds(std::int64_t uid) {
  return database_
      .submit([uid](sqlite3 *db) -> Json {
        Statement q(
            db, "SELECT CASE WHEN user_low=? THEN user_high ELSE user_low END "
                "FROM friendships WHERE user_low=? OR user_high=? ORDER BY 1");
        q.number(1, uid);
        q.number(2, uid);
        q.number(3, uid);
        Json rows = Json::array();
        while (q.row())
          rows.push_back(q.number(0));
        return rows;
      })
      .get()
      .get<std::vector<std::int64_t>>();
}

ServiceResult Service::execute(RequestContext context, const Json &request) {
  ServiceResult result;
  try {
    require(request.is_object() && request.contains("version") &&
                request["version"].is_number_integer() &&
                request["version"] == 1,
            "INVALID_REQUEST", "协议版本不正确");
    const auto type = metadata(request, "type"),
               requestId = metadata(request, "request_id");
    require(!requestId.empty() && requestId.size() <= 64 && fields.count(type),
            "INVALID_REQUEST", "请求类型或编号不正确");
    require(request.contains("data") && request["data"].is_object(),
            "INVALID_REQUEST", "请求数据必须是对象");
    const auto &data = request["data"];
    for (auto it = data.begin(); it != data.end(); ++it)
      require(fields.at(type).count(it.key()) != 0, "INVALID_REQUEST",
              "请求包含未知参数");
    const auto uid = context.userId;
    if (type != "register" && type != "login" && type != "ping")
      require(uid > 0, "UNAUTHENTICATED", "请先登录");
    Json output = Json::object();
    if (type == "ping") {
      output = {{"pong", true}};
    } else if (type == "register") {
      const auto name = field(data, "username", 32),
                 display = field(data, "display_name", 128),
                 password = field(data, "password", 128);
      const auto count =
          std::count_if(display.begin(), display.end(),
                        [](unsigned char c) { return (c & 0xc0) != 0x80; });
      require(usernameValid(name) && count > 0 && count <= 32 &&
                  display.find_first_not_of(" \t\r\n") != std::string::npos &&
                  password.size() >= 8,
              "INVALID_REQUEST", "账号、昵称或密码格式不正确");
      const auto hash = hashPassword(password);
      output = database_
                   .submit([=](sqlite3 *db) -> Json {
                     Transaction tx(db);
                     Statement old(db, "SELECT 1 FROM users WHERE username=?");
                     old.text(1, name);
                     require(!old.row(), "USERNAME_TAKEN", "用户名已被使用");
                     Statement q(db, "INSERT INTO "
                                     "users(username,display_name,salt,hash,"
                                     "iterations) VALUES(?,?,?,?,?)");
                     q.text(1, name);
                     q.text(2, display);
                     q.text(3, hash.saltHex);
                     q.text(4, hash.hashHex);
                     q.number(5, hash.iterations);
                     q.execute();
                     auto created = findUser(db, sqlite3_last_insert_rowid(db));
                     tx.commit();
                     return {{"user", created}};
                   })
                   .get();
    } else if (type == "login") {
      const auto name = field(data, "username", 32),
                 password = field(data, "password", 128);
      require(usernameValid(name) && password.size() >= 8, "AUTH_FAILED",
              "账号或密码错误");
      auto saved =
          database_
              .submit([=](sqlite3 *db) -> Json {
                Statement q(
                    db, "SELECT id,username,display_name,salt,hash,iterations "
                        "FROM users WHERE username=?");
                q.text(1, name);
                if (!q.row())
                  return nullptr;
                return {{"user", user(q)},
                        {"salt", q.text(3)},
                        {"hash", q.text(4)},
                        {"iterations", q.number(5)}};
              })
              .get();
      require(!saved.is_null() &&
                  verifyPassword(password, {saved.value("salt", ""),
                                            saved.value("hash", ""),
                                            saved.value("iterations", 600000)}),
              "AUTH_FAILED", "账号或密码错误");
      result.authenticatedUser = saved["user"]["id"].get<std::int64_t>();
      output = {{"user", saved["user"]}};
    } else if (type == "logout") {
      result.logout = true;
    } else if (type == "user_search") {
      const auto name = field(data, "username", 32);
      require(usernameValid(name), "INVALID_REQUEST", "用户名格式不正确");
      output = database_
                   .submit([=](sqlite3 *db) -> Json {
                     Statement q(db, "SELECT id,username,display_name FROM "
                                     "users WHERE username=?");
                     q.text(1, name);
                     require(q.row(), "NOT_FOUND", "未找到用户");
                     auto found = user(q);
                     return {{"user", found},
                             {"is_friend", friends(db, uid, found["id"])}};
                   })
                   .get();
    } else if (type == "friend_list") {
      output =
          database_
              .submit([=](sqlite3 *db) -> Json {
                Json list = Json::array(), pending = Json::array();
                Statement q(
                    db, "SELECT u.id,u.username,u.display_name FROM users u "
                        "JOIN friendships f ON u.id=CASE WHEN f.user_low=? "
                        "THEN f.user_high ELSE f.user_low END WHERE "
                        "f.user_low=? OR f.user_high=? ORDER BY u.id");
                q.number(1, uid);
                q.number(2, uid);
                q.number(3, uid);
                while (q.row())
                  list.push_back(user(q));
                Statement r(
                    db,
                    "SELECT r.id,u.id,u.username,u.display_name,r.created_at "
                    "FROM friend_requests r JOIN users u ON u.id=r.from_id "
                    "WHERE r.to_id=? AND r.status='pending' ORDER BY r.id");
                r.number(1, uid);
                while (r.row())
                  pending.push_back({{"id", r.number(0)},
                                     {"from", user(r, 1)},
                                     {"created_at", r.text(4)}});
                return {{"friends", list}, {"requests", pending}};
              })
              .get();
    } else if (type == "friend_request") {
      auto peer = integer(data, "peer_id");
      require(peer != uid, "INVALID_REQUEST", "不能添加自己");
      auto bundle =
          database_
              .submit([=](sqlite3 *db) -> Json {
                Transaction tx(db);
                findUser(db, peer);
                auto from = findUser(db, uid);
                require(!friends(db, uid, peer), "ALREADY_FRIENDS",
                        "已经是好友");
                Statement old(db, "SELECT id,from_id FROM friend_requests "
                                  "WHERE min(from_id,to_id)=? AND "
                                  "max(from_id,to_id)=? AND status='pending'");
                old.number(1, std::min(uid, peer));
                old.number(2, std::max(uid, peer));
                if (old.row()) {
                  Json r = {{"request_id", old.number(0)},
                            {"outgoing", old.number(1) == uid},
                            {"created", false}};
                  tx.commit();
                  return r;
                }
                Statement q(db,
                            "INSERT INTO friend_requests(from_id,to_id,status) "
                            "VALUES(?,?,'pending')");
                q.number(1, uid);
                q.number(2, peer);
                q.execute();
                auto id = sqlite3_last_insert_rowid(db);
                tx.commit();
                return {{"request_id", id},
                        {"outgoing", true},
                        {"created", true},
                        {"from", from}};
              })
              .get();
      output = {{"request_id", bundle["request_id"]},
                {"outgoing", bundle["outgoing"]}};
      if (bundle["created"] == true)
        result.pushes.push_back(
            {peer, event("friend_request", {{"id", bundle["request_id"]},
                                            {"from", bundle["from"]}})});
    } else if (type == "friend_respond") {
      auto id = integer(data, "request_id");
      require(data.contains("accept") && data["accept"].is_boolean(),
              "INVALID_REQUEST", "accept必须是布尔值");
      bool accept = data["accept"];
      auto bundle =
          database_
              .submit([=](sqlite3 *db) -> Json {
                Transaction tx(db);
                Statement q(db, "SELECT from_id,to_id,status FROM "
                                "friend_requests WHERE id=?");
                q.number(1, id);
                require(q.row(), "NOT_FOUND", "好友申请不存在");
                auto from = q.number(0);
                require(q.number(1) == uid, "FORBIDDEN",
                        "不能处理其他人的申请");
                const auto status = q.text(2);
                const std::string desired = accept ? "accepted" : "rejected";
                if (status != "pending") {
                  require(status == desired, "CONFLICT", "申请已被处理");
                  tx.commit();
                  return {{"changed", false},
                          {"peer_id", from},
                          {"status", status}};
                }
                Statement update(
                    db, "UPDATE friend_requests SET status=? WHERE id=?");
                update.text(1, desired);
                update.number(2, id);
                update.execute();
                if (accept) {
                  Statement insert(
                      db, "INSERT OR IGNORE INTO "
                          "friendships(user_low,user_high) VALUES(?,?)");
                  insert.number(1, std::min(uid, from));
                  insert.number(2, std::max(uid, from));
                  insert.execute();
                }
                tx.commit();
                return {
                    {"changed", true}, {"peer_id", from}, {"status", desired}};
              })
              .get();
      output = {{"status", bundle["status"]}};
      if (bundle["changed"] == true) {
        auto peer = bundle["peer_id"].get<std::int64_t>();
        result.pushes.push_back(
            {uid, event("friends_changed", {{"peer_id", peer}})});
        result.pushes.push_back(
            {peer, event("friends_changed", {{"peer_id", uid}})});
      }
    } else if (type == "friend_remove") {
      auto peer = integer(data, "peer_id");
      require(peer != uid, "INVALID_REQUEST", "不能删除自己");
      output =
          database_
              .submit([=](sqlite3 *db) -> Json {
                Transaction tx(db);
                findUser(db, peer);
                Statement q(
                    db,
                    "DELETE FROM friendships WHERE user_low=? AND user_high=?");
                q.number(1, std::min(uid, peer));
                q.number(2, std::max(uid, peer));
                q.execute();
                bool removed = sqlite3_changes(db) != 0;
                tx.commit();
                return {{"removed", removed}};
              })
              .get();
      if (output["removed"] == true) {
        result.pushes.push_back(
            {uid, event("friends_changed", {{"peer_id", peer}})});
        result.pushes.push_back(
            {peer, event("friends_changed", {{"peer_id", uid}})});
      }
    } else if (type == "message_send") {
      auto peer = integer(data, "peer_id");
      const auto clientId = field(data, "client_id", 36),
                 text = field(data, "text", 8192);
      require(peer != uid && uuidValid(clientId) && !text.empty(),
              "INVALID_REQUEST", "消息内容或编号不正确");
      auto bundle =
          database_
              .submit([=](sqlite3 *db) -> Json {
                Transaction tx(db);
                Statement old(
                    db, std::string("SELECT ") + columns +
                            " FROM messages WHERE sender_id=? AND client_id=?");
                old.number(1, uid);
                old.text(2, clientId);
                if (old.row()) {
                  auto saved = message(old);
                  require(saved["receiver_id"] == peer && saved["text"] == text,
                          "CONFLICT", "消息编号已用于不同内容");
                  tx.commit();
                  return {{"message", saved}, {"created", false}};
                }
                require(friends(db, uid, peer), "NOT_FRIENDS",
                        "仅支持向好友发送消息");
                Statement insert(db, "INSERT INTO "
                                     "messages(sender_id,receiver_id,client_id,"
                                     "body) VALUES(?,?,?,?)");
                insert.number(1, uid);
                insert.number(2, peer);
                insert.text(3, clientId);
                insert.text(4, text);
                insert.execute();
                Statement q(db, std::string("SELECT ") + columns +
                                    " FROM messages WHERE id=?");
                q.number(1, sqlite3_last_insert_rowid(db));
                q.row();
                auto saved = message(q);
                tx.commit();
                return {{"message", saved}, {"created", true}};
              })
              .get();
      output = {{"message", bundle["message"]},
                {"duplicate", bundle["created"] == false}};
      if (bundle["created"] == true)
        result.pushes.push_back(
            {peer, event("message", {{"message", bundle["message"]}})});
    } else if (type == "history") {
      const bool forward = data.contains("after_id");
      auto peer = integer(data, "peer_id"),
           before = data.contains("before_id")
                        ? integer(data, "before_id", true)
                        : 0,
           after =
               data.contains("after_id") ? integer(data, "after_id", true) : 0;
      auto limit = data.contains("limit")
                       ? std::min<std::int64_t>(100, integer(data, "limit"))
                       : 50;
      require(!(data.contains("before_id") && forward), "INVALID_REQUEST",
              "不能同时指定两个历史游标");
      output =
          database_
              .submit([=](sqlite3 *db) -> Json {
                auto found = findUser(db, peer);
                Statement q(
                    db,
                    std::string("SELECT ") + columns +
                        " FROM messages WHERE ((sender_id=? AND receiver_id=?) "
                        "OR (sender_id=? AND receiver_id=?)) AND id>? AND (?=0 "
                        "OR id<?) ORDER BY id " +
                        (forward ? "ASC" : "DESC") + " LIMIT ?");
                q.number(1, uid);
                q.number(2, peer);
                q.number(3, peer);
                q.number(4, uid);
                q.number(5, after);
                q.number(6, before);
                q.number(7, before);
                q.number(8, limit + 1);
                std::vector<Json> rows;
                std::size_t bytes = 0;
                bool more = false;
                while (q.row()) {
                  auto record = message(q);
                  const auto size = record.dump().size() + 1;
                  if (bytes + size > 56000) {
                    more = true;
                    break;
                  }
                  bytes += size;
                  rows.push_back(std::move(record));
                }
                more = more || rows.size() > static_cast<std::size_t>(limit);
                if (rows.size() > static_cast<std::size_t>(limit))
                  rows.pop_back();
                if (!forward)
                  std::reverse(rows.begin(), rows.end());
                return {{"messages", rows},
                        {"has_more", more},
                        {"peer", found},
                        {"is_friend", friends(db, uid, peer)}};
              })
              .get();
    } else if (type == "read") {
      auto peer = integer(data, "peer_id"),
           upTo = integer(data, "up_to_id", true);
      output = database_
                   .submit([=](sqlite3 *db) -> Json {
                     Statement q(db, "UPDATE messages SET is_read=1 WHERE "
                                     "receiver_id=? AND sender_id=? AND id<=?");
                     q.number(1, uid);
                     q.number(2, peer);
                     q.number(3, upTo);
                     q.execute();
                     return {{"updated", sqlite3_changes(db)}};
                   })
                   .get();
    } else if (type == "conversations") {
      const auto before =
          data.contains("before_id") ? integer(data, "before_id", true) : 0;
      const auto limit =
          data.contains("limit")
              ? std::min<std::int64_t>(100, integer(data, "limit"))
              : 50;
      output =
          database_
              .submit([=](sqlite3 *db) -> Json {
                Statement peers(
                    db,
                    "SELECT u.id,u.username,u.display_name FROM users u JOIN "
                    "(SELECT peer,max(id) last_id FROM "
                    "(SELECT receiver_id peer,id FROM messages WHERE "
                    "sender_id=? "
                    "UNION ALL SELECT sender_id peer,id FROM messages WHERE "
                    "receiver_id=?) "
                    "GROUP BY peer) latest ON latest.peer=u.id "
                    "WHERE (?=0 OR latest.last_id<?) ORDER BY latest.last_id "
                    "DESC LIMIT ?");
                peers.number(1, uid);
                peers.number(2, uid);
                peers.number(3, before);
                peers.number(4, before);
                peers.number(5, limit + 1);
                Json list = Json::array();
                std::size_t bytes = 0;
                bool more = false;
                while (peers.row()) {
                  auto peer = peers.number(0);
                  Statement last(db,
                                 std::string("SELECT ") + columns +
                                     " FROM messages WHERE (sender_id=? AND "
                                     "receiver_id=?) OR (sender_id=? AND "
                                     "receiver_id=?) ORDER BY id DESC LIMIT 1");
                  last.number(1, uid);
                  last.number(2, peer);
                  last.number(3, peer);
                  last.number(4, uid);
                  if (!last.row())
                    continue;
                  Statement unread(
                      db, "SELECT count(*) FROM messages WHERE receiver_id=? "
                          "AND sender_id=? AND is_read=0");
                  unread.number(1, uid);
                  unread.number(2, peer);
                  unread.row();
                  auto saved = message(last);
                  auto text = saved["text"].get<std::string>();
                  const bool truncated = text.size() > 256;
                  if (truncated) {
                    std::size_t end = 256;
                    while ((static_cast<unsigned char>(text[end]) & 0xc0) ==
                           0x80)
                      --end;
                    text.resize(end);
                  }
                  saved["text"] = text;
                  saved["text_truncated"] = truncated;
                  Json item = {{"peer", user(peers)},
                               {"last_message", std::move(saved)},
                               {"unread", unread.number(0)},
                               {"is_friend", friends(db, uid, peer)}};
                  auto size = item.dump().size() + 1;
                  if (list.size() >= static_cast<std::size_t>(limit) ||
                      bytes + size > 56000) {
                    more = true;
                    break;
                  }
                  bytes += size;
                  list.push_back(std::move(item));
                }
                const auto next =
                    list.empty()
                        ? std::int64_t{0}
                        : list.back()["last_message"]["id"].get<std::int64_t>();
                return {{"conversations", list},
                        {"has_more", more},
                        {"next_before_id", next}};
              })
              .get();
    }
    result.reply = reply(request, std::move(output));
  } catch (const RequestError &error) {
    result.reply = failure(request, error.code, error.what());
  } catch (const Json::exception &) {
    result.reply = failure(request, "INVALID_REQUEST", "参数格式不正确");
  } catch (const std::invalid_argument &) {
    result.reply = failure(request, "INVALID_REQUEST", "参数格式不正确");
  } catch (const std::exception &error) {
    std::cerr << "QtChat business operation failed: " << error.what() << '\n';
    result.reply =
        failure(request, "SERVER_ERROR", "服务暂时不可用，请稍后重试");
  }
  return result;
}
} // namespace qtchat
