#pragma once
// Copyright (c) 2010, Shuo Chen. All rights reserved.
// Adapted from muduo v2.0.3 examples/idleconnection/echo.h and echo.cc.
// BSD redistribution terms and disclaimer: third_party/muduo/License.
// QtChat changes: opaque connection callback, explicit cancellation and tick
// driver.
#include <functional>
#include <memory>

namespace qtchat {
// Adapter for muduo examples/idleconnection, not a new timeout algorithm.
class IdleTimeout {
public:
  struct Watch;
  using Handle = std::shared_ptr<Watch>;
  explicit IdleTimeout(unsigned seconds);
  ~IdleTimeout();
  Handle watch(std::function<void()> close);
  void touch(const Handle &handle);
  void advance(unsigned elapsedSeconds);
  void cancel(Handle &handle);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace qtchat
