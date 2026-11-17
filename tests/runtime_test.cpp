#include "BS_thread_pool.hpp"
#include "check.h"
#include "common/idle_timeout.h"
#include <atomic>
#include <vector>
using namespace qtchat;

int main() {
  Checks tests;
  tests.run("third party executor completes submitted work", [] {
    BS::thread_pool pool(2);
    check(pool.submit_task([] { return 42; }).get() == 42, "task result lost");
    std::atomic<int> count{0};
    for (int i = 0; i < 64; ++i)
      pool.detach_task([&] { ++count; });
    pool.wait();
    check(count == 64, "accepted work did not complete");
  });
  tests.run("single executor preserves database task order", [] {
    BS::thread_pool pool(1);
    std::vector<int> order;
    for (int i = 0; i < 32; ++i)
      pool.detach_task([&, i] { order.push_back(i); });
    pool.wait();
    for (int i = 0; i < 32; ++i)
      check(order.at(i) == i, "database task order changed");
  });
  tests.run("timeout expires once after sixty ticks", [] {
    int closed = 0;
    IdleTimeout timeout(60);
    auto handle = timeout.watch([&] { ++closed; });
    timeout.advance(59);
    check(closed == 0, "connection expired early");
    timeout.advance(1);
    check(closed == 1, "connection was not closed at expiry");
    timeout.advance(60);
    check(closed == 1, "connection closed twice");
  });
  tests.run("valid activity renews the connection", [] {
    int closed = 0;
    IdleTimeout timeout(60);
    auto handle = timeout.watch([&] { ++closed; });
    timeout.advance(30);
    timeout.touch(handle);
    timeout.advance(59);
    check(closed == 0, "renewal was ignored");
    timeout.advance(1);
    check(closed == 1, "renewed timeout did not expire");
  });
  tests.run("cancellation cannot close a later connection", [] {
    int oldClosed = 0, newClosed = 0;
    IdleTimeout timeout(60);
    auto old = timeout.watch([&] { ++oldClosed; });
    timeout.cancel(old);
    timeout.cancel(old);
    auto current = timeout.watch([&] { ++newClosed; });
    timeout.advance(60);
    check(oldClosed == 0, "cancelled close callback ran");
    check(newClosed == 1, "current connection not expired");
  });
  tests.run("suspended event loop expires without replaying every second", [] {
    int closed = 0;
    IdleTimeout timeout(60);
    auto handle = timeout.watch([&] { ++closed; });
    timeout.advance(3600);
    check(closed == 1, "suspend lost the timeout");
  });
  tests.run("destroying timeout owner does not invoke close callback", [] {
    int closed = 0;
    IdleTimeout::Handle handle;
    {
      IdleTimeout timeout(60);
      handle = timeout.watch([&] { ++closed; });
    }
    check(closed == 0, "owner destruction called into dead network owner");
  });
  return tests.result();
}
