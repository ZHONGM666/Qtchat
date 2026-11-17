#include "database.h"
#include "BS_thread_pool.hpp"
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
namespace qtchat {
namespace {
void exec(sqlite3 *db, const char *sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}
std::string scalar(sqlite3 *db, const char *sql) {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  std::string result;
  if (sqlite3_step(statement) == SQLITE_ROW) {
    const auto text = sqlite3_column_text(statement, 0);
    if (text)
      result = reinterpret_cast<const char *>(text);
  }
  sqlite3_finalize(statement);
  return result;
}
constexpr const char *schema = R"SQL(
BEGIN;
CREATE TABLE IF NOT EXISTS users(
 id INTEGER PRIMARY KEY,username TEXT NOT NULL UNIQUE,display_name TEXT NOT NULL,
 salt TEXT NOT NULL,hash TEXT NOT NULL,iterations INTEGER NOT NULL,
 created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')));
CREATE TABLE IF NOT EXISTS friend_requests(
 id INTEGER PRIMARY KEY,from_id INTEGER NOT NULL REFERENCES users(id),to_id INTEGER NOT NULL REFERENCES users(id),
 status TEXT NOT NULL CHECK(status IN('pending','accepted','rejected')),
 created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),CHECK(from_id<>to_id));
CREATE UNIQUE INDEX IF NOT EXISTS pending_friend_pair ON friend_requests(min(from_id,to_id),max(from_id,to_id)) WHERE status='pending';
CREATE INDEX IF NOT EXISTS incoming_requests ON friend_requests(to_id,status,id);
CREATE TABLE IF NOT EXISTS friendships(
 user_low INTEGER NOT NULL REFERENCES users(id),user_high INTEGER NOT NULL REFERENCES users(id),
 created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
 PRIMARY KEY(user_low,user_high),CHECK(user_low<user_high));
CREATE TABLE IF NOT EXISTS messages(
 id INTEGER PRIMARY KEY AUTOINCREMENT,sender_id INTEGER NOT NULL REFERENCES users(id),receiver_id INTEGER NOT NULL REFERENCES users(id),
 client_id TEXT NOT NULL,body TEXT NOT NULL CHECK(length(CAST(body AS BLOB)) BETWEEN 1 AND 8192),
 created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
 is_read INTEGER NOT NULL DEFAULT 0 CHECK(is_read IN(0,1)),UNIQUE(sender_id,client_id));
CREATE INDEX IF NOT EXISTS conversation_messages ON messages(sender_id,receiver_id,id);
CREATE INDEX IF NOT EXISTS unread_messages ON messages(receiver_id,is_read,sender_id,id);
PRAGMA user_version=1;
COMMIT;
)SQL";
} // namespace
struct Database::Impl {
  BS::thread_pool<> worker{1};
  sqlite3 *connection = nullptr;
  explicit Impl(const std::string &path) {
    worker
        .submit_task([this, path] {
          if (path != ":memory:" &&
              !std::filesystem::path(path).parent_path().empty())
            std::filesystem::create_directories(
                std::filesystem::path(path).parent_path());
          if (sqlite3_open_v2(path.c_str(), &connection,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                  SQLITE_OPEN_NOMUTEX,
                              nullptr) != SQLITE_OK) {
            if (connection)
              sqlite3_close_v2(connection);
            connection = nullptr;
            throw std::runtime_error("cannot open database");
          }
          try {
            sqlite3_busy_timeout(connection, 5000);
            exec(connection,
                 "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
            if (path != ":memory:") {
              const bool shared = path.rfind("/mnt/hgfs/", 0) == 0;
              const auto journal =
                  scalar(connection, shared ? "PRAGMA journal_mode=DELETE"
                                            : "PRAGMA journal_mode=WAL");
              if (journal != (shared ? "delete" : "wal"))
                throw std::runtime_error(
                    "requested database journal mode unavailable");
              std::cerr << "SQLite journal_mode=" << journal << '\n';
            }
            const auto version = scalar(connection, "PRAGMA user_version");
            if (version != "0" && version != "1")
              throw std::runtime_error("unsupported database schema version");
            exec(connection, schema);
          } catch (...) {
            sqlite3_close_v2(connection);
            connection = nullptr;
            throw;
          }
        })
        .get();
  }
  ~Impl() {
    worker.wait();
    if (connection)
      worker
          .submit_task([this] {
            sqlite3_close_v2(connection);
            connection = nullptr;
          })
          .get();
  }
};
Database::Database(std::string path) : impl_(std::make_unique<Impl>(path)) {}
Database::~Database() = default;
std::future<nlohmann::json>
Database::submit(std::function<nlohmann::json(sqlite3 *)> task) {
  return impl_->worker.submit_task([this, task = std::move(task)] {
    try {
      auto result = task(impl_->connection);
      if (!sqlite3_get_autocommit(impl_->connection))
        throw std::runtime_error(
            "database task left an uncommitted transaction");
      return result;
    } catch (...) {
      if (!sqlite3_get_autocommit(impl_->connection))
        sqlite3_exec(impl_->connection, "ROLLBACK", nullptr, nullptr, nullptr);
      throw;
    }
  });
}
void Database::drain() { impl_->worker.wait(); }
} // namespace qtchat
