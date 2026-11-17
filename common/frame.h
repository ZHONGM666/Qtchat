#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace qtchat {
inline constexpr std::size_t MaxFrameSize = 64 * 1024;

class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

std::string encodeFrame(std::string_view payload);

class FrameDecoder {
public:
  std::vector<std::string> feed(std::string_view bytes);
  void reset();

private:
  std::string buffer_;
};
} // namespace qtchat
