#include "reactor.h"
#include "BS_thread_pool.hpp"
#include "common/frame.h"
#include "common/idle_timeout.h"
#include "service.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace qtchat {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
Json errorReply(const Json &request, const char *code, const char *message) {
  return {{"version", 1},
          {"type", request.value("type", std::string())},
          {"request_id", request.value("request_id", std::string())},
          {"ok", false},
          {"error", {{"code", code}, {"message", message}}}};
}
Json event(const char *type, Json data) {
  return {{"version", 1},
          {"event", true},
          {"type", type},
          {"data", std::move(data)}};
}
} // namespace
struct Reactor::Impl {
  struct Connection {
    int fd = -1;
    std::uint64_t id = 0, epoch = 0;
    std::int64_t user = 0;
    FrameDecoder decoder;
    std::deque<Json> requests;
    std::string output;
    std::size_t offset = 0;
    bool busy = false, readClosed = false, afterFlushClose = false;
    IdleTimeout::Handle timeout;
    Clock::time_point created = Clock::now();
    std::unordered_set<std::int64_t> peers;
  };
  struct Completion {
    std::uint64_t id, epoch;
    ServiceResult result;
    std::vector<std::int64_t> peers;
  };
  ServerOptions options;
  Database database;
  Service service;
  BS::thread_pool<> workers;
  IdleTimeout idle;
  int listener = -1, poller = -1, wakeup = -1;
  std::atomic<bool> stop{false};
  std::uint64_t nextId = 3;
  std::size_t totalRequests = 0;
  std::unordered_map<std::uint64_t, std::unique_ptr<Connection>> connections;
  std::unordered_map<std::int64_t, std::uint64_t> online;
  std::mutex completionMutex;
  std::deque<Completion> completed;
  std::unordered_set<std::uint64_t> doomed;
  std::vector<std::uint64_t> closeQueue;

  explicit Impl(ServerOptions value)
      : options(std::move(value)), database(options.databasePath),
        service(database), workers(options.workers),
        idle(options.heartbeatTimeout) {
    try {
      poller = epoll_create1(EPOLL_CLOEXEC);
      wakeup = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (poller < 0 || wakeup < 0 || listener < 0)
        throw std::runtime_error("cannot create network descriptors");
      int yes = 1;
      setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(static_cast<std::uint16_t>(options.port));
      if (inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("host must be an IPv4 address");
      if (bind(listener, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) < 0 ||
          listen(listener, 512) < 0)
        throw std::runtime_error("cannot bind or listen");
      epoll_event ev{};
      ev.events = EPOLLIN;
      ev.data.u64 = 1;
      if (epoll_ctl(poller, EPOLL_CTL_ADD, listener, &ev) < 0)
        throw std::runtime_error("cannot register listener");
      ev.data.u64 = 2;
      if (epoll_ctl(poller, EPOLL_CTL_ADD, wakeup, &ev) < 0)
        throw std::runtime_error("cannot register eventfd");
    } catch (...) {
      closeDescriptors();
      throw;
    }
  }
  ~Impl() {
    workers.wait();
    for (auto &item : connections) {
      idle.cancel(item.second->timeout);
      ::close(item.second->fd);
    }
    connections.clear();
    closeDescriptors();
  }
  void closeDescriptors() {
    for (int fd : {listener, wakeup, poller})
      if (fd >= 0)
        ::close(fd);
    listener = wakeup = poller = -1;
  }
  bool alive(std::uint64_t id) const {
    return connections.count(id) && !doomed.count(id);
  }
  void deferClose(std::uint64_t id) {
    if (connections.count(id) && doomed.insert(id).second)
      closeQueue.push_back(id);
  }
  void interest(Connection &c) {
    epoll_event ev{};
    ev.data.u64 = c.id;
    if (!c.readClosed && !c.afterFlushClose)
      ev.events = EPOLLIN | EPOLLRDHUP;
    if (c.offset < c.output.size())
      ev.events |= EPOLLOUT;
    if (epoll_ctl(poller, EPOLL_CTL_MOD, c.fd, &ev) < 0)
      deferClose(c.id);
  }
  bool queue(std::uint64_t id, Json response) {
    if (!alive(id))
      return false;
    auto &c = *connections.at(id);
    std::string payload;
    try {
      payload = response.dump();
      if (payload.size() > MaxFrameSize ||
          payload.size() + 4 > options.maxOutput)
        payload = errorReply(response, "RESPONSE_TOO_LARGE",
                             "响应过大，请缩小查询范围")
                      .dump();
    } catch (...) {
      deferClose(id);
      return false;
    }
    auto bytes = encodeFrame(payload);
    if (c.output.size() - c.offset + bytes.size() > options.maxOutput) {
      deferClose(id);
      return false;
    }
    if (c.offset) {
      c.output.erase(0, c.offset);
      c.offset = 0;
    }
    c.output += bytes;
    interest(c);
    return true;
  }
  void announce(std::int64_t user,
                const std::unordered_set<std::int64_t> &peers) {
    auto notice = event(
        "presence", {{"user_id", user}, {"online", online.count(user) != 0}});
    for (auto peer : peers) {
      auto found = online.find(peer);
      if (found != online.end() && alive(found->second) &&
          connections.at(found->second)->peers.count(user))
        queue(found->second, notice);
    }
  }
  void drainCloses() {
    for (std::size_t index = 0; index < closeQueue.size(); ++index) {
      auto id = closeQueue[index];
      auto it = connections.find(id);
      if (it == connections.end())
        continue;
      auto &c = *it->second;
      auto uid = c.user;
      auto peers = c.peers;
      totalRequests -= c.requests.size();
      idle.cancel(c.timeout);
      epoll_ctl(poller, EPOLL_CTL_DEL, c.fd, nullptr);
      ::close(c.fd);
      connections.erase(it);
      if (uid && online.count(uid) && online.at(uid) == id) {
        online.erase(uid);
        announce(uid, peers);
      }
    }
    closeQueue.clear();
    doomed.clear();
  }
  void acceptReady() {
    for (unsigned count = 0; count < 64; ++count) {
      int fd =
          accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) {
        if (errno == EINTR) {
          --count;
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return;
        return;
      }
      if (connections.size() >= options.maxConnections) {
        ::close(fd);
        continue;
      }
      int yes = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
      auto c = std::make_unique<Connection>();
      c->id = nextId++;
      c->fd = fd;
      auto id = c->id;
      c->timeout = idle.watch([this, id] { deferClose(id); });
      epoll_event ev{};
      ev.events = EPOLLIN | EPOLLRDHUP;
      ev.data.u64 = id;
      if (epoll_ctl(poller, EPOLL_CTL_ADD, fd, &ev) < 0) {
        idle.cancel(c->timeout);
        ::close(fd);
        continue;
      }
      connections.emplace(id, std::move(c));
    }
  }
  void post(Completion completion) {
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      completed.push_back(std::move(completion));
    }
    std::uint64_t one = 1;
    while (::write(wakeup, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
  }
  void startNext(std::uint64_t id) {
    if (!alive(id))
      return;
    auto &c = *connections.at(id);
    if (c.busy || c.requests.empty() || c.afterFlushClose)
      return;
    auto request = std::move(c.requests.front());
    c.requests.pop_front();
    c.busy = true;
    auto epoch = c.epoch;
    RequestContext context{id, c.user};
    workers.detach_task(
        [this, id, epoch, context, request = std::move(request)] {
          Completion done{id, epoch, {}, {}};
          try {
            done.result = service.execute(context, request);
            if (done.result.authenticatedUser)
              done.peers = service.friendIds(*done.result.authenticatedUser);
          } catch (...) {
            done.result = {};
            done.result.reply =
                errorReply(request, "SERVER_ERROR", "服务暂时不可用");
          }
          post(std::move(done));
        });
  }
  void completeReady() {
    std::uint64_t count;
    while (::read(wakeup, &count, sizeof(count)) > 0) {
    }
    std::deque<Completion> results;
    {
      std::lock_guard<std::mutex> lock(completionMutex);
      results.swap(completed);
    }
    for (auto &done : results) {
      --totalRequests;
      if (alive(done.id) && connections.at(done.id)->epoch == done.epoch) {
        auto &c = *connections.at(done.id);
        c.busy = false;
        if (done.result.authenticatedUser) {
          const auto uid = *done.result.authenticatedUser;
          if (c.user && c.user != uid && online.count(c.user) &&
              online.at(c.user) == c.id) {
            auto old = c.user;
            online.erase(old);
            announce(old, c.peers);
          }
          if (online.count(uid) && online.at(uid) != c.id &&
              alive(online.at(uid))) {
            auto &old = *connections.at(online.at(uid));
            old.user = 0;
            ++old.epoch;
            totalRequests -= old.requests.size();
            old.requests.clear();
            old.afterFlushClose = true;
            queue(old.id, event("session_replaced",
                                {{"message", "账号已在其他客户端登录"}}));
          }
          c.user = uid;
          ++c.epoch;
          c.peers = {done.peers.begin(), done.peers.end()};
          online[uid] = c.id;
          announce(uid, c.peers);
        }
        if (done.result.logout) {
          auto uid = c.user;
          if (uid && online.count(uid) && online.at(uid) == c.id)
            online.erase(uid);
          c.user = 0;
          ++c.epoch;
          announce(uid, c.peers);
          c.peers.clear();
        }
        auto &response = done.result.reply;
        if (response.value("ok", false)) {
          idle.touch(c.timeout);
          if (response.value("type", "") == "friend_list")
            for (auto &peer : response["data"]["friends"])
              peer["online"] =
                  online.count(peer["id"].get<std::int64_t>()) != 0;
          if (response.value("type", "") == "history")
            response["data"]["peer"]["online"] =
                online.count(
                    response["data"]["peer"]["id"].get<std::int64_t>()) != 0;
          if (response.value("type", "") == "conversations")
            for (auto &item : response["data"]["conversations"])
              item["peer"]["online"] =
                  online.count(item["peer"]["id"].get<std::int64_t>()) != 0;
        }
        queue(c.id, response);
        startNext(c.id);
        if (c.readClosed && !c.busy && c.requests.empty() && c.output.empty())
          deferClose(c.id);
      }
      // Committed operations survive a source disconnect; only replies bind to
      // its generation.
      for (auto &push : done.result.pushes) {
        auto target = online.find(push.userId);
        if (target == online.end() || !alive(target->second))
          continue;
        if (push.event.value("type", "") == "friends_changed") {
          auto peer = push.event["data"]["peer_id"].get<std::int64_t>();
          auto &recipient = *connections.at(target->second);
          // Result type distinguishes accepted/removed relationships without DB
          // on I/O thread.
          const auto type = done.result.reply.value("type", "");
          bool added =
              type == "friend_respond" &&
              done.result.reply["data"].value("status", "") == "accepted";
          if (added)
            recipient.peers.insert(peer);
          else
            recipient.peers.erase(peer);
        }
        queue(target->second, std::move(push.event));
      }
    }
  }
  void receiveReady(std::uint64_t id) {
    if (!alive(id))
      return;
    auto &c = *connections.at(id);
    char bytes[16384];
    std::size_t budget = 65536;
    while (budget && !c.readClosed && !c.afterFlushClose) {
      auto n = recv(c.fd, bytes, std::min(budget, sizeof(bytes)), 0);
      if (n == 0) {
        c.readClosed = true;
        interest(c);
        if (!c.busy && c.requests.empty() && c.output.empty())
          deferClose(id);
        return;
      }
      if (n < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return;
        deferClose(id);
        return;
      }
      budget -= static_cast<std::size_t>(n);
      try {
        for (const auto &payload : c.decoder.feed(
                 std::string_view(bytes, static_cast<std::size_t>(n)))) {
          auto request =
              Json::parse(payload, [](int depth, Json::parse_event_t, Json &) {
                if (depth > 32)
                  throw ProtocolError("JSON nesting exceeds limit");
                return true;
              });
          if (request.is_object() && request.contains("type") &&
              request["type"] == "ping") {
            auto response = service.execute({id, c.user}, request).reply;
            if (response.value("ok", false))
              idle.touch(c.timeout);
            queue(id, std::move(response));
          } else if (c.requests.size() >= options.maxPending ||
                     totalRequests >= options.maxInFlight) {
            Json safe = {
                {"type", request.is_object() &&
                                 request.value("type", Json()).is_string()
                             ? request["type"]
                             : Json("")},
                {"request_id",
                 request.is_object() &&
                         request.value("request_id", Json()).is_string()
                     ? request["request_id"]
                     : Json("")}};
            queue(id, errorReply(safe, "BUSY", "请求过多，请稍后重试"));
          } else {
            ++totalRequests;
            c.requests.push_back(std::move(request));
            startNext(id);
          }
          if (!alive(id))
            return;
        }
      } catch (...) {
        deferClose(id);
        return;
      }
    }
  }
  void sendReady(std::uint64_t id) {
    if (!alive(id))
      return;
    auto &c = *connections.at(id);
    std::size_t budget = 65536;
    while (c.offset < c.output.size() && budget) {
      auto n = send(c.fd, c.output.data() + c.offset,
                    std::min(budget, c.output.size() - c.offset), MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        deferClose(id);
        return;
      }
      if (n == 0)
        break;
      c.offset += static_cast<std::size_t>(n);
      budget -= static_cast<std::size_t>(n);
    }
    if (c.offset == c.output.size()) {
      c.output.clear();
      c.offset = 0;
      if (c.afterFlushClose ||
          (c.readClosed && !c.busy && c.requests.empty())) {
        deferClose(id);
        return;
      }
    }
    interest(c);
  }
  void run() {
    sockaddr_in address{};
    socklen_t size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) <
        0)
      throw std::runtime_error("cannot read listening address");
    std::cout << "LISTENING " << ntohs(address.sin_port) << std::endl;
    auto lastTick = Clock::now();
    epoll_event events[128];
    while (!stop.load()) {
      int count = epoll_wait(poller, events, 128, 250);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        throw std::runtime_error("epoll_wait failed");
      }
      for (int i = 0; i < count; ++i) {
        auto id = events[i].data.u64;
        auto flags = events[i].events;
        if (id == 1) {
          acceptReady();
          continue;
        }
        if (id == 2) {
          completeReady();
          continue;
        }
        if (flags & EPOLLERR) {
          deferClose(id);
          continue;
        }
        if (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))
          receiveReady(id);
        if (flags & EPOLLOUT)
          sendReady(id);
      }
      auto now = Clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(now - lastTick)
              .count();
      if (elapsed > 0) {
        idle.advance(static_cast<unsigned>(
            std::min<std::int64_t>(elapsed, options.heartbeatTimeout)));
        lastTick += std::chrono::seconds(elapsed);
      }
      for (const auto &item : connections)
        if (!item.second->user && now - item.second->created >=
                                      std::chrono::seconds(options.authTimeout))
          deferClose(item.first);
      drainCloses();
    }
    epoll_ctl(poller, EPOLL_CTL_DEL, listener, nullptr);
    ::close(listener);
    listener = -1;
    while (totalRequests) {
      workers.wait();
      completeReady();
      drainCloses();
    }
    database.drain();
    std::vector<std::uint64_t> ids;
    for (const auto &item : connections)
      ids.push_back(item.first);
    for (auto id : ids)
      sendReady(id);
    drainCloses();
  }
};
Reactor::Reactor(ServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
Reactor::~Reactor() = default;
void Reactor::run() { impl_->run(); }
void Reactor::requestStop() {
  impl_->stop.store(true);
  std::uint64_t one = 1;
  ::write(impl_->wakeup, &one, sizeof(one));
}
} // namespace qtchat
