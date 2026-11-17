#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

inline void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <class Exception, class Action> void throws(Action action) {
  try {
    action();
  } catch (const Exception &) {
    return;
  }
  throw std::runtime_error("expected exception was not thrown");
}

struct Checks {
  int failed = 0;
  int passed = 0;
  void run(const char *name, const std::function<void()> &test) {
    try {
      test();
      ++passed;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  int result() const {
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
  }
};
