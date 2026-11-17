#pragma once
#include <functional>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
struct sqlite3;
namespace qtchat {
class Database {
public:
  explicit Database(std::string path);
  ~Database();
  std::future<nlohmann::json>
  submit(std::function<nlohmann::json(sqlite3 *)> task);
  void drain();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace qtchat
