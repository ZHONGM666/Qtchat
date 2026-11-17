#include "check.h"
#include "server/service.h"
#include <iomanip>
#include <set>
#include <sqlite3.h>
#include <sstream>
using namespace qtchat;
using Json = nlohmann::json;
struct Fixture {
  Database database{":memory:"};
  Service service{database};
  int sequence = 0;
  ServiceResult call(std::int64_t user, const std::string &type,
                     Json data = Json::object()) {
    return service.execute({1, user},
                           {{"version", 1},
                            {"type", type},
                            {"request_id", std::to_string(++sequence)},
                            {"data", std::move(data)}});
  }
  std::int64_t add(const std::string &name) {
    auto result = call(0, "register",
                       {{"username", name},
                        {"display_name", name},
                        {"password", "Secret-123"}});
    check(result.reply.value("ok", false), "register failed");
    return result.reply["data"]["user"]["id"].get<std::int64_t>();
  }
  void connect(std::int64_t a, std::int64_t b) {
    auto request = call(a, "friend_request", {{"peer_id", b}});
    check(request.reply.value("ok", false), "friend request failed");
    check(call(b, "friend_respond",
               {{"request_id", request.reply["data"]["request_id"]},
                {"accept", true}})
                  .reply["ok"] == true,
          "friend acceptance failed");
  }
};
static std::string uuid(int number) {
  std::ostringstream out;
  out << "00000000-0000-4000-8000-" << std::setw(12) << std::setfill('0')
      << number;
  return out.str();
}
static void ok(const ServiceResult &result) {
  check(result.reply.value("ok", false), "operation rejected");
}
static void code(const ServiceResult &result, const char *error) {
  check(result.reply.value("ok", true) == false, "invalid operation accepted");
  check(result.reply["error"]["code"] == error, "wrong error code");
}

int main() {
  Checks tests;
  tests.run("registration and authentication validate real boundaries", [] {
    Fixture f;
    auto a = f.add("abc");
    auto b = f.add(std::string(32, 'x'));
    check(a != b, "duplicate account ids");
    for (const auto &username :
         std::vector<std::string>{"ab", std::string(33, 'x'), u8"用户"})
      code(f.call(0, "register",
                  {{"username", username},
                   {"display_name", "Name"},
                   {"password", "Secret-123"}}),
           "INVALID_REQUEST");
    code(f.call(0, "register",
                {{"username", "abc"},
                 {"display_name", "Name"},
                 {"password", "Secret-123"}}),
         "USERNAME_TAKEN");
    std::string longName;
    for (int i = 0; i < 33; ++i)
      longName += u8"中";
    code(f.call(0, "register",
                {{"username", "newname"},
                 {"display_name", longName},
                 {"password", "Secret-123"}}),
         "INVALID_REQUEST");
    auto login =
        f.call(0, "login", {{"username", "abc"}, {"password", "Secret-123"}});
    ok(login);
    check(login.authenticatedUser == a, "login identity missing");
    check(!login.reply["data"]["user"].contains("hash"),
          "password hash leaked");
    code(f.call(0, "login", {{"username", "abc"}, {"password", "Wrong-123"}}),
         "AUTH_FAILED");
    code(f.call(0, "login",
                {{"username", "missing"}, {"password", "Wrong-123"}}),
         "AUTH_FAILED");
    code(f.call(0, "friend_list"), "UNAUTHENTICATED");
    auto logout = f.call(a, "logout");
    ok(logout);
    check(logout.logout, "logout must clear reactor identity");
    code(f.call(0, "conversations"), "UNAUTHENTICATED");
  });
  tests.run("friend requests are authorized and idempotent", [] {
    Fixture f;
    auto a = f.add("alice"), b = f.add("bobby"), c = f.add("carol");
    code(f.call(a, "friend_request", {{"peer_id", a}}), "INVALID_REQUEST");
    auto request = f.call(a, "friend_request", {{"peer_id", b}});
    ok(request);
    check(request.pushes.size() == 1, "incoming notification missing");
    const auto id = request.reply["data"]["request_id"];
    auto duplicate = f.call(a, "friend_request", {{"peer_id", b}});
    ok(duplicate);
    check(duplicate.reply["data"]["request_id"] == id &&
              duplicate.pushes.empty(),
          "pending request duplicated");
    auto crossed = f.call(b, "friend_request", {{"peer_id", a}});
    ok(crossed);
    check(crossed.reply["data"]["request_id"] == id,
          "crossed request duplicated");
    code(f.call(c, "friend_respond", {{"request_id", id}, {"accept", true}}),
         "FORBIDDEN");
    ok(f.call(b, "friend_respond", {{"request_id", id}, {"accept", true}}));
    check(f.call(b, "friend_respond", {{"request_id", id}, {"accept", true}})
              .pushes.empty(),
          "repeated accept notified twice");
    check(f.service.friendIds(a) == std::vector<std::int64_t>{b},
          "friendship not created");
    check(f.service.friendIds(b) == std::vector<std::int64_t>{a},
          "friendship is not bidirectional");
    auto other = f.call(c, "friend_request", {{"peer_id", a}});
    ok(other);
    ok(f.call(a, "friend_respond",
              {{"request_id", other.reply["data"]["request_id"]},
               {"accept", false}}));
    check(f.service.friendIds(c).empty(), "rejected request became friendship");
  });
  tests.run("messages are committed once and history is authorized", [] {
    Fixture f;
    auto a = f.add("alice"), b = f.add("bobby"), c = f.add("carol");
    code(f.call(a, "message_send",
                {{"peer_id", b}, {"client_id", uuid(1)}, {"text", "hello"}}),
         "NOT_FRIENDS");
    f.connect(a, b);
    Json data = {{"peer_id", b},
                 {"client_id", uuid(1)},
                 {"text", std::string(8192, 'x')}};
    auto first = f.call(a, "message_send", data);
    ok(first);
    check(first.pushes.size() == 1, "online message event missing");
    auto duplicate = f.call(a, "message_send", data);
    ok(duplicate);
    check(duplicate.reply["data"]["message"]["id"] ==
                  first.reply["data"]["message"]["id"] &&
              duplicate.pushes.empty(),
          "message deduplication failed");
    data["text"] = "changed";
    code(f.call(a, "message_send", data), "CONFLICT");
    data["client_id"] = uuid(2);
    data["text"] = std::string(8193, 'x');
    code(f.call(a, "message_send", data), "INVALID_REQUEST");
    data["text"] = "spoof";
    data["sender_id"] = c;
    code(f.call(a, "message_send", data), "INVALID_REQUEST");
    for (int i = 2; i <= 103; ++i)
      ok(f.call(a, "message_send",
                {{"peer_id", b}, {"client_id", uuid(i)}, {"text", u8"你好"}}));
    auto history = f.call(b, "history", {{"peer_id", a}, {"limit", 1000}});
    ok(history);
    check(history.reply["data"]["messages"].size() == 100 &&
              history.reply["data"]["has_more"] == true,
          "history limit/pagination wrong");
    auto next =
        f.call(b, "history",
               {{"peer_id", a},
                {"before_id", history.reply["data"]["messages"][0]["id"]},
                {"limit", 100}});
    ok(next);
    check(next.reply["data"]["messages"].size() == 3,
          "history cursor lost or repeated records");
    auto forbidden = f.call(c, "history", {{"peer_id", b}});
    ok(forbidden);
    check(forbidden.reply["data"]["messages"].empty(),
          "other users' conversation leaked");
    ok(f.call(
        b, "read",
        {{"peer_id", a}, {"up_to_id", first.reply["data"]["message"]["id"]}}));
    auto summary = f.call(b, "conversations");
    ok(summary);
    check(summary.reply["data"]["conversations"][0]["unread"] == 102,
          "read marked later messages prematurely");
    auto count = f.database
                     .submit([](sqlite3 *db) -> Json {
                       sqlite3_stmt *statement = nullptr;
                       sqlite3_prepare_v2(db, "SELECT count(*) FROM messages",
                                          -1, &statement, nullptr);
                       sqlite3_step(statement);
                       int n = sqlite3_column_int(statement, 0);
                       sqlite3_finalize(statement);
                       return n;
                     })
                     .get();
    check(count == 103, "database persisted duplicate messages");
    ok(f.call(a, "friend_remove", {{"peer_id", b}}));
    check(f.service.friendIds(a).empty() && f.service.friendIds(b).empty(),
          "deletion not bidirectional");
    code(
        f.call(a, "message_send",
               {{"peer_id", b}, {"client_id", uuid(104)}, {"text", "blocked"}}),
        "NOT_FRIENDS");
    check(!f.call(a, "history", {{"peer_id", b}})
               .reply["data"]["messages"]
               .empty(),
          "friend deletion erased history");
    ok(f.call(a, "friend_remove", {{"peer_id", b}}));
  });
  tests.run("history pages also respect the wire byte limit", [] {
    Fixture f;
    auto a = f.add("alice"), b = f.add("bobby");
    f.connect(a, b);
    for (int i = 1; i <= 12; ++i)
      ok(f.call(a, "message_send",
                {{"peer_id", b},
                 {"client_id", uuid(i)},
                 {"text", std::string(8192, 'x')}}));
    auto page = f.call(b, "history", {{"peer_id", a}});
    ok(page);
    check(page.reply.dump().size() <= 65536,
          "history cannot fit a legal frame");
    check(page.reply["data"]["has_more"] == true,
          "byte-limited page lost continuation");
    auto next =
        f.call(b, "history",
               {{"peer_id", a},
                {"before_id", page.reply["data"]["messages"][0]["id"]}});
    ok(next);
    check(page.reply["data"]["messages"].size() +
                  next.reply["data"]["messages"].size() ==
              12,
          "byte pagination lost messages");
  });
  tests.run("forward history from zero starts at the oldest record", [] {
    Fixture f;
    auto a = f.add("alice"), b = f.add("bobby");
    f.connect(a, b);
    for (int i = 1; i <= 60; ++i)
      ok(f.call(a, "message_send",
                {{"peer_id", b}, {"client_id", uuid(i)}, {"text", "hello"}}));
    auto page = f.call(a, "history", {{"peer_id", b}, {"after_id", 0}});
    ok(page);
    check(page.reply["data"]["messages"][0]["id"] == 1,
          "zero forward cursor skipped earliest records");
  });
  tests.run(
      "conversation summaries have bounded UTF8 previews and complete "
      "pagination",
      [] {
        Fixture f;
        const auto a = f.add("alice");
        f.database
            .submit([a](sqlite3 *db) -> Json {
              for (int i = 0; i < 160; ++i) {
                std::string sql =
                    "INSERT INTO "
                    "users(username,display_name,salt,hash,iterations) SELECT "
                    "'peer" +
                    std::to_string(i) +
                    "','Peer',salt,hash,iterations FROM users WHERE id=" +
                    std::to_string(a);
                check(sqlite3_exec(db, sql.c_str(), nullptr, nullptr,
                                   nullptr) == SQLITE_OK,
                      "seed user failed");
                auto peer = sqlite3_last_insert_rowid(db);
                std::string body;
                for (int n = 0; n < 2730; ++n)
                  body += u8"中";
                sqlite3_stmt *s = nullptr;
                check(sqlite3_prepare_v2(db,
                                         "INSERT INTO "
                                         "messages(sender_id,receiver_id,"
                                         "client_id,body) VALUES(?,?,?,?)",
                                         -1, &s, nullptr) == SQLITE_OK,
                      "seed prepare failed");
                sqlite3_bind_int64(s, 1, a);
                sqlite3_bind_int64(s, 2, peer);
                auto id = uuid(i);
                sqlite3_bind_text(s, 3, id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 4, body.c_str(), -1, SQLITE_TRANSIENT);
                auto rc = sqlite3_step(s);
                sqlite3_finalize(s);
                check(rc == SQLITE_DONE, "seed message failed");
              }
              return {};
            })
            .get();
        std::set<std::int64_t> seen;
        std::int64_t before = 0;
        for (int i = 0; i < 10; ++i) {
          auto page = f.call(a, "conversations",
                             {{"before_id", before}, {"limit", 100}});
          ok(page);
          check(page.reply.dump().size() <= 65536,
                "summary exceeded frame limit");
          auto data = page.reply["data"];
          for (const auto &row : data["conversations"]) {
            check(seen.insert(row["peer"]["id"].get<std::int64_t>()).second,
                  "summary peer repeated");
            auto preview = row["last_message"]["text"].get<std::string>();
            check(preview.size() <= 256 && preview.size() % 3 == 0,
                  "preview is too long or splits UTF8");
          }
          if (!data.value("has_more", false))
            break;
          auto next = data["next_before_id"].get<std::int64_t>();
          check(next > 0 && (before == 0 || next < before),
                "summary cursor did not advance");
          before = next;
        }
        check(seen.size() == 160, "summary pagination omitted peers");
      });
  return tests.result();
}
