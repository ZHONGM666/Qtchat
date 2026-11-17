#pragma once
#include <cstddef>
#include <memory>
#include <string>
namespace qtchat {
struct ServerOptions {
  std::string host = "127.0.0.1", databasePath;
  unsigned port = 9527, workers = 2, heartbeatTimeout = 60, authTimeout = 30;
  std::size_t maxConnections = 4096, maxPending = 32, maxInFlight = 1024,
              maxOutput = 1024 * 1024;
};
class Reactor {
public:
  explicit Reactor(ServerOptions options);
  ~Reactor();
  void run();
  void requestStop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace qtchat
