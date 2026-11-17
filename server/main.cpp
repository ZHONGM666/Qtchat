#include "reactor.h"
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace {
qtchat::Reactor *running = nullptr;
void stop(int) {
  if (running)
    running->requestStop();
}
std::size_t number(const std::string &text, std::size_t minimum,
                   std::size_t maximum) {
  std::size_t used = 0;
  if (text.empty() || text[0] == '-')
    throw std::invalid_argument("invalid numeric option");
  auto n = std::stoull(text, &used);
  if (used != text.size() || n < minimum || n > maximum)
    throw std::invalid_argument("numeric option out of range");
  return n;
}
} // namespace
int main(int argc, char **argv) {
  try {
    qtchat::ServerOptions options;
    const char *data = std::getenv("XDG_DATA_HOME");
    const char *home = std::getenv("HOME");
    options.databasePath =
        (data && *data
             ? std::filesystem::path(data)
             : std::filesystem::path(home ? home : ".") / ".local/share") /
        "qtchat/qtchat.db";
    for (int i = 1; i < argc; ++i) {
      std::string key = argv[i];
      if (key == "--help") {
        std::cout << "qtchat_server --host 127.0.0.1 --port 9527 --db PATH "
                     "--workers 2 --heartbeat-timeout 60\n";
        return 0;
      }
      if (i + 1 == argc)
        throw std::invalid_argument("missing option value");
      std::string value = argv[++i];
      if (key == "--host")
        options.host = value;
      else if (key == "--db")
        options.databasePath = value;
      else if (key == "--port")
        options.port = number(value, 0, 65535);
      else if (key == "--workers")
        options.workers = number(value, 1, 64);
      else if (key == "--heartbeat-timeout")
        options.heartbeatTimeout = number(value, 1, 3600);
      else if (key == "--auth-timeout")
        options.authTimeout = number(value, 1, 3600);
      else if (key == "--max-connections")
        options.maxConnections = number(value, 1, 100000);
      else if (key == "--max-pending")
        options.maxPending = number(value, 1, 1024);
      else if (key == "--max-inflight")
        options.maxInFlight = number(value, 1, 65536);
      else if (key == "--max-output")
        options.maxOutput = number(value, 256, 16 * 1024 * 1024);
      else
        throw std::invalid_argument("unknown option: " + key);
    }
    qtchat::Reactor reactor(options);
    running = &reactor;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    reactor.run();
    running = nullptr;
  } catch (const std::exception &error) {
    running = nullptr;
    std::cerr << "QtChat server: " << error.what() << '\n';
    return 1;
  }
}
