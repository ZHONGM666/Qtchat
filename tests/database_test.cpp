#include "check.h"
#include "server/database.h"
#include "server/password.h"
#include <chrono>
#include <filesystem>
#include <sqlite3.h>
#include <sstream>
#include <thread>
using namespace qtchat;
using Json = nlohmann::json;

static void sql(sqlite3 *db, const char *text) {
  if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}
static std::string threadName() {
  std::ostringstream out;
  out << std::this_thread::get_id();
  return out.str();
}

int main() {
  Checks tests;
  tests.run("salted password accepts only the matching secret", [] {
    auto first = hashPassword("Correct-Secret-123");
    auto second = hashPassword("Correct-Secret-123");
    check(first.saltHex.size() == 32 && first.hashHex.size() == 64,
          "derived password format wrong");
    check(first.saltHex != second.saltHex, "password salts reused");
    check(verifyPassword("Correct-Secret-123", first),
          "correct password rejected");
    check(!verifyPassword("Incorrect-Secret", first),
          "incorrect password accepted");
    auto malformed = first;
    malformed.saltHex = "xx";
    check(!verifyPassword("Correct-Secret-123", malformed),
          "malformed stored hash accepted");
  });
  tests.run("password boundaries are enforced without truncation", [] {
    throws<std::invalid_argument>([] { hashPassword(""); });
    throws<std::invalid_argument>([] { hashPassword("1234567"); });
    throws<std::invalid_argument>([] { hashPassword(std::string(129, 'a')); });
    auto limit = hashPassword(std::string(128, 'a'));
    check(verifyPassword(std::string(128, 'a'), limit),
          "maximum password rejected");
  });
  tests.run("database work uses one dedicated worker", [] {
    Database db(":memory:");
    const auto caller = threadName();
    std::vector<std::future<Json>> results;
    for (int i = 0; i < 16; ++i)
      results.push_back(
          db.submit([](sqlite3 *) -> Json { return threadName(); }));
    const auto worker = results.front().get().get<std::string>();
    check(worker != caller, "database ran in caller thread");
    for (std::size_t i = 1; i < results.size(); ++i)
      check(results[i].get() == worker, "database used multiple workers");
  });
  tests.run("schema and committed changes survive reopening", [] {
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("qtchat-test-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          ".db"))
            .string();
    {
      Database db(path);
      auto saved = db.submit([](sqlite3 *conn) -> Json {
                       sql(conn, "CREATE TABLE proof(value INTEGER); INSERT "
                                 "INTO proof VALUES(42)");
                       return true;
                     }).get();
      check(saved == true, "database task was not executed");
    }
    {
      Database db(path);
      check(db.submit([](sqlite3 *conn) -> Json {
                sqlite3_stmt *statement = nullptr;
                if (sqlite3_prepare_v2(conn, "SELECT value FROM proof", -1,
                                       &statement, nullptr) != SQLITE_OK)
                  throw std::runtime_error("prepare failed");
                const int result = sqlite3_step(statement) == SQLITE_ROW
                                       ? sqlite3_column_int(statement, 0)
                                       : -1;
                sqlite3_finalize(statement);
                return result;
              }).get() == 42,
            "committed value did not survive reopen");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
  });
  tests.run("task failure rolls back and does not kill database executor", [] {
    Database db(":memory:");
    throws<std::runtime_error>([&] {
      db.submit([](sqlite3 *conn) -> Json {
          sql(conn, "BEGIN; INSERT INTO "
                    "users(username,display_name,salt,hash,iterations) "
                    "VALUES('failed','Failed','salt','hash',600000)");
          throw std::runtime_error("simulated task failure");
        }).get();
    });
    check(db.submit([](sqlite3 *conn) -> Json {
              sqlite3_stmt *statement = nullptr;
              sqlite3_prepare_v2(conn, "SELECT count(*) FROM users", -1,
                                 &statement, nullptr);
              sqlite3_step(statement);
              const auto count = sqlite3_column_int(statement, 0);
              sqlite3_finalize(statement);
              return count;
            }).get() == 0,
          "failed task left partial transaction");
  });
  return tests.result();
}
