#include "idle_timeout.h"
#include <algorithm>
#include <boost/circular_buffer.hpp>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

// Copyright (c) 2010, Shuo Chen. All rights reserved.
// Original algorithm: third_party/muduo/examples/idleconnection/echo.{h,cc}.
// BSD terms: third_party/muduo/License. See third_party/README.md for changes.
namespace qtchat {
struct IdleEntry;
struct IdleTimeout::Watch {
  std::function<void()> close;
  std::weak_ptr<IdleEntry> entry;
  bool active = true;
};

// muduo Entry: the buckets own entries; each entry weakly refers to a
// connection.
struct IdleEntry {
  explicit IdleEntry(const IdleTimeout::Handle &connection)
      : weakConn_(connection) {}
  ~IdleEntry() {
    auto connection = weakConn_.lock();
    if (connection && connection->active) {
      connection->active = false;
      try {
        connection->close();
      } catch (...) {
        std::cerr << "QtChat timeout close callback failed\n";
      }
    }
  }
  std::weak_ptr<IdleTimeout::Watch> weakConn_;
};

struct IdleTimeout::Impl {
  using EntryPtr = std::shared_ptr<IdleEntry>;
  using Bucket = std::unordered_set<EntryPtr>;
  using WeakConnectionList = boost::circular_buffer<Bucket>;
  explicit Impl(unsigned seconds) : connectionBuckets_(seconds) {
    if (!seconds)
      throw std::invalid_argument("timeout must be at least one second");
    connectionBuckets_.resize(seconds);
  }
  WeakConnectionList connectionBuckets_;
};

IdleTimeout::IdleTimeout(unsigned seconds)
    : impl_(std::make_unique<Impl>(seconds)) {}

IdleTimeout::~IdleTimeout() {
  // Explicit shutdown is an adapter responsibility, not a liveness expiry.
  for (auto &bucket : impl_->connectionBuckets_)
    for (const auto &entry : bucket)
      if (auto connection = entry->weakConn_.lock())
        connection->active = false;
}

IdleTimeout::Handle IdleTimeout::watch(std::function<void()> close) {
  if (!close)
    throw std::invalid_argument("timeout requires a close callback");
  auto connection = std::make_shared<Watch>();
  connection->close = std::move(close);
  auto entry = std::make_shared<IdleEntry>(connection);
  impl_->connectionBuckets_.back().insert(entry); // muduo onConnection
  connection->entry = entry;
  return connection;
}

void IdleTimeout::touch(const Handle &handle) {
  if (handle && handle->active)
    if (auto entry = handle->entry.lock())
      impl_->connectionBuckets_.back().insert(entry); // muduo onMessage
}

void IdleTimeout::advance(unsigned elapsedSeconds) {
  // After a whole wheel elapsed every old bucket expires. Bound suspend
  // catch-up.
  const auto ticks =
      std::min<std::size_t>(elapsedSeconds, impl_->connectionBuckets_.size());
  for (std::size_t i = 0; i < ticks; ++i)
    impl_->connectionBuckets_.push_back(Impl::Bucket()); // muduo onTimer
}

void IdleTimeout::cancel(Handle &handle) {
  if (handle)
    handle->active = false;
  handle.reset();
}
} // namespace qtchat
